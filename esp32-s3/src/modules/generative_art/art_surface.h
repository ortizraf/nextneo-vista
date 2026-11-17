#pragma once
// Generative Art: RGB565 drawing surface + palettes + cheap math.
// No LVGL / Arduino dependency on purpose: the engine and patterns only draw into
// a plain uint16_t buffer that the screen shows with an lv_image (like clock.cpp).
// Pixels are native RGB565 (same format the rest of the UI uses).
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- fast trig
// 256-entry table: ~1.4 degree steps, plenty for procedural motion. Avoids sinf()
// inside per-particle / per-vertex loops.
namespace art {
extern float kSinLut[256];
void initMath();                                       // fills kSinLut once
inline float fsin(float rad) {
  int i = (int)(rad * 40.7436654f);                    // 256 / (2*pi)
  return kSinLut[i & 255];
}
inline float fcos(float rad) { return fsin(rad + 1.5707963f); }

// xorshift32: deterministic, no heap, no esp_random() per particle
struct Rng {
  uint32_t s = 0x9E3779B9u;
  void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  float uni() { return (next() >> 8) * (1.0f / 16777216.0f); }          // [0,1)
  float range(float a, float b) { return a + (b - a) * uni(); }
};

// ---------------------------------------------------------------- colour
inline uint16_t rgb565(uint32_t hex) {
  return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}
// Per-channel saturating add: overlapping strokes glow instead of overwriting.
inline uint16_t add565(uint16_t a, uint16_t b) {
  uint32_t r = (a >> 11) + (b >> 11);                 if (r > 31) r = 31;
  uint32_t g = ((a >> 5) & 63) + ((b >> 5) & 63);     if (g > 63) g = 63;
  uint32_t l = (a & 31) + (b & 31);                   if (l > 31) l = 31;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

// 32-step intensity ramp (dark -> bright) built from 3 colour stops.
static constexpr int PALETTE_STEPS = 32;
struct Palette {
  uint16_t c[PALETTE_STEPS];
  void build(uint32_t dark, uint32_t mid, uint32_t bright);
  uint16_t at(int i) const { return c[i < 0 ? 0 : i >= PALETTE_STEPS ? PALETTE_STEPS - 1 : i]; }
};
static constexpr uint8_t PALETTE_COUNT = 3;
const char *paletteName(uint8_t i);
void paletteBuild(uint8_t i, Palette &out);
}  // namespace art

// ---------------------------------------------------------------- surface
struct ArtSurface {
  uint16_t *px = nullptr;
  int w = 0, h = 0;

  void clear();
  // Trail fade: every channel loses (c >> shift) + 1 per call, so everything
  // reaches black in a bounded number of frames. Black words are skipped.
  void fade(uint8_t shift);
  inline void add(int x, int y, uint16_t c) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    uint16_t &p = px[y * w + x];
    p = art::add565(p, c);
  }
  void dot(int x, int y, uint16_t c);                    // 2x2 additive
  void line(int x0, int y0, int x1, int y1, uint16_t c); // Bresenham, additive, clipped per pixel
};
