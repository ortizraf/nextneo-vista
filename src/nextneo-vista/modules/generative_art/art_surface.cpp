#include "art_surface.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

namespace art {
float kSinLut[256];

void initMath() {
  static bool done = false;
  if (done) return;
  for (int i = 0; i < 256; ++i) kSinLut[i] = sinf(i * 6.28318531f / 256.0f);
  done = true;
}

static uint16_t mix(uint32_t a, uint32_t b, float t) {
  const int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  const int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  const uint32_t r = (uint32_t)(ar + (br - ar) * t), g = (uint32_t)(ag + (bg - ag) * t), l = (uint32_t)(ab + (bb - ab) * t);
  return rgb565((r << 16) | (g << 8) | l);
}

void Palette::build(uint32_t dark, uint32_t mid, uint32_t bright) {
  const int half = PALETTE_STEPS / 2;
  // 0..15: black -> dark -> mid, 16..31: mid -> bright
  for (int i = 0; i < half; ++i) {
    const float t = (float)(i + 1) / half;
    c[i] = t < 0.5f ? mix(0x000000, dark, t * 2) : mix(dark, mid, (t - 0.5f) * 2);
  }
  for (int i = half; i < PALETTE_STEPS; ++i) c[i] = mix(mid, bright, (float)(i - half + 1) / half);
}

// Nextneo Vista identity: navy / deep blue / electric blue / cyan only.
struct PaletteDef { const char *name; uint32_t dark, mid, bright; };
static const PaletteDef kPalettes[PALETTE_COUNT] = {
  {"Oceano",            0x06205E, 0x1456E0, 0x38D6F5},
  {"Aurora",            0x0A2A66, 0x1E90FF, 0x6EE7D8},
  {"El\xC3\xA9trico",   0x101A70, 0x3D5BFF, 0xB5F1FF},
};
const char *paletteName(uint8_t i) { return kPalettes[i < PALETTE_COUNT ? i : 0].name; }
void paletteBuild(uint8_t i, Palette &out) {
  const PaletteDef &d = kPalettes[i < PALETTE_COUNT ? i : 0];
  out.build(d.dark, d.mid, d.bright);
}
}  // namespace art

void ArtSurface::clear() {
  if (px) memset(px, 0, (size_t)w * h * 2);
}

static inline uint16_t fadePx(uint32_t c, uint8_t s) {
  if (!c) return 0;
  int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
  if (r) r -= (r >> s) + 1;
  if (g) g -= (g >> s) + 1;
  if (b) b -= (b >> s) + 1;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void ArtSurface::fade(uint8_t shift) {
  if (!px) return;
  uint32_t *p = (uint32_t *)px;                         // 2 pixels per read: halves PSRAM traffic
  const size_t n = (size_t)w * h / 2;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t v = p[i];
    if (!v) continue;                                   // most of the screen is black
    p[i] = (uint32_t)fadePx(v & 0xFFFF, shift) | ((uint32_t)fadePx(v >> 16, shift) << 16);
  }
}

void ArtSurface::dot(int x, int y, uint16_t c) {
  add(x, y, c); add(x + 1, y, c); add(x, y + 1, c); add(x + 1, y + 1, c);
}

void ArtSurface::line(int x0, int y0, int x1, int y1, uint16_t c) {
  // reject lines fully outside (cheap) so off-screen particles cost nothing
  if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= w && x1 >= w) || (y0 >= h && y1 >= h)) return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 2048; ++guard) {          // guard: never spin on bad input
    add(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
