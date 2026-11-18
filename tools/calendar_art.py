#!/usr/bin/env python3
"""Calendar art: one background (no cat) + 4 cat sprites drawn on top at runtime.

Inputs  (assets/calendar/):  background_source.jpg (944x1120), cats/<name>.png (RGBA cutouts)
Outputs:
  esp32-s3/src/modules/calendar/calendar_art.c   3.5" (480x320): background JPEG + per cat a
                                                  JPEG sprite and a 4-bit RLE alpha mask
  assets/calendar/out/<WxH>/background.jpg, <cat>.png   5" sets (720x1280 and 1280x720)
Needs Pillow:  python3 -m pip install pillow   (run from the repo root)
"""
import io, os, sys
from PIL import Image, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = os.path.join(ROOT, "assets", "calendar")
CATS = ["frajola", "tricolor", "laranja", "cinza"]          # CalendarCat order
# Cat placement on the 944x1120 source: height, centre x, bottom y (sits on the stump).
CAT_H, CAT_CX, CAT_BOTTOM = 450, 294, 812           # 294: ~10 px further left on 3.5"
# Crop of the source for each screen size (left, top, right, bottom).
CROPS = {(480, 320): (100, 320, 944, 883), (720, 1280): (157, 0, 787, 1120), (1280, 720): (0, 300, 944, 831)}

def finish(img, size):
    img = img.resize(size, Image.LANCZOS)
    img = ImageEnhance.Color(img).enhance(1.06)
    return img.filter(ImageFilter.UnsharpMask(radius=1, percent=45, threshold=1))

def sprite_for(cat, size):
    """Cat cutout scaled for this screen and its top-left position."""
    l, t, r, b = CROPS[size]
    k = size[0] / (r - l)
    c = Image.open(os.path.join(A, "cats", cat + ".png")).convert("RGBA")
    h = round(CAT_H * k); w = round(c.size[0] * h / c.size[1])
    c = c.resize((w, h), Image.LANCZOS)
    c = blend_in(c, k)
    x = round((CAT_CX - l) * k) - w // 2 - c.pad; y = round((CAT_BOTTOM - t) * k) - h - c.pad
    return c, x, y

def blend_in(cat, k):
    """Make the cutout belong to the night painting: blue moonlight grade on the cat, a
    soft dark halo around it and a contact shadow on the stump under the paws."""
    rgb, a = cat.convert("RGB"), cat.split()[3]
    rgb = ImageEnhance.Contrast(rgb).enhance(1.22)            # as punchy as the painting
    rgb = ImageEnhance.Color(rgb).enhance(1.15)
    night = Image.new("RGB", rgb.size, (28, 52, 120))
    rgb = Image.blend(rgb, night, 0.05)                       # light moonlit cast only
    pad = max(4, round(18 * k))
    W, H = cat.size[0] + 2 * pad, cat.size[1] + 2 * pad
    halo = Image.new("L", (W, H), 0); halo.paste(a, (pad, pad))
    halo = halo.filter(ImageFilter.GaussianBlur(max(2, 8 * k))).point(lambda v: int(v * 0.55))
    shadow = Image.new("L", (W, H), 0)                        # contact shadow under the paws
    from PIL import ImageDraw
    ImageDraw.Draw(shadow).ellipse((pad + cat.size[0] * 0.05, pad + cat.size[1] * 0.93,
                                    pad + cat.size[0] * 0.98, pad + cat.size[1] * 1.03), fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(max(2, 10 * k)))
    dark = Image.eval(Image.merge("L", (halo,)), lambda v: v)
    dark = Image.fromarray if False else dark
    base_a = Image.new("L", (W, H), 0)
    base_a.paste(255, mask=None) if False else None
    out = Image.new("RGBA", (W, H), (8, 14, 34, 0))
    shade = Image.new("RGBA", (W, H), (8, 14, 34, 255))
    sa = Image.new("L", (W, H)); sa = Image.composite(halo, shadow, halo.point(lambda v: 255 if v > 0 else 0))
    sa = Image.eval(sa, lambda v: v)
    from PIL import ImageChops
    sa = ImageChops.lighter(halo, shadow)
    shade.putalpha(sa)
    out = Image.alpha_composite(out, shade)
    body = Image.new("RGBA", (W, H), (0, 0, 0, 0)); rgba = rgb.copy(); rgba.putalpha(a); body.paste(rgba, (pad, pad))
    out = Image.alpha_composite(out, body)
    out.pad = pad
    return out

