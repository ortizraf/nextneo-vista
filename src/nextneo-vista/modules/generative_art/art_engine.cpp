#include "art_engine.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

static ArtPattern *const kPatterns[] = {
  &artPatternWaves(),
  &artPatternParticles(),
  &artPatternFlow(),
  &artPatternChristmas(),
};
static constexpr uint8_t kPatternCount = sizeof(kPatterns) / sizeof(kPatterns[0]);

uint8_t GenerativeArtEngine::patternCount() const { return kPatternCount; }
const char *GenerativeArtEngine::patternName(uint8_t i) const { return kPatterns[i < kPatternCount ? i : 0]->name(); }

bool GenerativeArtEngine::begin(int w, int h) {
  art::initMath();
  if (!surface_.px) {
    surface_.px = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!surface_.px) return false;                    // never fall back to internal RAM: 300 KB
  }
  surface_.w = w; surface_.h = h;
  surface_.clear();
  rng_.seed(esp_random());
  art::paletteBuild(params_.palette, palette_);
  t_ = 0;
  startPattern();
  return true;
}

void GenerativeArtEngine::end() {
  if (patternLive_) { kPatterns[pattern_]->end(); patternLive_ = false; }
  if (surface_.px) { heap_caps_free(surface_.px); surface_.px = nullptr; }
  touch_.down = false;
}

void GenerativeArtEngine::startPattern() {
  if (!surface_.px) return;
  if (patternLive_) { kPatterns[pattern_]->end(); patternLive_ = false; }
  surface_.clear();
  ArtContext ctx{surface_, palette_, params_, rng_, t_};
  patternLive_ = kPatterns[pattern_]->begin(ctx);
}

void GenerativeArtEngine::setPattern(uint8_t i) {
  if (i >= kPatternCount) i = 0;
  if (i == pattern_ && patternLive_) return;
  if (patternLive_) { kPatterns[pattern_]->end(); patternLive_ = false; }
  pattern_ = i;
  startPattern();
}

void GenerativeArtEngine::setParams(const ArtParams &p) {
  ArtParams n = p;
  if (n.speed < 0.25f) n.speed = 0.25f;
  if (n.speed > 2.0f) n.speed = 2.0f;
  if (n.density < 25) n.density = 25;
  if (n.density > 100) n.density = 100;
  if (n.palette >= art::PALETTE_COUNT) n.palette = 0;
  const bool densityChanged = n.density != params_.density;
  const bool paletteChanged = n.palette != params_.palette;
  params_ = n;
  if (paletteChanged) art::paletteBuild(params_.palette, palette_);
  if (densityChanged && patternLive_ && kPatterns[pattern_]->densityNeedsRestart()) startPattern();
}

void GenerativeArtEngine::update(uint32_t dtMs) {
  if (!surface_.px || !patternLive_) return;
  if (dtMs > 200) dtMs = 66;                           // after a stall, don't jump
  const uint32_t t0 = micros();
  const float dt = dtMs * 0.001f * params_.speed * timeScale_;
  t_ += dt;
  if (t_ > 100000.0f) t_ = 0;                          // keep float precision
  ArtContext ctx{surface_, palette_, params_, rng_, t_};
  kPatterns[pattern_]->step(ctx, dt, touch_);
  lastStepUs_ = micros() - t0;
}

bool GenerativeArtEngine::caption(char *buf, size_t n) {
  return surface_.px && patternLive_ && kPatterns[pattern_]->caption(buf, n);
}
