// Pattern B - Partículas: a bounded swarm drifting in a slowly changing force field,
// attracted to (or repelled from) the finger, with faint links between neighbours.
// State lives in PSRAM, allocated once in begin(). O(n^2/2) link test with a hard
// cap on drawn links keeps the cost predictable.
#include "art_pattern.h"
#include <esp_heap_caps.h>
#include <math.h>

namespace {
constexpr int MIN_P = 30, MAX_P = 140;
constexpr float LINK_D = 46.0f, LINK_D2 = LINK_D * LINK_D;
constexpr int MAX_LINKS = 260;                          // per frame
constexpr float MAX_V = 90.0f;                          // px/s
constexpr float TOUCH_R = 170.0f, TOUCH_ACC = 160.0f;  // finger influence radius / strength

struct Particle { float x, y, vx, vy; uint8_t tone; };

class ParticlesPattern : public ArtPattern {
 public:
  const char *name() const override { return "Part\xC3\xAD" "culas"; }
  bool densityNeedsRestart() const override { return true; }

  bool begin(ArtContext &c) override {
    if (!p_) p_ = (Particle *)heap_caps_malloc(sizeof(Particle) * MAX_P, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p_) return false;
    n_ = MIN_P + (MAX_P - MIN_P) * (c.p.density - 25) / 75;
    if (n_ < MIN_P) n_ = MIN_P;
    if (n_ > MAX_P) n_ = MAX_P;
    for (int i = 0; i < n_; ++i) {
      Particle &q = p_[i];
      q.x = c.rng.range(0, c.s.w); q.y = c.rng.range(0, c.s.h);
      q.vx = c.rng.range(-20, 20); q.vy = c.rng.range(-20, 20);
      q.tone = (uint8_t)(20 + c.rng.next() % 12);
    }
    return true;
  }
  void end() override {
    if (p_) { heap_caps_free(p_); p_ = nullptr; }
    n_ = 0;
  }

  void step(ArtContext &c, float dt, const ArtTouch &touch) override {
    c.s.fade(1);                                         // short trails: halves every frame
    const float W = (float)c.s.w, H = (float)c.s.h;
    const float damp = 1.0f - 0.8f * dt;
    const float sign = c.p.touchRepels ? -1.0f : 1.0f;
    for (int i = 0; i < n_; ++i) {
      Particle &q = p_[i];
      // procedural force field (cheap curl-like swirl)
      float ax = 14.0f * art::fsin(q.y * 0.012f + c.t * 0.40f);
      float ay = 14.0f * art::fsin(q.x * 0.010f - c.t * 0.31f);
      if (touch.down) {
        const float dx = touch.x - q.x, dy = touch.y - q.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < TOUCH_R * TOUCH_R && d2 > 1.0f) {
          const float d = sqrtf(d2);                     // hardware FPU, n <= 140 per frame
          const float nx = dx / d, ny = dy / d;
          float f = sign * TOUCH_ACC * (1.0f - d / TOUCH_R);
          if (sign > 0 && d < 30.0f) f = -60.0f;         // attract: keep a small core free -> particles orbit
          ax += nx * f - ny * 40.0f * sign;              // + tangential swirl
          ay += ny * f + nx * 40.0f * sign;
        }
      }
      q.vx = (q.vx + ax * dt) * damp;
      q.vy = (q.vy + ay * dt) * damp;
      if (q.vx > MAX_V) q.vx = MAX_V; else if (q.vx < -MAX_V) q.vx = -MAX_V;
      if (q.vy > MAX_V) q.vy = MAX_V; else if (q.vy < -MAX_V) q.vy = -MAX_V;
      // keep a minimum drift so the swarm never freezes
      if (q.vx * q.vx + q.vy * q.vy < 16.0f) { q.vx += c.rng.range(-6, 6); q.vy += c.rng.range(-6, 6); }
      q.x += q.vx * dt; q.y += q.vy * dt;
      if (q.x < 0) q.x += W; else if (q.x >= W) q.x -= W;    // wrap around
      if (q.y < 0) q.y += H; else if (q.y >= H) q.y -= H;
    }
    int links = 0;
    for (int i = 0; i < n_ && links < MAX_LINKS; ++i) {
      const Particle &a = p_[i];
      for (int j = i + 1; j < n_; ++j) {
        const Particle &b = p_[j];
        const float dx = a.x - b.x;
        if (dx > LINK_D || dx < -LINK_D) continue;
        const float dy = a.y - b.y;
        if (dy > LINK_D || dy < -LINK_D) continue;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= LINK_D2) continue;
        const int tone = 3 + (int)(11.0f * (1.0f - d2 / LINK_D2));   // nearer = brighter
        c.s.line((int)a.x, (int)a.y, (int)b.x, (int)b.y, c.pal.at(tone));
        if (++links >= MAX_LINKS) break;
      }
    }
    for (int i = 0; i < n_; ++i) c.s.dot((int)p_[i].x, (int)p_[i].y, c.pal.at(p_[i].tone));
    if (touch.down) {                                   // faint ring under the finger
      for (int k = 0; k < 32; ++k) {
        const float a = k * 0.19634954f;
        c.s.add(touch.x + (int)(18 * art::fcos(a)), touch.y + (int)(18 * art::fsin(a)), c.pal.at(12));
      }
    }
  }

 private:
  Particle *p_ = nullptr;
  int n_ = 0;
};
}  // namespace

ArtPattern &artPatternParticles() { static ParticlesPattern p; return p; }
