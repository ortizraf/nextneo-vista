// Pattern A - Ondas: a few horizontal waves, each the sum of two sines with their
// own frequency / amplitude / phase drift. Drawn as additive lines (some dotted)
// over a fast fade, so crossings glow and motion leaves a short trail.
#include "art_pattern.h"

namespace {
constexpr int MAX_WAVES = 6;
constexpr int STEP_X = 4;                               // vertex every 4 px -> 121 segments per wave

struct Wave {
  float baseY, a1, k1, v1, p1, a2, k2, v2, p2;
  int color;                                            // palette index of the stroke
  bool dotted;
};

class WavesPattern : public ArtPattern {
 public:
  const char *name() const override { return "Ondas"; }
  bool densityNeedsRestart() const override { return true; }

  bool begin(ArtContext &c) override {
    count_ = 3 + (c.p.density * (MAX_WAVES - 3) + 50) / 100;     // 3..6 waves
    if (count_ > MAX_WAVES) count_ = MAX_WAVES;
    const float h = (float)c.s.h;
    for (int i = 0; i < count_; ++i) {
      Wave &w = waves_[i];
      w.baseY = h * (0.30f + 0.40f * (i + 0.5f) / count_);
      w.a1 = c.rng.range(18, 46);  w.k1 = c.rng.range(0.008f, 0.020f); w.v1 = c.rng.range(0.5f, 1.1f);  w.p1 = c.rng.range(0, 6.28f);
      w.a2 = c.rng.range(6, 18);   w.k2 = c.rng.range(0.025f, 0.050f); w.v2 = c.rng.range(-1.6f, -0.7f); w.p2 = c.rng.range(0, 6.28f);
      w.color = 14 + (i * 13) % 14;                     // spread over the mid/bright half
      w.dotted = (i % 3) == 2;
    }
    return true;
  }
  void end() override {}

  void step(ArtContext &c, float, const ArtTouch &) override {
    c.s.fade(2);
    const int w = c.s.w;
    for (int i = 0; i < count_; ++i) {
      const Wave &wv = waves_[i];
      const float ph1 = wv.p1 + c.t * wv.v1, ph2 = wv.p2 + c.t * wv.v2;
      // slow "breathing" of the amplitude keeps the picture from looking periodic
      const float breathe = 0.75f + 0.25f * art::fsin(c.t * 0.23f + i * 1.7f);
      const uint16_t col = c.pal.at(wv.color), glow = c.pal.at(wv.color + 4);
      int px = 0, py = 0;
      for (int x = 0; x <= w; x += STEP_X) {
        const float y = wv.baseY + breathe * (wv.a1 * art::fsin(wv.k1 * x + ph1) + wv.a2 * art::fsin(wv.k2 * x + ph2));
        const int iy = (int)y;
        if (wv.dotted) {
          if ((x / STEP_X) % 2 == 0) c.s.dot(x, iy, glow);
        } else if (x) {
          c.s.line(px, py, x, iy, col);
        }
        px = x; py = iy;
      }
    }
  }

 private:
  Wave waves_[MAX_WAVES];
  int count_ = 3;
};
}  // namespace

ArtPattern &artPatternWaves() { static WavesPattern p; return p; }