def rle4(alpha):
    """Alpha (L image) -> bytes of (run, value4) pairs, run 1..255."""
    px = [v >> 4 for v in alpha.tobytes()]
    out, i = bytearray(), 0
    while i < len(px):
        v, n = px[i], 1
        while i + n < len(px) and px[i + n] == v and n < 255: n += 1
        out += bytes((n, v)); i += n
    return bytes(out)

def jpeg(img, q=88):
    b = io.BytesIO(); img.convert("RGB").save(b, "JPEG", quality=q); return b.getvalue()

def c_array(name, data):
    rows = ",\n".join(", ".join(f"0x{x:02x}" for x in data[i:i + 20]) for i in range(0, len(data), 20))
    return f"static const uint8_t {name}[{len(data)}] = {{\n{rows}\n}};\n"

def main():
    bg_src = Image.open(os.path.join(A, "background_source.jpg")).convert("RGB")
    for size in CROPS:                                   # 5" (and 3.5") sets as plain files
        d = os.path.join(A, "out", f"{size[0]}x{size[1]}"); os.makedirs(d, exist_ok=True)
        finish(bg_src.crop(CROPS[size]), size).save(os.path.join(d, "background.jpg"), quality=88)
        for cat in CATS:
            s, x, y = sprite_for(cat, size)
            s.save(os.path.join(d, f"{cat}.png"))
            prev = finish(bg_src.crop(CROPS[size]), size).convert("RGBA"); prev.alpha_composite(s, (max(0, x), max(0, y)))
            prev.convert("RGB").save(os.path.join(d, f"preview_{cat}.jpg"), quality=85)
            with open(os.path.join(d, "placement.txt"), "a" if cat != CATS[0] else "w") as f:
                f.write(f"{cat} x={x} y={y} w={s.size[0]} h={s.size[1]}\n")
    size = (480, 320)                                    # 3.5" firmware data
    bg = finish(bg_src.crop(CROPS[size]), size)
    out = ["// GENERATED by tools/calendar_art.py - do not edit.\n"
           "// Calendar 3.5\" (480x320): background without cat + one sprite per cat\n"
           "// (JPEG colours on a neutral fill + 4-bit RLE alpha), blended on the board.\n"
           "#include <stdint.h>\n#include <stddef.h>\n#include \"modules/calendar/calendar_art.h\"\n\n",
           c_array("kBg", jpeg(bg))]
    entries = []
    for cat in CATS:
        s, x, y = sprite_for(cat, size)
        rgb = Image.new("RGB", s.size, (40, 60, 110)); rgb.paste(s, mask=s.split()[3])
        out.append(c_array(f"kCat_{cat}", jpeg(rgb, 90)))
        out.append(c_array(f"kAlpha_{cat}", rle4(s.split()[3])))
        entries.append(f"  {{kCat_{cat}, sizeof kCat_{cat}, kAlpha_{cat}, sizeof kAlpha_{cat}, {x}, {y}, {s.size[0]}, {s.size[1]}}},")
    out.append("\nconst uint8_t *const kCalendarBg = kBg;\nconst size_t kCalendarBgSize = sizeof kBg;\n")
    out.append("const CalendarSprite kCalendarSprites[CALENDAR_SPRITES] = {\n" + "\n".join(entries) + "\n};\n")
    path = os.path.join(ROOT, "esp32-s3", "src", "modules", "calendar", "calendar_art.c")
    open(path, "w").write("".join(out))
    print("wrote", path, os.path.getsize(path), "bytes of C")

if __name__ == "__main__":
    sys.exit(main())
