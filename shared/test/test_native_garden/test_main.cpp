// Native suite (computer, no board): Garden plant model.
#include <unity.h>
#include "modules/garden/garden_model.h"

using namespace garden;

static const uint32_t T0 = 1800000000;   // any synced epoch
static const Weather kMild = {true, 22, false};

void setUp() {}
void tearDown() {}

static void test_new_state_has_no_plant() {
  State s;
  TEST_ASSERT_FALSE(hasPlant(s));
  plant(s, 0, T0);
  TEST_ASSERT_TRUE(hasPlant(s));
  TEST_ASSERT_EQUAL(STAGE_SEED, stage(s));
  TEST_ASSERT_EQUAL(1, dayNumber(s));
}

static void test_watered_sunflower_blooms_after_its_days() {
  State s; plant(s, 0, T0);                       // sunflower: 21 days
  for (int d = 1; d <= 22; ++d) { water(s); advance(s, T0 + d * DAY_S, 0, kMild); }
  TEST_ASSERT_EQUAL(STAGE_BLOOM, stage(s));
  TEST_ASSERT_TRUE(s.album & 1);
  TEST_ASSERT_EQUAL(23, dayNumber(s));
}

static void test_forgotten_plant_dries_up() {
  State s; plant(s, 7, T0);                       // tomato: thirstiest
  advance(s, T0 + 10 * DAY_S, 0, kMild);
  TEST_ASSERT_EQUAL(0, s.alive);
  TEST_ASSERT_EQUAL(MOOD_DEAD, mood(s, kMild));
}

static void test_board_off_counts_at_most_one_day_of_thirst() {
  State a; plant(a, 0, T0);
  State b = a;
  advance(a, T0 + 10 * DAY_S, 10 * DAY_S, kMild);   // switched off for 10 days
  advance(b, T0 + DAY_S, 0, kMild);                 // on for 1 day
  TEST_ASSERT_EQUAL(1, a.alive);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, b.water, a.water);
  TEST_ASSERT_TRUE(a.growDays > 9);                 // but it kept growing
}

static void test_heat_dries_faster_and_rain_waters() {
  State hot; plant(hot, 0, T0);
  State mild = hot, rain = hot;
  advance(hot, T0 + DAY_S / 2, 0, {true, 34, false});
  advance(mild, T0 + DAY_S / 2, 0, kMild);
  advance(rain, T0 + DAY_S / 2, 0, {true, 22, true});
  TEST_ASSERT_TRUE(hot.water < mild.water);
  TEST_ASSERT_TRUE(rain.water > mild.water);
}

static void test_cactus_rots_when_soaked() {
  State s; plant(s, 4, T0);                       // cactus wet limit 55 %
  for (int d = 1; d <= 6; ++d) { water(s); water(s); advance(s, T0 + d * DAY_S, 0, kMild); }
  TEST_ASSERT_TRUE(s.health < 100);
}

static void test_replanting_keeps_the_album() {
  State s; plant(s, 0, T0);
  s.album = 0x5;
  plant(s, 2, T0 + 5);
  TEST_ASSERT_EQUAL_HEX16(0x5, s.album);
  TEST_ASSERT_EQUAL(2, s.species);
}

static void test_thirsty_plant_needs_water_dead_does_not() {
  State s; plant(s, 0, T0);
  TEST_ASSERT_FALSE(needsWater(s));                 // planted with 70 %
  s.water = THIRSTY_BELOW - 1;
  TEST_ASSERT_TRUE(needsWater(s));
  s.alive = 0;
  TEST_ASSERT_FALSE(needsWater(s));                 // dried up: replant, not water
  TEST_ASSERT_FALSE(needsWater(State()));
}

static void test_new_stage_is_celebrated_once() {
  State s; plant(s, 0, T0);
  markStageSeen(s);
  TEST_ASSERT_FALSE(newStageReached(s));            // seed: nothing to celebrate
  s.growDays = 3;                                   // sunflower sprout
  TEST_ASSERT_TRUE(newStageReached(s));
  markStageSeen(s);
  TEST_ASSERT_EQUAL(STAGE_SPROUT, s.seenStage);
  TEST_ASSERT_FALSE(newStageReached(s));
  s.growDays = 22; s.alive = 0;
  TEST_ASSERT_FALSE(newStageReached(s));            // a dead plant has no party
  plant(s, 1, T0);
  TEST_ASSERT_EQUAL(0, s.seenStage);                // replanting starts again
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_new_state_has_no_plant);
  RUN_TEST(test_watered_sunflower_blooms_after_its_days);
  RUN_TEST(test_forgotten_plant_dries_up);
  RUN_TEST(test_board_off_counts_at_most_one_day_of_thirst);
  RUN_TEST(test_heat_dries_faster_and_rain_waters);
  RUN_TEST(test_cactus_rots_when_soaked);
  RUN_TEST(test_replanting_keeps_the_album);
  RUN_TEST(test_thirsty_plant_needs_water_dead_does_not);
  RUN_TEST(test_new_stage_is_celebrated_once);
  return UNITY_END();
}
