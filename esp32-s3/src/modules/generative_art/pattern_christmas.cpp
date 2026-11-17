// Pattern D - Natal: a 3D Christmas tree computed every frame (no images).
//   - The tree is a cone: every needle, light, ornament and the tinsel has cylindrical
//     coordinates (height t, angle a, radius). Each frame the whole tree turns (a + rot),
//     is projected (x = cx + r cos, depth = sin) and drawn back-to-front, with
//     depth shading so it reads as 3D.
//   - Lights: three spiral strands, gold / red / green / warm white, twinkling in waves
//     that run along the spiral.
//   - Snow: falling flakes; dragging a finger acts as wind.
//   - Static scenery (sky gradient, snowy ground, trunk, presents, star) is drawn once into
//     a PSRAM background that is copied at the start of each frame.
// Fixed Christmas colours: the user's blue palettes apply to the other patterns.
#include "core/i18n.h"
#include "modules/generative_art/art_pattern.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

namespace {
constexpr int NEEDLES = 2200, LIGHTS = 198, TINSEL = 360, ORNAMENTS = 14, MAX_FLAKES = 200;

struct Needle { uint8_t t, a, rf; int8_t dx; uint8_t dy, g, keepBack, pad; };
struct Flake { float x, y, vy, ph; uint8_t s; };

inline uint16_t rgb(uint32_t h) { return art::rgb565(h); }
inline uint16_t shade(uint32_t h, float k) {                // colour * k (0..1)
  const int r = (int)(((h >> 16) & 255) * k), g = (int)(((h >> 8) & 255) * k), b = (int)((h & 255) * k);
  return art::rgb565(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}
inline float lsin(int a256) { return art::kSinLut[a256 & 255]; }
inline float lcos(int a256) { return art::kSinLut[(a256 + 64) & 255]; }

class ChristmasPattern : public ArtPattern {
 public:
  const char *name() const override { return "Natal"; }
  bool densityNeedsRestart() const override { return true; }   // snow amount

  bool begin(ArtContext &c) override {
    end();
    W_ = c.s.w; H_ = c.s.h;
    cx_ = W_ / 2; top_ = H_ * 0.18f; base_ = H_ * 0.82f; R_ = H_ * 0.30f;
    bg_ = (uint16_t *)heap_caps_malloc((size_t)W_ * H_ * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    nd_ = (Needle *)heap_caps_malloc(sizeof(Needle) * NEEDLES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fl_ = (Flake *)heap_caps_malloc(sizeof(Flake) * MAX_FLAKES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bg_ || !nd_ || !fl_) { end(); return false; }
    // needles: random points on/in the cone (more towards the outside), short strokes
    for (int i = 0; i < NEEDLES; ++i) {
      Needle &n = nd_[i];
      n.t = (uint8_t)(255 * powf(c.rng.uni(), 0.8f));
      n.a = (uint8_t)(c.rng.next() & 255);
      n.rf = (uint8_t)(140 + c.rng.next() % 116);
      n.dx = (int8_t)((int)(c.rng.next() % 7) - 3);
      n.dy = (uint8_t)(1 + c.rng.next() % 4);
      n.g = (uint8_t)(90 + c.rng.next() % 70);
      n.keepBack = (c.rng.next() % 10) < 4;               // only some of the back needles (cheaper, airier)
    }
    for (int i = 0; i < ORNAMENTS; ++i) {
      ornT_[i] = 0.25f + 0.7f * c.rng.uni();
      ornA_[i] = (uint8_t)(c.rng.next() & 255);
    }
    nFlakes_ = 60 + (MAX_FLAKES - 60) * (c.p.density - 25) / 75;
    if (nFlakes_ > MAX_FLAKES) nFlakes_ = MAX_FLAKES;
    for (int i = 0; i < nFlakes_; ++i) spawnFlake(c, fl_[i], true);
    rot_ = 0; wind_ = 0; lastTouchX_ = -1;
    drawBackground();
    return true;
  }

  void end() override {
    if (bg_) { heap_caps_free(bg_); bg_ = nullptr; }
    if (nd_) { heap_caps_free(nd_); nd_ = nullptr; }
    if (fl_) { heap_caps_free(fl_); fl_ = nullptr; }
  }

  bool caption(char *buf, size_t n) override {          // countdown, local time (NTP)
    const time_t now = time(nullptr);
    if (now < 1700000000) return false;
    struct tm t; localtime_r(&now, &t);
    if (t.tm_mon == 11 && t.tm_mday == 24) { snprintf(buf, n, T("V\xC3\xA9spera de Natal!", "Christmas Eve!")); return true; }
    if (t.tm_mon == 11 && t.tm_mday == 25) { snprintf(buf, n, T("Feliz Natal!", "Merry Christmas!")); return true; }
    if (t.tm_mon == 11 && t.tm_mday > 25) { snprintf(buf, n, T("Boas festas!", "Happy holidays!")); return true; }
    struct tm x = t; x.tm_mon = 11; x.tm_mday = 25; x.tm_hour = 0; x.tm_min = 0; x.tm_sec = 0;
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    const long days = (long)((mktime(&x) - mktime(&t) + 43200) / 86400);
    if (days == 1) snprintf(buf, n, T("Falta 1 dia para o Natal", "1 day to Christmas"));
    else snprintf(buf, n, T("Faltam %ld dias para o Natal", "%ld days to Christmas"), days);
    return true;
  }

  void step(ArtContext &c, float dt, const ArtTouch &touch) override {
    memcpy(c.s.px, bg_, (size_t)W_ * H_ * 2);
    rot_ += 0.35f * dt;                                   // tree turns slowly
    if (rot_ > 6.2831853f) rot_ -= 6.2831853f;
    const int rot = (int)(rot_ * 40.743665f);
    for (int pass = 0; pass < 2; ++pass) {                // 0: back half, 1: front half
      drawNeedles(c, rot, pass);
      drawTinsel(c, rot, pass);
      drawLights(c, rot, pass);
      drawOrnaments(c, rot, pass);
    }
    drawStar(c);
    // wind from a finger drag, decaying back to calm
    if (touch.down) {
      if (lastTouchX_ >= 0 && dt > 0) {
        float w = (touch.x - lastTouchX_) / dt * 0.6f;
        if (w > 160) w = 160; else if (w < -160) w = -160;
        wind_ += (w - wind_) * 0.3f;
      }
      lastTouchX_ = touch.x;
    } else lastTouchX_ = -1;
    wind_ *= 1.0f - 0.9f * dt;
    for (int i = 0; i < nFlakes_; ++i) {
      Flake &f = fl_[i];
      f.y += f.vy * dt;
      f.x += (art::fsin(c.t * 1.3f + f.ph) * 10.0f + wind_ * (0.6f + f.s * 0.2f)) * dt;
      if (f.y > groundY((int)f.x) || f.x < -10 || f.x > W_ + 10) spawnFlake(c, f, false);
      disc(c.s, (int)f.x, (int)f.y, f.s * 0.5f + 0.6f, rgb(f.s >= 3 ? 0xF4F8FF : 0xC8D4EC));
    }
  }

 private:
  float groundY(int x) const { return H_ * 0.89f + 6 * art::fsin(x * 0.02f) + 4 * art::fsin(x * 0.051f); }

  void put(ArtSurface &s, int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)W_ && (unsigned)y < (unsigned)H_) s.px[y * W_ + x] = c;
  }
  void disc(ArtSurface &s, int x, int y, float r, uint16_t c) {
    const int ri = (int)(r + 0.5f);
    for (int dy = -ri; dy <= ri; ++dy)
      for (int dx = -ri; dx <= ri; ++dx)
        if (dx * dx + dy * dy <= r * r + 0.5f) put(s, x + dx, y + dy, c);
  }
  void halo(ArtSurface &s, int x, int y, uint32_t col, float k) {   // soft additive glow
    const uint16_t c1 = shade(col, 0.22f * k), c2 = shade(col, 0.10f * k);
    for (int dy = -3; dy <= 3; ++dy)
      for (int dx = -3; dx <= 3; ++dx) {
        const int d2 = dx * dx + dy * dy;
        if (d2 <= 4) s.add(x + dx, y + dy, c1); else if (d2 <= 10) s.add(x + dx, y + dy, c2);
      }
  }

  // projection of a point of the cone: height t (0 top .. 1 base), angle a256, radius factor
  void project(float t, int a256, float rf, float &x, float &y, float &z) const {
    const float r = R_ * t * rf;
    x = cx_ + r * lcos(a256); y = top_ + (base_ - top_) * t; z = lsin(a256);
  }

  void drawNeedles(ArtContext &c, int rot, int pass) {
    for (int i = 0; i < NEEDLES; ++i) {
      const Needle &n = nd_[i];
      const int a = n.a + rot;
      const float z = lsin(a);
      if ((z >= 0) != (pass == 1)) continue;
      if (!pass && !n.keepBack) continue;
      float x, y, zz; project(n.t / 255.0f, a, n.rf / 255.0f, x, y, zz);
      const float k = 0.42f + 0.58f * (z + 1) * 0.5f;
      const uint16_t col = rgb(((uint32_t)(int)(16 * k) << 16) | ((uint32_t)(int)(n.g * k) << 8) | (uint32_t)(int)(38 * k));
      const int x0 = (int)x, y0 = (int)y;
      const int steps = n.dy > abs(n.dx) ? n.dy : abs(n.dx);
      for (int s = 0; s <= steps; ++s) put(c.s, x0 + n.dx * s / (steps ? steps : 1), y0 + n.dy * s / (steps ? steps : 1), col);
    }
  }

  void drawTinsel(ArtContext &c, int rot, int pass) {      // gold garland, 5.5 turns
    float px = 0, py = 0; bool have = false;
    for (int k = 0; k < TINSEL; ++k) {
      const float t = (float)k / (TINSEL - 1);
      const int a = (int)(t * 5.5f * 256) + 50 + rot;
      float x, y, z; project(t, a, 1.03f, x, y, z);
      const bool front = z >= -0.1f;
      if (front == (pass == 1) && have) {
        const uint16_t col = shade(0xFFC23C, 0.45f + 0.55f * (z + 1) * 0.5f);
        lineSolid(c.s, (int)px, (int)py, (int)x, (int)y, col);
        lineSolid(c.s, (int)px, (int)py + 1, (int)x, (int)y + 1, shade(0xC88A1E, 0.45f + 0.55f * (z + 1) * 0.5f));
      }
      px = x; py = y; have = true;
    }
  }
  void lineSolid(ArtSurface &s, int x0, int y0, int x1, int y1, uint16_t col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int g = 0; g < 64; ++g) {
      put(s, x0, y0, col);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void drawLights(ArtContext &c, int rot, int pass) {
    static const uint32_t kCol[6] = {0xFFC83C, 0xEB2828, 0xFFC83C, 0x46DC5A, 0xEB2828, 0xFFF5D7};
    const int perStrand = LIGHTS / 3;
    for (int s = 0; s < 3; ++s)
      for (int k = 0; k < perStrand; ++k) {
        const float t = (k + 0.5f) / perStrand;
        const int a = (int)(t * 3.85f * 256) + s * 85 + rot;
        float x, y, z; project(t, a, 0.97f, x, y, z);
        if ((z >= 0) != (pass == 1)) continue;
        if (z < -0.35f) continue;                          // hidden by the foliage
        const int idx = s * perStrand + k;
        const uint32_t col = kCol[(idx + s) % 6];
        const float tw = 0.55f + 0.45f * art::fsin(c.t * 2.4f - t * 14.0f + idx * 0.9f);   // twinkle wave
        const float depth = 0.4f + 0.6f * (z + 1) * 0.5f;
        const float k2 = tw * depth;
        if (pass == 1) halo(c.s, (int)x, (int)y, col, k2);
        disc(c.s, (int)x, (int)y, 1.2f + 1.9f * (z + 1) * 0.5f, shade(col, 0.35f + 0.65f * k2));
      }
  }

  void drawOrnaments(ArtContext &c, int rot, int pass) {
    if (pass != 1) return;
    for (int i = 0; i < ORNAMENTS; ++i) {
      const int a = ornA_[i] + rot;
      float x, y, z; project(ornT_[i], a, 0.86f, x, y, z);
      if (z < 0.05f) continue;
      const uint32_t col = (i & 1) ? 0xE02828 : 0xFFC23C;
      const float k = 0.55f + 0.45f * z;
      disc(c.s, (int)x, (int)y, 3.4f, shade(col, k));
      put(c.s, (int)x - 1, (int)y - 1, rgb(0xFFFFFF));      // highlight
    }
  }

  void drawStar(ArtContext &c) {
    const float sx = cx_, sy = top_ - 8;
    const float pulse = 1.0f + 0.08f * art::fsin(c.t * 2.0f);
    for (int dy = -16; dy <= 16; ++dy)                   // 5-point star by angle test + glow
      for (int dx = -16; dx <= 16; ++dx) {
        const float r = sqrtf((float)(dx * dx + dy * dy));
        float a = atan2f((float)dy, (float)dx) + 1.5707963f;
        a = fmodf(a + 6.2831853f, 1.2566371f) - 0.6283185f;   // fold into one point
        const float edge = (14 * pulse) * (0.42f + 0.58f * (1 - fabsf(a) / 0.6283185f));
        if (r <= edge) put(c.s, (int)sx + dx, (int)sy + dy, rgb(r < edge * 0.45f ? 0xFFF3C0 : 0xFFD24A));
        else if (r <= edge + 3) c.s.add((int)sx + dx, (int)sy + dy, shade(0xFFC23C, 0.25f));
      }
  }

  void spawnFlake(ArtContext &c, Flake &f, bool anywhere) {
    f.x = c.rng.range(-10, W_ + 10);
    f.y = anywhere ? c.rng.range(0, H_ * 0.85f) : c.rng.range(-20, -2);
    f.s = (uint8_t)(1 + c.rng.next() % 4);
    f.vy = 14 + f.s * 9 + c.rng.range(0, 8);
    f.ph = c.rng.range(0, 6.28f);
  }

  void drawBackground() {
    ArtSurface s{bg_, W_, H_};
    for (int y = 0; y < H_; ++y) {                        // navy sky gradient (Nextneo)
      const float t = (float)y / H_;
      const uint16_t col = rgb(((uint32_t)(int)(5 + 6 * t) << 16) | ((uint32_t)(int)(14 + 28 * t) << 8) | (uint32_t)(int)(36 + 71 * t));
      for (int x = 0; x < W_; ++x) bg_[y * W_ + x] = col;
    }
    // trunk
    for (int y = (int)base_; y < (int)base_ + 18; ++y)
      for (int x = cx_ - 9; x <= cx_ + 9; ++x) put(s, x, y, rgb(0x46281A));
    // snowy ground with a soft edge
    for (int x = 0; x < W_; ++x) {
      const int gy = (int)groundY(x);
      for (int y = gy; y < H_; ++y) bg_[y * W_ + x] = rgb(y == gy ? 0xFFFFFF : 0xE2ECFA);
    }
    // presents: box + ribbons
    struct P { int x, w, h; uint32_t c, rib; } ps[3] = {{cx_ - 54, 26, 20, 0xE02828, 0xFFC23C},
                                                        {cx_ + 28, 30, 24, 0x1E9646, 0xFFC23C},
                                                        {cx_ - 26, 20, 15, 0xFFC23C, 0xE02828}};
    for (const P &p : ps) {
      const int yb = (int)groundY(p.x + p.w / 2) + 2;
      for (int y = yb - p.h; y < yb; ++y)
        for (int x = p.x; x < p.x + p.w; ++x) {
          const bool rib = abs(x - (p.x + p.w / 2)) <= 2 || abs(y - (yb - p.h / 2)) <= 2;
          put(s, x, y, rgb(rib ? p.rib : p.c));
        }
    }
  }

  uint16_t *bg_ = nullptr;
  Needle *nd_ = nullptr;
  Flake *fl_ = nullptr;
  int nFlakes_ = 0;
  float ornT_[ORNAMENTS];
  uint8_t ornA_[ORNAMENTS];
  int W_ = 0, H_ = 0, cx_ = 0;
  float top_ = 0, base_ = 0, R_ = 0, rot_ = 0, wind_ = 0;
  int lastTouchX_ = -1;
};
}  // namespace

ArtPattern &artPatternChristmas() { static ChristmasPattern p; return p; }
