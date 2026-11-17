#pragma once
// Common interface of a Generative Art pattern.
// To add a pattern (fractal, geometry, nebula, liquid, oscilloscope/audio...):
//   1. create pattern_<name>.cpp implementing ArtPattern;
//   2. expose `ArtPattern &artPattern<Name>()`;
//   3. add it to kPatterns[] in art_engine.cpp (and a caption in the screen's map).
#include "modules/generative_art/art_surface.h"

struct ArtParams {
  float speed = 1.0f;          // 0.25 .. 2.0
  uint8_t density = 60;        // 25 .. 100 (% of the pattern's maximum)
  uint8_t palette = 0;         // art::PALETTE_COUNT
  bool touchRepels = false;    // Particles: touch attracts (false) or repels (true)
};

struct ArtContext {
  ArtSurface &s;
  const art::Palette &pal;
  const ArtParams &p;
  art::Rng &rng;
  float t;                     // seconds of animation time (already speed-scaled)
};

struct ArtTouch {
  bool down = false;
  int x = 0, y = 0;
};

class ArtPattern {
 public:
  virtual const char *name() const = 0;
  // Called when the pattern becomes active (and after density changes).
  // Allocate state here (PSRAM, once) - never inside step().
  virtual bool begin(ArtContext &ctx) = 0;
  virtual void end() = 0;                                // free everything begin() allocated
  // Advance by dt seconds (speed-scaled) and draw into ctx.s.
  virtual void step(ArtContext &ctx, float dt, const ArtTouch &touch) = 0;
  // true: begin() must run again when density changes
  virtual bool densityNeedsRestart() const { return false; }
  // Optional short text shown over the art (e.g. the Christmas countdown). false = none.
  virtual bool caption(char *buf, size_t n) { (void)buf; (void)n; return false; }
};

ArtPattern &artPatternWaves();
ArtPattern &artPatternParticles();
ArtPattern &artPatternFlow();
ArtPattern &artPatternChristmas();
