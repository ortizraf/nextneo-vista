#pragma once
// GenerativeArtEngine: owns the frame buffer and the active pattern. Knows nothing
// about LVGL widgets; the screen (generative_art_screen.cpp) shows pixels() with an
// lv_image and calls update() from an LVGL timer. Single-threaded (LVGL thread).
#include "art_pattern.h"

class GenerativeArtEngine {
 public:
  bool begin(int w, int h);                // allocates the frame buffer (PSRAM) once per visit
  void end();                              // stops the pattern and frees every buffer
  bool running() const { return surface_.px != nullptr; }

  uint8_t patternCount() const;
  const char *patternName(uint8_t i) const;
  uint8_t pattern() const { return pattern_; }
  void setPattern(uint8_t i);              // switches + clears (allocates only the new pattern's state)

  const ArtParams &params() const { return params_; }
  void setParams(const ArtParams &p);      // applied live, no reboot

  void setTimeScale(float k) { timeScale_ = k; }   // ambient mode slows everything down
  void setTouch(bool down, int x, int y) { touch_.down = down; touch_.x = x; touch_.y = y; }

  void update(uint32_t dtMs);              // advance + draw one frame
  uint16_t *pixels() const { return surface_.px; }
  uint32_t lastStepUs() const { return lastStepUs_; }

 private:
  void startPattern();
  ArtSurface surface_;
  art::Palette palette_;
  art::Rng rng_;
  ArtParams params_;
  ArtTouch touch_;
  uint8_t pattern_ = 0;
  bool patternLive_ = false;
  float t_ = 0, timeScale_ = 1.0f;
  uint32_t lastStepUs_ = 0;
};
