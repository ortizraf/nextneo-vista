#pragma once
// Garden ("Jardim") model: one plant that grows over real days, watered by a tap,
// affected by the live weather. Pure C++ (no Arduino/LVGL): unit-tested on the
// computer (test/test_native_garden). The screen saves State in Preferences ("garden").
#include <stdint.h>

namespace garden {

enum Stage : uint8_t { STAGE_SEED, STAGE_SPROUT, STAGE_YOUNG, STAGE_GROWN, STAGE_BUD, STAGE_BLOOM, STAGE_SEEDS, STAGE_COUNT };

// How the flower is drawn (garden_screen.cpp).
enum Shape : uint8_t { SHAPE_SUNFLOWER, SHAPE_HIBISCUS, SHAPE_LAVENDER, SHAPE_ORCHID, SHAPE_CACTUS, SHAPE_TULIP,
                       SHAPE_BOUGAINVILLEA, SHAPE_TOMATO };

struct Species {
  const char *pt, *en;
  uint32_t petal, petalDark, center;   // flower colours (light side, shaded side, centre)
  Shape shape;
  uint8_t bloomDays;                   // real days from seed to flower (when healthy)
  uint8_t dryPerDay;                   // water % lost per day at a mild temperature
  uint8_t wetLimit;                    // water % above this rots the roots (cactus: low)
  int8_t tMin, tMax;                   // comfortable air temperature (°C)
};
static constexpr int SPECIES_COUNT = 8;
extern const Species kSpecies[SPECIES_COUNT];

static constexpr uint32_t DAY_S = 86400;
static constexpr uint32_t OFFLINE_DRY_CAP_S = DAY_S;   // board off: at most 1 day of thirst
static constexpr float BLOOM_KEEP_DAYS = 7;            // flowers last a week, then seeds
static constexpr float WATER_STEP = 35;                // one tap of the watering can
static constexpr float THIRSTY_BELOW = 25;             // water % that shows the drop badge on the menu

// Saved as raw bytes: only append fields (version guards the layout).
struct State {
  uint8_t version = 1;
  uint8_t species = 0xFF;      // 0xFF = nothing planted yet
  uint8_t alive = 0;
  uint8_t seenStage = 0;       // last stage shown to the owner (stage-up celebration)
  uint32_t plantedAt = 0;      // epoch seconds
  uint32_t lastTick = 0;       // epoch of the last advance()
  float water = 0;             // 0..100 %
  float health = 0;            // 0..100 %
  float growDays = 0;          // effective growth (slower when unhealthy)
  uint16_t album = 0;          // bit i = species i has already bloomed
  uint16_t reserved2 = 0;
};

struct Weather {
  bool known = false;
  float tempC = 0;
  bool raining = false;
};

bool hasPlant(const State &s);
void plant(State &s, uint8_t species, uint32_t now);
void water(State &s);
// Moves the plant to `now`. offlineS = the part of the gap (right after lastTick) when
// the board was switched off: it grows, but thirst counts for OFFLINE_DRY_CAP_S at most.
void advance(State &s, uint32_t now, uint32_t offlineS, const Weather &w);
Stage stage(const State &s);
float stageProgress(const State &s);   // 0..1 inside the current stage (stem height)
int dayNumber(const State &s);         // "Dia N" (1 = planting day), real days since planting
float daysToBloom(const State &s);     // remaining effective days (0 when blooming)

enum Mood : uint8_t { MOOD_OK, MOOD_THIRSTY, MOOD_SOAKED, MOOD_HOT, MOOD_COLD, MOOD_DEAD };
Mood mood(const State &s, const Weather &w);
bool wilting(const State &s, const Weather &w);   // drawn with drooping leaves
bool needsWater(const State &s);                  // alive and thirsty: drop badge on the menu icon
// Stage-up celebration: true once per new stage (from sprout on), until markStageSeen().
bool newStageReached(const State &s);
void markStageSeen(State &s);

const char *stageName(Stage st, bool en);
const char *moodText(Mood m, bool en);

}  // namespace garden
