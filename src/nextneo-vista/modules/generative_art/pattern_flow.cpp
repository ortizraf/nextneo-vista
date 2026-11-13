// Pattern C - Fluxo: tracers follow a procedural vector field (sum of three sine
// layers that drift over time) and leave faint additive strokes. Long, slow fade
// turns them into organic streams. No physics, just angle(x, y, t).
#include "art_pattern.h"
#include <esp_heap_caps.h>

namespace {
constexpr int MIN_T = 80, MAX_T = 320;

struct Tracer { float x, y; uint16_t life; uint8_t tone; };

class FlowPattern : public ArtPattern {
 public:
  const char *name() const override { return "Fluxo"; }
  bool densityNeedsRestart() const override { return true; }

  bool begin(ArtContext &c) override {
    if (!t_) t_ = (Tracer *)heap_caps_malloc(sizeof(Tracer) * MAX_T, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t_) return false;
    n_ = MIN_T + (MAX_T - MIN_T) * (c.p.density - 25) / 75;
    if (n_ < MIN_T) n_ = MIN_T;
    if (n_ > MAX_T) n_ = MAX_T;
    for (int i = 0; i < n_; ++i) spawn(c, t_[i]);
    frame_ = 0;
    return true;
  }
  void end() override {
    if (t_) { heap_caps_free(t_); t_ = nullptr; }
    n_ = 0;
  }

  void step(ArtContext &c, float dt, const ArtTouch &) override {
    if ((++frame_ & 1) == 0) c.s.fade(4);               // slow fade, every other frame: long streams
    const float W = (float)c.s.w, H = (float)c.s.h;
    const float speed = 42.0f * dt;
    for (int i = 0; i < n_; ++i) {
      Tracer &r = t_[i];
      const float a = angle(r.x, r.y, c.t);
      const float nx = r.x + art::fcos(a) * speed, ny = r.y + art::fsin(a) * speed;
      c.s.line((int)r.x, (int)r.y, (int)nx, (int)ny, c.pal.at(r.tone));
      r.x = nx; r.y = ny;
      if (r.life) --r.life;
      if (!r.life || r.x < -4 || r.y < -4 || r.x > W + 4 || r.y > H + 4) spawn(c, r);
    }
  }

 private:
  static float angle(float x, float y, float t) {
    return 1.9f * (art::fsin(x * 0.0093f + t * 0.21f) + art::fsin(y * 0.0117f - t * 0.17f) +
                   art::fsin((x + y) * 0.0061f + t * 0.11f));
  }
  void spawn(ArtContext &c, Tracer &r) {
    r.x = c.rng.range(0, c.s.w); r.y = c.rng.range(0, c.s.h);
    r.life = (uint16_t)(60 + c.rng.next() % 180);       // 4..16 s at 15 fps
    r.tone = (uint8_t)(6 + c.rng.next() % 9);           // faint: brightness builds up where streams overlap
  }
  Tracer *t_ = nullptr;
  int n_ = 0;
  uint32_t frame_ = 0;
};
}  // namespace

ArtPattern &artPatternFlow() { static FlowPattern p; return p; }
