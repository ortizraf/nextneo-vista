"""Pre-renders the Garden ("Jardim") plants with depth (relief, light, gloss, contact
shadows) and writes them as C arrays for the firmware.

    <python with pillow + numpy> tools/garden_art.py [--preview out_dir]

Writes esp32-s3/src/modules/garden/garden_art.cpp. Each picture is a JPEG (colour)
plus an RLE 4-bit alpha mask, decoded on the board only when the plant changes stage.
Every primitive (petal, leaf, stem, sphere, ribbed body) is a height field: its normals
are lit by one light from the upper left, so the shapes read as round 3D objects.
"""
import io, math, os, sys
import numpy as np
from PIL import Image

S = 3                                   # supersampling
LIGHT = np.array([-0.45, -0.70, 0.56]); LIGHT /= np.linalg.norm(LIGHT)
HALF = LIGHT + np.array([0, 0, 1.0]); HALF /= np.linalg.norm(HALF)
OUT_W, OUT_H = 220, 186                  # plant picture; stem foot at (110, OUT_H - 1)


def hexrgb(c):
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], float) / 255


def mix(a, b, t):
    t = np.asarray(t, float)[..., None] if np.ndim(t) else t
    return a * (1 - t) + b * t


def box_blur(a, r):
    if r < 1: return a
    for ax in (0, 1):
        c = np.cumsum(np.pad(a, [(r + 1, r) if i == ax else (0, 0) for i in range(2)], mode="edge"), axis=ax)
        a = (np.take(c, range(2 * r + 1, c.shape[ax]), axis=ax) - np.take(c, range(0, c.shape[ax] - 2 * r - 1), axis=ax)) / (2 * r + 1)
    return a


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.W, self.H = w * S, h * S
        self.rgb = np.zeros((self.H, self.W, 3))   # premultiplied
        self.a = np.zeros((self.H, self.W))
        self.yy, self.xx = np.mgrid[0:self.H, 0:self.W].astype(float) / S

    # mask (0..1), height (px), albedo (H,W,3), gloss 0..1 -> lit colour, composited over.
    def put(self, mask, height, albedo, gloss=0.25, shadow=0.45, amb=0.42, emissive=0.0):
        if mask.max() <= 0: return
        gy, gx = np.gradient(height * S)                # height in px, per supersampled px
        n = np.stack([-gx, -gy, np.ones_like(gx)], -1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        dif = np.clip(n @ LIGHT, 0, 1)
        spec = np.clip(n @ HALF, 0, 1) ** 28 * gloss
        rim = (1 - n[..., 2]) ** 2 * 0.18               # soft back light on the edges
        col = albedo * (amb + 0.78 * dif + emissive)[..., None] + spec[..., None] + rim[..., None] * albedo
        col = np.clip(col, 0, 1)
        if shadow > 0:                                  # contact shadow on what is already drawn
            sh = box_blur(np.roll(np.roll(mask, int(2.5 * S), 0), int(1.5 * S), 1), int(2.5 * S)) * shadow
            k = 1 - sh * (1 - mask)
            self.rgb *= k[..., None]
        m = mask[..., None]
        self.rgb = col * m + self.rgb * (1 - m)
        self.a = mask + self.a * (1 - mask)

    def image(self):
        a = self.a.reshape(self.h, S, self.w, S).mean((1, 3))
        rgb = self.rgb.reshape(self.h, S, self.w, S, 3).mean((1, 3))
        col = np.where(a[..., None] > 1e-4, rgb / np.maximum(a[..., None], 1e-4), 0)
        return np.clip(col, 0, 1), np.clip(a, 0, 1)


def smooth_mask(d):
    """d = signed distance-ish (positive inside, in supersampled px) -> antialiased 0..1."""
    return np.clip(d + 0.5, 0, 1)


# ------------------------------------------------------------------ primitives
PROFILES = {
    "petal": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.72) ** 0.65,
    "round": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.55) ** 0.45,
    "leaf": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.85) * (1 - np.clip(u, 0, 1)) ** 0.15,
    "heart": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.62) * (1 - np.clip(u, 0, 1)) ** 0.25,
    "strap": lambda u: np.clip(u * 7, 0, 1) * (1 - np.clip(u, 0, 1) ** 3) ** 0.6,
    "spike": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.9) ** 1.2,
    "bract": lambda u: np.sin(np.pi * np.clip(u, 0, 1) ** 0.6) ** 0.5 * (1 - 0.25 * np.clip(u, 0, 1) ** 6),
}


