// Native suite (computer, no board): F1 result window (result until 23:59 of race day).
#include <unity.h>
#include <string.h>
#include "modules/f1/f1_schedule.h"

void setUp() {}
void tearDown() {}

static const long BRT = -3 * 3600;
// 2026-11-22 04:00 UTC = Sunday 01:00 in Brasília (Las Vegas GP)
static const time_t LV = 1795320000;
static const time_t H = 3600, M = 60;

static void test_night_race_shows_result_until_midnight() {
  TEST_ASSERT_EQUAL(F1_UPCOMING, f1RacePhase(LV, LV - H, BRT));
  TEST_ASSERT_EQUAL(F1_UPCOMING, f1RacePhase(LV, LV + 90 * M, BRT));         // still racing (02:30)
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(LV, LV + 2 * H, BRT));        // 03:00: chequered flag
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(LV, LV + 12 * H, BRT));       // 13:00
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(LV, LV + 22 * H + 59 * M, BRT));   // 23:59
  TEST_ASSERT_EQUAL(F1_DONE, f1RacePhase(LV, LV + 23 * H, BRT));             // 00:00 Monday
  TEST_ASSERT_EQUAL((long)(LV + 23 * H), (long)f1ResultDayEnd(LV, BRT));
}

static void test_morning_and_afternoon_races() {
  const time_t nine = LV + 8 * H, two = LV + 13 * H;                         // Sunday 09:00 and 14:00 BRT
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(nine, nine + 2 * H, BRT));
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(nine, LV + 22 * H + 59 * M, BRT));
  TEST_ASSERT_EQUAL(F1_DONE, f1RacePhase(nine, LV + 23 * H, BRT));
  TEST_ASSERT_EQUAL(F1_UPCOMING, f1RacePhase(two, two + H, BRT));
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(two, two + 2 * H, BRT));
  TEST_ASSERT_EQUAL(F1_DONE, f1RacePhase(two, LV + 23 * H, BRT));
}

static void test_race_finishing_after_midnight_uses_finish_day() {
  const time_t late = LV - 2 * H;                                            // Saturday 23:00, ends Sunday 01:00
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(late, LV, BRT));
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(late, LV + 22 * H, BRT));
  TEST_ASSERT_EQUAL(F1_DONE, f1RacePhase(late, LV + 23 * H, BRT));
}

static void test_other_time_zones_and_missing_time() {
  TEST_ASSERT_EQUAL(F1_RESULT_DAY, f1RacePhase(LV, LV + 2 * H, 0));          // UTC: ends 06:00, same day
  TEST_ASSERT_EQUAL(F1_DONE, f1RacePhase(LV, LV + 20 * H, 0));               // UTC midnight = 21:00 BRT
  TEST_ASSERT_EQUAL(F1_UPCOMING, f1RacePhase(0, LV, BRT));                   // no race time
}

static void test_result_gap_column() {
  char b[24];
  TEST_ASSERT_EQUAL_STRING("1:27:39.624", f1ResultGap("1:27:39.624", "Finished", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("+0.639", f1ResultGap("+0.639", "Finished", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("+1 volta", f1ResultGap(nullptr, "+1 Lap", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("+2 voltas", f1ResultGap("", "+2 Laps", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("+1 volta", f1ResultGap(nullptr, "Lapped", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("abandonou", f1ResultGap("", "Retired", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("abandonou", f1ResultGap(nullptr, "Accident", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("n\xC3\xA3o largou", f1ResultGap(nullptr, "Did not start", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("desclassif.", f1ResultGap(nullptr, "Disqualified", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("", f1ResultGap(nullptr, "Finished", false, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("DNF", f1ResultGap(nullptr, "Retired", true, b, sizeof b));
  TEST_ASSERT_EQUAL_STRING("+3 laps", f1ResultGap(nullptr, "+3 Laps", true, b, sizeof b));
  char tiny[4];
  f1ResultGap("1:27:39.624", "Finished", false, tiny, sizeof tiny);        // never overflows
  TEST_ASSERT_EQUAL(3, (int)strlen(tiny));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_night_race_shows_result_until_midnight);
  RUN_TEST(test_morning_and_afternoon_races);
  RUN_TEST(test_race_finishing_after_midnight_uses_finish_day);
  RUN_TEST(test_other_time_zones_and_missing_time);
  RUN_TEST(test_result_gap_column);
  return UNITY_END();
}
