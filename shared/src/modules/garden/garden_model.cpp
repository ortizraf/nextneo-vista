// Garden model (see garden_model.h). Pure C++, unit-tested on the computer.
#include "modules/garden/garden_model.h"

namespace garden {

// name pt/en, petal light / shaded / centre, shape, days to bloom, dry %/day, wet limit, tMin, tMax
const Species kSpecies[SPECIES_COUNT] = {
  {"Girassol",   "Sunflower",     0xFFC21E, 0xD9820C, 0x4A2A10, SHAPE_SUNFLOWER,     21, 30, 95,  10, 35},
  {"Hibisco",    "Hibiscus",      0xF0384A, 0xA3162A, 0xF6D24C, SHAPE_HIBISCUS,      28, 28, 95,  12, 36},
  {"Lavanda",    "Lavender",      0xA88BE6, 0x6448B4, 0x4E3A8C, SHAPE_LAVENDER,      35, 20, 80,   0, 33},
  {"Orqu\xC3\xAD" "dea", "Orchid", 0xF58AC0, 0xC2508E, 0xF7E27A, SHAPE_ORCHID,        42, 15, 75,  15, 30},
  {"Cacto",      "Cactus",        0xFF6FA6, 0xD23A72, 0xFFE07A, SHAPE_CACTUS,        42,  8, 55,   5, 42},
  {"Tulipa",     "Tulip",         0xFF6A3D, 0xC23A1A, 0x3A2A10, SHAPE_TULIP,         28, 30, 90,  -5, 24},
  {"Buganv\xC3\xADlia", "Bougainvillea", 0xE447B4, 0x9C1E7C, 0xFFF3C4, SHAPE_BOUGAINVILLEA, 35, 25, 85, 8, 40},
  {"Tomateiro",  "Tomato",        0xEE4A30, 0xA82A18, 0xF7D23A, SHAPE_TOMATO,        35, 40, 98,  12, 34},
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool hasPlant(const State &s) { return s.species < SPECIES_COUNT; }

void plant(State &s, uint8_t species, uint32_t now) {
  const uint16_t album = s.album;
  s = State();
  s.album = album;
  if (species >= SPECIES_COUNT) return;
  s.species = species;
  s.alive = 1;
  s.plantedAt = s.lastTick = now;
  s.water = 70;
  s.health = 100;
}

void water(State &s) {
  if (!hasPlant(s) || !s.alive) return;
  s.water = clampf(s.water + WATER_STEP, 0, 100);
}

// Loss/gain of health per day for the current conditions.
static float healthPerDay(const Species &sp, float waterPct, const Weather &w, bool useWeather) {
  float d = 20;                                         // recovers when all is fine
  if (waterPct < 10) d = -40;
  else if (waterPct < 25) d = -15;
  else if (waterPct > sp.wetLimit) d = -20;
  if (useWeather && w.known && (w.tempC > sp.tMax + 2 || w.tempC < sp.tMin - 2)) d -= 10;
  return d;
}

void advance(State &s, uint32_t now, uint32_t offlineS, const Weather &w) {
  if (!hasPlant(s) || s.lastTick == 0 || now <= s.lastTick) { if (now > s.lastTick) s.lastTick = now; return; }
  if (!s.alive) { s.lastTick = now; return; }
  const Species &sp = kSpecies[s.species];
  uint32_t gap = now - s.lastTick;
  if (offlineS > gap) offlineS = gap;
  uint32_t pos = 0, offlineDry = 0;
  while (pos < gap && s.alive) {                          // hour steps: health reacts as water changes
    const uint32_t step = gap - pos < 3600 ? gap - pos : 3600;
    const bool offline = pos < offlineS;
    bool counts = true;                                   // thirst/health clock running
    if (offline) { counts = offlineDry < OFFLINE_DRY_CAP_S; offlineDry += step; }
    const float frac = step / (float)DAY_S;
    if (counts) {
      float dry = sp.dryPerDay;
      if (!offline && w.known) {
        if (w.tempC > 30) dry *= 2;                       // strong heat dries the soil twice as fast
        else if (w.tempC > 25) dry *= 1.4f;
        else if (w.tempC < 15) dry *= 0.6f;
      }
      s.water = clampf(s.water - dry * frac, 0, 100);
      if (!offline && w.known && w.raining) s.water = clampf(s.water + 240 * frac, 0, 100);   // +10 %/h in the rain
      s.health = clampf(s.health + healthPerDay(sp, s.water, w, !offline) * frac, 0, 100);
      if (s.health <= 0) { s.alive = 0; break; }
    }
    const float speed = s.health >= 50 ? 1.0f : s.health >= 20 ? 0.5f : 0.0f;
    s.growDays += speed * frac;
    if (s.growDays >= sp.bloomDays) s.album |= (uint16_t)(1u << s.species);
    pos += step;
  }
  s.lastTick = now;
}

// Stage boundaries as a fraction of bloomDays.
static const float kStageEnd[STAGE_COUNT] = {0.08f, 0.25f, 0.50f, 0.75f, 1.0f, 0, 0};

Stage stage(const State &s) {
  if (!hasPlant(s)) return STAGE_SEED;
  const float bd = kSpecies[s.species].bloomDays;
  const float f = s.growDays / bd;
  for (int i = 0; i < STAGE_BLOOM; ++i) if (f < kStageEnd[i]) return (Stage)i;
  return s.growDays < bd + BLOOM_KEEP_DAYS ? STAGE_BLOOM : STAGE_SEEDS;
}

float stageProgress(const State &s) {
  if (!hasPlant(s)) return 0;
  const Stage st = stage(s);
  if (st >= STAGE_BLOOM) return 1;
  const float f = s.growDays / kSpecies[s.species].bloomDays;
  const float lo = st == STAGE_SEED ? 0 : kStageEnd[st - 1], hi = kStageEnd[st];
  return clampf((f - lo) / (hi - lo), 0, 1);
}

int dayNumber(const State &s) {
  if (!hasPlant(s) || s.lastTick < s.plantedAt) return 1;
  return (int)((s.lastTick - s.plantedAt) / DAY_S) + 1;
}

float daysToBloom(const State &s) {
  if (!hasPlant(s)) return 0;
  const float r = kSpecies[s.species].bloomDays - s.growDays;
  return r > 0 ? r : 0;
}

Mood mood(const State &s, const Weather &w) {
  if (!hasPlant(s)) return MOOD_OK;
  if (!s.alive) return MOOD_DEAD;
  const Species &sp = kSpecies[s.species];
  if (s.water < THIRSTY_BELOW) return MOOD_THIRSTY;
  if (s.water > sp.wetLimit) return MOOD_SOAKED;
  if (w.known && w.tempC > sp.tMax) return MOOD_HOT;
  if (w.known && w.tempC < sp.tMin) return MOOD_COLD;
  return MOOD_OK;
}

bool wilting(const State &s, const Weather &w) {
  if (!hasPlant(s)) return false;
  const Mood m = mood(s, w);
  return m == MOOD_DEAD || m == MOOD_THIRSTY || m == MOOD_HOT || s.health < 40;
}

bool needsWater(const State &s) { return hasPlant(s) && s.alive && s.water < THIRSTY_BELOW; }

bool newStageReached(const State &s) {
  return hasPlant(s) && s.alive && stage(s) > STAGE_SEED && stage(s) > s.seenStage;
}
void markStageSeen(State &s) { if (hasPlant(s)) s.seenStage = stage(s); }

const char *stageName(Stage st, bool en) {
  static const char *const kNames[STAGE_COUNT][2] = {
    {"semente", "seed"}, {"broto", "sprout"}, {"muda", "seedling"}, {"crescendo", "growing"},
    {"bot\xC3\xB5" "es", "buds"}, {"florida", "in bloom"}, {"sementes prontas", "seeds ready"}};
  return kNames[st < STAGE_COUNT ? st : 0][en ? 1 : 0];
}

const char *moodText(Mood m, bool en) {
  switch (m) {
    case MOOD_THIRSTY: return en ? "Thirsty" : "Com sede";
    case MOOD_SOAKED:  return en ? "Soil too wet" : "Terra encharcada";
    case MOOD_HOT:     return en ? "Too hot" : "Muito calor";
    case MOOD_COLD:    return en ? "Too cold" : "Muito frio";
    case MOOD_DEAD:    return en ? "Dried up" : "Secou";
    default:           return en ? "Healthy" : "Saud\xC3\xA1vel";
  }
}

}  // namespace garden