def blade(cv, x, y, ang, L, Wd, c_base, c_tip, prof="leaf", bend=0.0, crease=0.35, dome=0.55,
          veins=0.0, ruffle=0.0, gloss=0.25, edge_dark=0.25, mid_light=0.15, shadow=0.45, droop=0.0):
    """Leaf / petal from base (x, y) pointing at `ang` degrees (0 = up, clockwise)."""
    t = math.radians(ang)
    ax_, ay_ = math.sin(t), -math.cos(t)
    nx_, ny_ = math.cos(t), math.sin(t)
    dx, dy = cv.xx - x, cv.yy - y
    tt = dx * ax_ + dy * ay_
    ss = dx * nx_ + dy * ny_
    u = tt / L
    uc = np.clip(u, 0, 1)
    ss = ss - bend * L * uc ** 2                        # curved midrib
    hw = Wd * PROFILES[prof](u) * (1 + ruffle * np.sin(uc * 31 + ss * 0.9) * uc)
    hw = np.maximum(hw, 1e-3)
    v = ss / hw
    inside = np.minimum(np.minimum(u * L, (1 - u) * L), (1 - np.abs(v)) * hw) * S
    mask = smooth_mask(inside) * ((u > -0.02) & (u < 1.02))
    vc = np.clip(np.abs(v), 0, 1)
    h = np.sqrt(1 - vc ** 2) * hw * dome
    h -= crease * hw * np.exp(-(v / 0.10) ** 2) * np.sin(np.pi * uc) * 0.5
    h += droop * L * (uc - uc ** 2)
    if veins:
        h -= veins * hw * 0.08 * np.clip(np.cos((uc * 7 - vc * 2.2) * np.pi), 0, 1) ** 6
    alb = mix(hexrgb(c_base), hexrgb(c_tip), uc ** 0.8)
    alb = alb * (1 - edge_dark * vc[..., None] ** 3) + mid_light * np.exp(-(v / 0.07) ** 2)[..., None] * (uc < 0.9)[..., None]
    cv.put(mask, h, np.clip(alb, 0, 1), gloss=gloss, shadow=shadow)


def bezier(p, n=40):
    p = np.array(p, float)
    t = np.linspace(0, 1, n)[:, None]
    if len(p) == 3:
        return (1 - t) ** 2 * p[0] + 2 * (1 - t) * t * p[1] + t ** 2 * p[2]
    return (1 - t) ** 3 * p[0] + 3 * (1 - t) ** 2 * t * p[1] + 3 * (1 - t) * t ** 2 * p[2] + t ** 3 * p[3]


def tube(cv, pts, w0, w1, c_dark, c_light, gloss=0.15, shadow=0.35):
    """Stem: round tube along the points, width w0 at the start to w1 at the end."""
    pts = np.asarray(pts, float)
    x0, y0 = pts.min(0) - max(w0, w1) - 2
    x1, y1 = pts.max(0) + max(w0, w1) + 2
    sl = (slice(max(0, int(y0 * S)), min(cv.H, int(y1 * S) + 1)), slice(max(0, int(x0 * S)), min(cv.W, int(x1 * S) + 1)))
    X, Y = cv.xx[sl], cv.yy[sl]
    best = np.full(X.shape, 1e9); bt = np.zeros(X.shape)
    n = len(pts) - 1
    for i in range(n):
        a, b = pts[i], pts[i + 1]
        d = b - a; l2 = max(d @ d, 1e-6)
        tt = np.clip(((X - a[0]) * d[0] + (Y - a[1]) * d[1]) / l2, 0, 1)
        dist = np.hypot(X - a[0] - tt * d[0], Y - a[1] - tt * d[1])
        sel = dist < best
        best = np.where(sel, dist, best); bt = np.where(sel, (i + tt) / n, bt)
    r = (w0 + (w1 - w0) * bt) / 2
    mask = np.zeros((cv.H, cv.W)); h = np.zeros((cv.H, cv.W)); alb = np.zeros((cv.H, cv.W, 3))
    mask[sl] = smooth_mask((r - best) * S)
    h[sl] = np.sqrt(np.clip(r ** 2 - best ** 2, 0, None))
    alb[sl] = mix(hexrgb(c_dark), hexrgb(c_light), 0.35 + 0.3 * bt)
    cv.put(mask, h, alb, gloss=gloss, shadow=shadow)


def ball(cv, x, y, r, c, c2=None, gloss=0.5, ry=None, tex=None, shadow=0.45, amb=0.42):
    ry = ry or r
    d = np.sqrt(((cv.xx - x) / r) ** 2 + ((cv.yy - y) / ry) ** 2)
    mask = smooth_mask((1 - d) * min(r, ry) * S)
    h = np.sqrt(np.clip(1 - d ** 2, 0, 1)) * min(r, ry)
    alb = np.broadcast_to(hexrgb(c), mask.shape + (3,)).copy()
    if c2 is not None: alb = mix(hexrgb(c), hexrgb(c2), np.clip((cv.yy - (y - ry)) / (2 * ry), 0, 1))
    if tex is not None: alb = tex(alb, d)
    cv.put(mask, h, alb, gloss=gloss, shadow=shadow, amb=amb)


def ribbed(cv, x, ytop, ybot, r, c, c2, ribs=7, arm=None):
    """Cactus column: capsule with vertical ribs and spines."""
    X, Y = cv.xx - x, cv.yy
    yc = np.clip(Y, ytop + r, ybot)
    d = np.sqrt(X ** 2 + (Y - yc) ** 2) / r
    mask = smooth_mask((1 - d) * r * S) * (Y <= ybot + 0.5)
    ang = np.arcsin(np.clip(X / r, -1, 1))
    rib = np.cos(ang * ribs) * 0.5 + 0.5
    h = np.sqrt(np.clip(1 - d ** 2, 0, 1)) * r * (0.82 + 0.18 * rib)
    alb = mix(hexrgb(c2), hexrgb(c), 0.35 + 0.65 * rib)
    alb = mix(alb, hexrgb(0xC8E8A0), np.clip((ytop + r - Y) / r, 0, 1) * 0.25)
    cv.put(mask, h, alb, gloss=0.18)
    for k in range(-ribs // 2 + 1, ribs // 2 + 1):     # spines on the rib crests (visible side)
        a = k * math.pi / ribs * 2 / 2
        if abs(a) > 1.2: continue
        sx = x + math.sin(a) * r * 0.95
        for yy in np.arange(ytop + r * 0.5, ybot - 2, 7.0):
            yy2 = yy + (k % 2) * 3.5
            ball(cv, sx, yy2, 1.3, 0xF4F0D8, gloss=0.1, shadow=0.25, amb=0.9)


# ------------------------------------------------------------------ plant parts
GREEN = (0x2E6A1E, 0x7CBF4E)
GREEN_DARK = (0x1E4E16, 0x5A9A3A)


def leaf_pair(cv, x, y, L, Wd, lift, prof="leaf", cols=GREEN, veins=1.0, gloss=0.25):
    blade(cv, x, y, -90 + lift, L, Wd, cols[0], cols[1], prof, bend=-0.12, veins=veins, gloss=gloss, droop=0.15)
    blade(cv, x, y, 90 - lift, L, Wd, cols[0], cols[1], prof, bend=0.12, veins=veins, gloss=gloss, droop=0.15)


def stem_curve(x0, y0, x1, y1, sway=0.0):
    return bezier([(x0, y0), (x0 + sway, (y0 + y1) / 2), (x1, y1)])


def bud(cv, x, y, r, col, open_=0.3, green=0x4E8A2E):
    ball(cv, x, y, r * 0.8, mix_hex(col, 0x205010, 0.25), ry=r * 1.15, gloss=0.35)
    for a in (-35, 0, 35):                              # sepals hugging the bud
        blade(cv, x, y + r * 0.9, a, r * (1.5 - open_ * 0.6), r * 0.42, green, 0x7AB84A, "leaf", crease=0.2)


def mix_hex(a, b, t):
    ca, cb = hexrgb(a), hexrgb(b)
    c = ca * (1 - t) + cb * t
    return int(c[0] * 255) << 16 | int(c[1] * 255) << 8 | int(c[2] * 255)


def daisy(cv, x, y, n, L, Wd, c1, c2, centre, cr, prof="petal", phase=0.0, layers=2, seeds=False, ruffle=0.0):
    for layer in range(layers):
        k = 0.92 if layer == 0 and layers > 1 else 1.0
        dark = 0.25 if layer == 0 and layers > 1 else 0.0
        for i in range(n):
            a = phase + (i + 0.5 * layer) * 360 / n
            blade(cv, x, y, a, L * k, Wd, mix_hex(c2, 0x000000, dark), mix_hex(c1, 0x000000, dark), prof,
                  crease=0.45, dome=0.35, ruffle=ruffle, gloss=0.2, shadow=0.35, mid_light=0.05, edge_dark=0.15)
    tex = None
    if seeds:
        pts = [(math.sqrt(i / 260) * cr * 0.97, i * 2.39996) for i in range(260)]
        px = np.array([x + r * math.cos(t) for r, t in pts]); py = np.array([y + r * math.sin(t) for r, t in pts])
        def tex(alb, d, px=px, py=py):
            sub = (np.abs(cv.xx - x) < cr + 1) & (np.abs(cv.yy - y) < cr + 1)
            m = np.full(cv.xx.shape, 9.0)
            xs, ys = cv.xx[sub], cv.yy[sub]
            dd = np.min((xs[:, None] - px[None]) ** 2 + (ys[:, None] - py[None]) ** 2, axis=1)
            m[sub] = dd
            dots = np.exp(-m / 0.9)
            return mix(alb, hexrgb(0xA07A3A), dots * 0.55 * (d < 0.85))
    ball(cv, x, y, cr, centre, mix_hex(centre, 0x000000, 0.4), gloss=0.12, tex=tex, shadow=0.5)


# ------------------------------------------------------------------ species
FOOT = (OUT_W / 2, OUT_H - 1)


def seed_img():
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    ball(cv, x, y - 2, 22, 0x5A3A22, 0x2A180C, ry=7, gloss=0.05)          # little mound
    ball(cv, x + 2, y - 8, 6, 0xD8B880, 0x8A6A40, ry=4, gloss=0.4)        # seed
    return cv


def sprout_img():
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    ball(cv, x, y - 1, 18, 0x5A3A22, 0x2A180C, ry=5, gloss=0.05)
    tube(cv, stem_curve(x, y - 2, x + 2, y - 34, 4), 4, 3, 0x4E8A2E, 0xA6D86A)
    blade(cv, x + 2, y - 33, -70, 22, 9, 0x5A9A34, 0x9AD45E, "round", bend=-0.2, droop=0.2, gloss=0.3)
    blade(cv, x + 2, y - 33, 70, 22, 9, 0x5A9A34, 0x9AD45E, "round", bend=0.2, droop=0.2, gloss=0.3)
    blade(cv, x + 2, y - 34, 4, 10, 4, 0x4E8A2E, 0x8ACB52, "leaf")
    return cv


def leafy(cv, h, pairs, L, Wd, prof="heart", w=8, woody=False, cols=GREEN, sway=6, gloss=0.25):
    x, y = FOOT
    top = (x + sway * 0.4, y - h)
    pts = stem_curve(x, y, top[0], top[1], sway)
    tube(cv, pts, w, w * 0.6, 0x5A3A22 if woody else 0x3E7A26, 0x9A7050 if woody else 0x9ACB5A)
    n = pairs * 2 - 1                                     # alternate leaves, big at the bottom
    for k in range(n):
        t = 0.12 + k * (0.8 / n)
        px, py = pts[int(t * (len(pts) - 1))]
        s = 1.12 - k * 0.09; side = 1 if k % 2 else -1
        blade(cv, px, py, side * (68 - k * 3), L * s, Wd * s, cols[0], cols[1], prof, bend=side * 0.18,
              veins=1.0, gloss=gloss, droop=0.25)
    return top


def sunflower(stage, sp):
    cv = Canvas(OUT_W, OUT_H)
    h = {2: 70, 3: 118, 4: 132, 5: 116}[stage]
    top = leafy(cv, h, {2: 2, 3: 3, 4: 3, 5: 3}[stage], 50, 21, "heart", w=9)
    if stage == 4: bud(cv, top[0], top[1] - 6, 11, 0x8ABF4A)
    if stage == 5:
        daisy(cv, top[0], top[1] - 30, 18, 38, 8.5, sp[0], sp[1], 0x5A3414, 25, seeds=True, phase=6)
    return cv


def hibiscus(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    h = {2: 72, 3: 118, 4: 128, 5: 128}[stage]
    top = leafy(cv, h, 2 if stage == 2 else 3, 36, 17, "leaf", w=7, woody=True, cols=GREEN_DARK, gloss=0.55)
    if stage >= 3:                                       # side branches
        for s, (bx, by) in ((-1, (x - 48, y - h * 0.62)), (1, (x + 50, y - h * 0.7))):
            tube(cv, bezier([(x + s * 3, y - h * 0.35), (x + s * 20, y - h * 0.55), (bx, by)]), 5, 3.5, 0x5A3A22, 0x9A7050)
            blade(cv, bx, by, s * 50, 34, 15, *GREEN_DARK, "leaf", bend=s * 0.15, veins=1, gloss=0.55, droop=0.2)
            blade(cv, bx, by, s * 120, 30, 13, *GREEN_DARK, "leaf", bend=-s * 0.1, veins=1, gloss=0.55, droop=0.2)
    spots = [(top[0], top[1] - 6, 1.0), (x - 50, y - h * 0.66, 0.78), (x + 52, y - h * 0.74, 0.8)]
    if stage == 4:
        for fx, fy, k in spots: bud(cv, fx, fy - 4, 8 * k, sp[1])
    if stage == 5:
        for fx, fy, k in spots:
            daisy(cv, fx, fy - 6, 5, 27 * k, 16 * k, sp[0], sp[1], 0x7A0A1A, 5 * k, prof="round", layers=1,
                  phase=10, ruffle=0.05)
            tube(cv, [(fx, fy - 6), (fx + 6 * k, fy - 18 * k), (fx + 9 * k, fy - 26 * k)], 2.2, 1.6, 0xD8A030, 0xF6D24C)
            ball(cv, fx + 9 * k, fy - 27 * k, 3 * k, sp[2], gloss=0.4)
    return cv


def lavender(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    n = {2: 9, 3: 13, 4: 13, 5: 13}[stage]
    for i in range(n):                                   # grey-green narrow leaves
        a = -70 + i * 140 / (n - 1)
        blade(cv, x + a * 0.08, y, a, 34 + (i % 3) * 6, 3.4, 0x4E6E48, 0xA6C49A, "strap", bend=0.25 * np.sign(a),
              crease=0.3, gloss=0.1, shadow=0.3)
    if stage < 3: return cv
    for i in range(7):
        a = (i - 3) * 9
        L = 150 - abs(i - 3) * 12 if stage >= 4 else 95
        tx, ty = x + math.sin(math.radians(a)) * L, y - math.cos(math.radians(a)) * L
        tube(cv, bezier([(x, y - 4), (x + (tx - x) * 0.4, y - L * 0.5), (tx, ty)]), 2.6, 2.0, 0x4E6E3A, 0x9AB87A)
        if stage < 4: continue
        florets = 13 if stage == 5 else 8
        for k in range(florets):
            fy = ty + k * 3.4; fx = tx + (k * 0.12) * math.sin(math.radians(a)) * 3 + (1.6 if k % 2 else -1.6)
            c = sp[0] if stage == 5 else 0x7A8A6A
            ball(cv, fx, fy, 3.2 - k * 0.06, c if k % 2 else sp[1], gloss=0.35, shadow=0.3)
    return cv


def orchid(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    for a, L, b in ((-62, 72, -0.3), (58, 68, 0.32), (-22, 52, -0.15)):  # broad glossy leaves
        blade(cv, x, y - 3, a, L, 15, 0x1E5418, 0x6AAE4A, "strap", bend=b, droop=0.25, gloss=0.6, crease=0.5)
    if stage < 3: return cv
    spike = bezier([(x - 4, y - 8), (x - 6, y - 120), (x + 40, y - 170), (x + 78, y - 150)], 50)
    tube(cv, spike, 3, 2.2, 0x3E5A26, 0x8A9A52)
    if stage == 3: return cv
    for k, t in enumerate((0.55, 0.68, 0.8, 0.92)):
        fx, fy = spike[int(t * (len(spike) - 1))]
        fy += 16
        if stage == 4:
            ball(cv, fx, fy - 8, 5 + k * 0.4, mix_hex(sp[0], 0x6A9A40, 0.4), ry=6, gloss=0.4)
            continue
        s = 0.9 + 0.08 * k
        for a in (0, 125, -125):                              # sepals
            blade(cv, fx, fy, a, 17 * s, 6.5 * s, sp[1], sp[0], "petal", crease=0.3, dome=0.3)
        for a in (68, -68):                                   # broad petals
            blade(cv, fx, fy, a, 18 * s, 11 * s, sp[1], sp[0], "round", crease=0.2, dome=0.35, veins=0.6)
        blade(cv, fx, fy + 1, 180, 12 * s, 6 * s, 0x9A1E5A, 0xD24A8A, "round", crease=0.6)   # lip
        ball(cv, fx, fy - 1, 2.6 * s, sp[2], gloss=0.5)
    return cv


def cactus(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    if stage == 2:
        ribbed(cv, x, y - 52, y, 18, 0x6FBE60, 0x1F5A28); return cv
    hgt = 108 if stage == 3 else 122
    ribbed(cv, x + 32, y - hgt * 0.72, y - hgt * 0.32, 10, 0x6FBE60, 0x1F5A28, ribs=5)     # arm (behind)
    tube(cv, [(x + 14, y - hgt * 0.32), (x + 30, y - hgt * 0.32)], 14, 14, 0x2F7A34, 0x6FBE60)
    ribbed(cv, x, y - hgt, y, 23, 0x6FBE60, 0x1F5A28)
    if stage == 4:
        for dx in (-9, 6): ball(cv, x + dx, y - hgt - 4, 5, sp[1], 0x6A9A40, ry=7, gloss=0.4)
    if stage == 5:
        for fx, fs in ((x - 8, 1.0), (x + 34, 0.7)):
            fy = y - hgt - 6 if fs == 1.0 else y - hgt * 0.72 - 6
            daisy(cv, fx, fy, 14, 15 * fs, 3.2 * fs, sp[0], sp[1], sp[2], 4 * fs, prof="spike", layers=2)
    return cv


def tulip(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    for a, L, b in ((-28, 74, -0.25), (24, 80, 0.22), (-8, 60, -0.08)):
        blade(cv, x, y - 2, a, L if stage > 2 else L * 0.7, 13, 0x2A5E28, 0x8AC27A, "strap", bend=b,
              crease=0.55, gloss=0.35, droop=0.1)
    if stage < 3: return cv
    top = (x + 4, y - (110 if stage == 3 else 132))
    tube(cv, stem_curve(x, y - 4, top[0], top[1], 5), 5, 4.2, 0x3E7A2E, 0x9AC872)
    if stage == 3:
        ball(cv, top[0], top[1] - 12, 9, 0x7AB84A, 0x3E7A2A, ry=15, gloss=0.4); return cv
    col0, col1 = (sp[0], sp[1]) if stage == 5 else (mix_hex(sp[0], 0x7AB84A, 0.55), mix_hex(sp[1], 0x3E7A2A, 0.5))
    open_ = 1.0 if stage == 5 else 0.0
    fx, fy = top[0], top[1] + 2
    for a in (-14 - 10 * open_, 14 + 10 * open_, 0):       # back petals then the front one
        blade(cv, fx, fy, a, 42 if stage == 5 else 32, 15, col1, col0, "petal", crease=0.2, dome=0.8,
              gloss=0.45, edge_dark=0.35)
    blade(cv, fx - 3, fy + 1, -6, 40 if stage == 5 else 31, 14, mix_hex(col1, 0xFFFFFF, 0.1), col0, "petal",
          crease=0.1, dome=0.9, gloss=0.5)
    return cv


def bougainvillea(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    h = {2: 70, 3: 112, 4: 124, 5: 124}[stage]
    top = leafy(cv, h, 2 if stage == 2 else 3, 30, 14, "leaf", w=7, woody=True, gloss=0.35)
    if stage < 3: return cv
    tips = []
    for s, L in ((-1, 70), (1, 74), (-1, 40), (1, 42)):
        sx, sy = x + s * 4, y - h * (0.45 if L > 50 else 0.8)
        ex, ey = sx + s * L, sy - L * 0.55
        tube(cv, bezier([(sx, sy), (sx + s * L * 0.5, sy - L * 0.6), (ex, ey)]), 4, 2.5, 0x5A3A22, 0x9A7050)
        blade(cv, ex, ey, s * 60, 22, 10, *GREEN, "leaf", veins=1, gloss=0.35)
        tips.append((ex, ey)); tips.append(((sx + ex) / 2 + s * 4, (sy + ey) / 2 - 10))
    tips.append((top[0], top[1] - 4))
    if stage == 3: return cv
    rng = np.random.default_rng(7)
    for (tx, ty) in tips:
        for k in range(3 if stage == 5 else 2):
            cx, cy = tx + rng.uniform(-10, 10), ty + rng.uniform(-8, 6)
            if stage == 4:
                ball(cv, cx, cy, 3.5, sp[1], gloss=0.3); continue
            ph = rng.uniform(0, 120)
            for a in (ph, ph + 120, ph + 240):
                blade(cv, cx, cy, a, 11, 7.5, sp[1], sp[0], "bract", crease=0.3, dome=0.25, veins=0.8, gloss=0.2)
            ball(cv, cx, cy, 1.8, sp[2], gloss=0.3)
    return cv


def tomato(stage, sp):
    cv = Canvas(OUT_W, OUT_H); x, y = FOOT
    h = {2: 70, 3: 118, 4: 132, 5: 132}[stage]
    pts = stem_curve(x, y, x + 3, y - h, 6)
    tube(cv, pts, 7, 4.5, 0x3E7A26, 0x9ACB5A)
    arms = [(0.3, -1), (0.45, 1), (0.62, -1), (0.78, 1), (0.92, -1)][:2 if stage == 2 else 5]
    fruit_spots = []
    for t, s in arms:                                     # compound leaves: stalk + serrated leaflets
        px, py = pts[int(t * (len(pts) - 1))]
        ex, ey = px + s * 46, py - 18
        tube(cv, bezier([(px, py), (px + s * 22, py - 16), (ex, ey)]), 3, 2, 0x3E7A26, 0x9ACB5A)
        for k, f in enumerate((0.4, 0.7, 1.0)):
            lx, ly = px + (ex - px) * f, py + (ey - py) * f - 6 * f
            blade(cv, lx, ly, s * (40 + k * 20), 17, 7.5, *GREEN, "leaf", ruffle=0.12, veins=1, bend=s * 0.1)
            blade(cv, lx, ly, s * (140 - k * 12), 15, 6.5, *GREEN, "leaf", ruffle=0.12, veins=1, bend=-s * 0.1)
        fruit_spots.append((px + s * 16, py + 14))
    if stage == 4:
        for fx, fy in fruit_spots[1:]:
            daisy(cv, fx, fy, 5, 7, 2.6, 0xF7D23A, 0xD8A818, 0xC8A020, 2, prof="spike", layers=1)
    if stage == 5:
        for i, (fx, fy) in enumerate(fruit_spots[1:]):
            c = (0x8AC04A, 0x4E8A2A) if i == 2 else (sp[0], sp[1])
            ball(cv, fx, fy + 6, 11, c[0], c[1], ry=10, gloss=0.9, shadow=0.5)
            for a in (-60, 0, 60, 120, 180, 240):
                blade(cv, fx, fy - 3, a, 6, 2, 0x2E6A1E, 0x5A9A3A, "leaf", shadow=0.2)
    return cv


SPECIES = [sunflower, hibiscus, lavender, orchid, cactus, tulip, bougainvillea, tomato]
# petal light, petal shaded, centre (same order as kSpecies in garden_model.cpp)
COLORS = [(0xFFC21E, 0xD9820C, 0x4A2A10), (0xF0384A, 0xA3162A, 0xF6D24C), (0xA88BE6, 0x6448B4, 0x4E3A8C),
          (0xF58AC0, 0xC2508E, 0xF7E27A), (0xFF6FA6, 0xD23A72, 0xFFE07A), (0xFF6A3D, 0xC23A1A, 0x3A2A10),
          (0xE447B4, 0x9C1E7C, 0xFFF3C4), (0xEE4A30, 0xA82A18, 0xF7D23A)]


def pot_img():
    w, h = 150, 92
    cv = Canvas(w, h); cx = w / 2
    X, Y = cv.xx - cx, cv.yy
    ball(cv, cx, h - 7, 66, 0x000000, ry=7, gloss=0, shadow=0, amb=0.0)            # ground shadow
    cv.a *= 0.45; cv.rgb *= 0.45
    top, bot, rt, rb = 18, h - 8, 54, 42                                           # tapered body
    r = rt + (rb - rt) * np.clip((Y - top) / (bot - top), 0, 1)
    d = np.abs(X) / r
    mask = smooth_mask((1 - d) * r * S) * (Y >= top) * smooth_mask((bot - Y) * S + 0.5 * S)
    hgt = np.sqrt(np.clip(1 - d ** 2, 0, 1)) * r * 0.6
    alb = mix(hexrgb(0xC8703F), hexrgb(0x8E4424), np.clip((Y - top) / (bot - top), 0, 1) * 0.5)
    alb = alb * (1 + 0.04 * np.sin(Y * 1.7 + X * 0.3))[..., None]                 # clay texture
    cv.put(mask, hgt, np.clip(alb, 0, 1), gloss=0.12)
    tube(cv, [(cx - 58, 18), (cx + 58, 18)], 15, 15, 0xB05A30, 0xE89A68, gloss=0.2, shadow=0.5)   # rim
    ball(cv, cx, 15, 50, 0x3A2412, 0x1E1008, ry=7.5, gloss=0.05, shadow=0.0)      # soil
    rng = np.random.default_rng(3)
    for _ in range(40):
        ball(cv, cx + rng.uniform(-44, 44), 15 + rng.uniform(-5, 5), rng.uniform(0.8, 1.8), 0x6A4A2A, gloss=0.1,
             shadow=0.2)
    return cv


def butterfly_img(open_):
    w, h = 34, 28
    cv = Canvas(w, h); cx, cy = w / 2, h / 2 + 1
    k = 1.0 if open_ else 0.45
    for s in (-1, 1):
        blade(cv, cx, cy, s * 62, 15 * k + 2, 8, 0x1A1A2A, 0xF29A2E, "round", crease=0.1, dome=0.15, shadow=0.2,
              edge_dark=0.6, gloss=0.3)
        blade(cv, cx, cy + 1, s * 128, 11 * k + 2, 6, 0x1A1A2A, 0xF2B84E, "round", crease=0.1, dome=0.15,
              shadow=0.2, edge_dark=0.6)
    tube(cv, [(cx, cy - 7), (cx, cy + 7)], 2.6, 1.8, 0x1A1410, 0x4A3A30, shadow=0.1)
    return cv


BACK_W, BACK_H = 480, 150             # backdrop strip: tree line + hedge, placed at y = 82 on the screen


def backdrop_img():
    """Out-of-focus garden behind the pot: two layers of leafy tree crowns (many lit leaf
    clumps, far layer hazy and blue-ish), thin trunks, a clipped hedge and flower bokeh,
    blurred like a photo with shallow depth of field."""
    w, h = BACK_W, BACK_H
    rng = np.random.default_rng(11)

    def crown(cv, cx, cy, rx, ry, cols, n, rmin, rmax):
        for _ in range(n):                       # clumps: inside an ellipse, lit ones on top
            t = rng.uniform(0, 2 * np.pi); u = np.sqrt(rng.uniform(0, 1))
            x, y = cx + np.cos(t) * u * rx, cy + np.sin(t) * u * ry
            k = np.clip((y - (cy - ry)) / (2 * ry), 0, 1)
            c = cols[min(len(cols) - 1, int(k * len(cols) + rng.uniform(-0.6, 0.6)))]
            ball(cv, x, y, rng.uniform(rmin, rmax), c, gloss=0.08, shadow=0.35, amb=0.35)

    far = Canvas(w, h)
    for cx in range(-20, w + 40, 58):          # far row: hazy, cool
        cx += rng.uniform(-14, 14); cy = rng.uniform(60, 74)
        crown(far, cx, cy, rng.uniform(40, 52), rng.uniform(34, 44), [0x8AB27A, 0x6E9E64, 0x588A52, 0x467646], 90, 4, 8)
    farI, farA = far.image()

    near = Canvas(w, h)
    for cx, cy, rx, ry in ((40, 70, 56, 46), (175, 62, 50, 44), (305, 76, 46, 40), (432, 66, 58, 46)):
        tube(near, [(cx + 4, h), (cx + 2, cy + ry * 0.4), (cx - 6, cy)], 7, 4, 0x2A2016, 0x5A4632, shadow=0.2)
        crown(near, cx, cy, rx, ry, [0x7CAE50, 0x5A963C, 0x427A32, 0x2E6028, 0x1E441C], 260, 3.5, 8)
    for x in range(-10, w + 20, 18):           # hedge: rounded clipped bushes
        crown(near, x, h - 18, 16, 14, [0x5E9A40, 0x3E7A30, 0x2A5A24, 0x1E441C], 14, 3, 6)
    nearI, nearA = near.image()

    blur = lambda a, r: box_blur(box_blur(a, r), r)
    haze = hexrgb(0xBFD6E4)
    farI = mix(farI, haze, 0.22)
    fa = blur(farA, 4); fc = np.dstack([blur(farI[..., i] * farA, 4) for i in range(3)])
    na = blur(nearA, 2); nc = np.dstack([blur(nearI[..., i] * nearA, 2) for i in range(3)])
    a = na + fa * (1 - na)                      # near over far (premultiplied)
    col = nc + fc * (1 - na[..., None])
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    ground = np.clip((yy - (h - 22)) / 6, 0, 1)  # bottom rows solid lawn-dark, to meet the lawn
    col = col + (hexrgb(0x2E5A2C) * ground[..., None]) * (1 - a[..., None]); a = a + ground * (1 - a)
    for i in range(26):                         # bokeh flowers in the hedge
        bx, by = rng.uniform(0, w), rng.uniform(h - 30, h - 6); r = rng.uniform(2.5, 5.5)
        c = hexrgb([0xF27AA0, 0xF6C45A, 0xB49AF0, 0xFFFFFF, 0xF05A5A][i % 5])
        m = np.clip(r + 0.8 - np.hypot(xx - bx, yy - by), 0, 1)[..., None] * 0.6
        col = col * (1 - m) + c * m * a[..., None]
    rgb = np.where(a[..., None] > 1e-4, col / np.maximum(a[..., None], 1e-4), 0)
    cv = Canvas.__new__(Canvas); cv.w, cv.h = w, h
    cv.image = lambda: (np.clip(rgb, 0, 1), np.clip(a, 0, 1))
    return cv


# ------------------------------------------------------------------ export
def encode(cv, name, crop=True):
    col, a = cv.image()
    if crop:                                             # trim empty margins (keeps the foot position known)
        ys, xs = np.nonzero(a > 0.02)
        x0, x1, y0 = max(0, xs.min() - 1), min(cv.w, xs.max() + 2), max(0, ys.min() - 1)
        y1 = cv.h
    else:
        x0, x1, y0, y1 = 0, cv.w, 0, cv.h
    col, a = col[y0:y1, x0:x1], a[y0:y1, x0:x1]
    # bleed colours into transparent pixels so JPEG edges carry no dark fringe
    pm = col * a[..., None]; aa = a.copy()
    for r in (1, 2, 4, 8, 16):
        pm = np.where(aa[..., None] > 0.02, pm, box_blur_rgb(pm, r)); aa = np.where(aa > 0.02, aa, box_blur(aa, r))
    fill = np.where(aa[..., None] > 1e-4, pm / np.maximum(aa[..., None], 1e-4), 0)
    out = np.where(a[..., None] > 0.5, col, fill)
    img = Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
    buf = io.BytesIO(); img.save(buf, "JPEG", quality=88, subsampling=0, optimize=True)
    q = np.round(a * 15).astype(np.uint8).ravel()
    rle = bytearray(); i = 0
    while i < len(q):
        v = q[i]; n = 1
        while i + n < len(q) and q[i + n] == v and n < 271: n += 1
        if n <= 15: rle.append(v << 4 | n)
        else: rle += bytes((v << 4, n - 16))
        i += n
    return dict(name=name, w=x1 - x0, h=y1 - y0, ox=x0, jpg=buf.getvalue(), rle=bytes(rle),
                preview=(np.dstack([col, a]) * 255).astype(np.uint8))


def box_blur_rgb(a, r):
    return np.dstack([box_blur(a[..., i], r) for i in range(3)])


def carray(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    pics = [encode(seed_img(), "seed"), encode(sprout_img(), "sprout")]
    for i, f in enumerate(SPECIES):
        for stage in (2, 3, 4, 5):
            pics.append(encode(f(stage, COLORS[i]), f"p{i}_{stage}"))
            print(f"  {f.__name__} {stage}", flush=True)
    pics.append(encode(pot_img(), "pot", crop=False))
    pics.append(encode(butterfly_img(True), "fly_open", crop=False))
    pics.append(encode(butterfly_img(False), "fly_closed", crop=False))
    pics.append(encode(backdrop_img(), "backdrop", crop=False))
    if preview:
        os.makedirs(preview, exist_ok=True)
        for p in pics: Image.fromarray(p["preview"], "RGBA").save(os.path.join(preview, p["name"] + ".png"))
    body = ["// Generated by tools/garden_art.py - do not edit. Pre-rendered Garden plants:",
            "// JPEG colour + RLE 4-bit alpha (byte = alpha<<4 | run 1..15; run 0 -> next byte + 16).",
            '#include "modules/garden/garden_art.h"', ""]
    for p in pics:
        body.append(carray(f"j_{p['name']}", p["jpg"])); body.append(carray(f"a_{p['name']}", p["rle"]))
    def ent(p): return f"  {{{p['w']}, {p['h']}, {p['ox']}, j_{p['name']}, {len(p['jpg'])}, a_{p['name']}, {len(p['rle'])}}},"
    body.append("const GardenArt kGardenSeed = " + ent(pics[0])[2:-1] + ";")
    body.append("const GardenArt kGardenSprout = " + ent(pics[1])[2:-1] + ";")
    body.append("const GardenArt kGardenPlant[8][4] = {")
    for i in range(8):
        body.append("  {"); body += ["  " + ent(p) for p in pics[2 + i * 4: 6 + i * 4]]; body.append("  },")
    body.append("};")
    body.append("const GardenArt kGardenPot = " + ent(pics[-4])[2:-1] + ";")
    body.append("const GardenArt kGardenFly[2] = {" + ent(pics[-3])[2:-1] + ", " + ent(pics[-2])[2:-1] + "};")
    body.append("const GardenArt kGardenBackdrop = " + ent(pics[-1])[2:-1] + ";")
    out = os.path.join(root, "esp32-s3/src/modules/garden/garden_art.cpp")
    open(out, "w").write("\n".join(body) + "\n")
    total = sum(len(p["jpg"]) + len(p["rle"]) for p in pics)
    print(f"{len(pics)} imagens, {total / 1024:.0f} KB -> {out}")


if __name__ == "__main__":
    main()
