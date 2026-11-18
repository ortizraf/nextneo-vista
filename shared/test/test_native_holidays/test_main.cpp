// Native suite (computer, no board): holidays parsing and cache rules (BrasilAPI).
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "modules/calendar/holidays_model.h"
#include "modules/calendar/calendar_cats.h"

void setUp() {}
void tearDown() {}

static const uint32_t NOW = 1795000000UL;   // 2026-11-18, a valid clock
static const char *kJson2026 =
  "[{\"date\":\"2026-01-01\",\"name\":\"Confraternização mundial\",\"type\":\"national\"},"
  "{\"date\":\"2026-02-16\",\"name\":\"Carnaval\",\"type\":\"national\"},"
  "{\"date\":\"2026-02-17\",\"name\":\"Carnaval\",\"type\":\"national\"},"
  "{\"date\":\"2026-04-03\",\"name\":\"Sexta-feira Santa\",\"type\":\"national\"},"
  "{\"date\":\"2026-11-15\",\"name\":\"Proclamação da República\",\"type\":\"national\"},"
  "{\"date\":\"2026-11-20\",\"name\":\"Dia da consciência negra\",\"type\":\"national\"},"
  "{\"date\":\"2026-12-25\",\"name\":\"Natal\",\"type\":\"national\"}]";

static void test_parse_valid_year() {
  HolidayList l;
  TEST_ASSERT_TRUE(parseHolidaysJson(kJson2026, strlen(kJson2026), 2026, NOW, l));
  TEST_ASSERT_EQUAL_UINT8(7, l.n);
  TEST_ASSERT_EQUAL_UINT32(NOW, l.fetchedAt);
  TEST_ASSERT_TRUE(holidayListHas(l, 1, 1));
  TEST_ASSERT_TRUE(holidayListHas(l, 2, 17));
  TEST_ASSERT_TRUE(holidayListHas(l, 11, 20));
  TEST_ASSERT_TRUE(holidayListHas(l, 12, 25));
  TEST_ASSERT_FALSE(holidayListHas(l, 11, 17));
  TEST_ASSERT_FALSE(holidayListHas(l, 12, 24));
}

static void test_parse_ignores_other_years_and_bad_dates() {
  const char *j = "[{\"date\":\"2025-12-25\"},{\"date\":\"2026-13-01\"},{\"date\":\"2026-02-00\"},"
                  "{\"date\":\"bad\"},{\"name\":\"no date\"},{\"date\":\"2026-09-07\"},{\"date\":\"2026-09-07\"}]";
  HolidayList l;
  TEST_ASSERT_TRUE(parseHolidaysJson(j, strlen(j), 2026, NOW, l));
  TEST_ASSERT_EQUAL_UINT8(1, l.n);                   // only 07/09, once
  TEST_ASSERT_TRUE(holidayListHas(l, 9, 7));
  TEST_ASSERT_FALSE(holidayListHas(l, 12, 25));
}

static void test_parse_rejects_invalid_input() {
  HolidayList l;
  TEST_ASSERT_FALSE(parseHolidaysJson(nullptr, 0, 2026, NOW, l));
  TEST_ASSERT_FALSE(parseHolidaysJson("", 0, 2026, NOW, l));
  TEST_ASSERT_FALSE(parseHolidaysJson("{\"type\":\"error\"}", 17, 2026, NOW, l));   // error object
  TEST_ASSERT_FALSE(parseHolidaysJson("[]", 2, 2026, NOW, l));
  TEST_ASSERT_FALSE(parseHolidaysJson("[{", 2, 2026, NOW, l));
  TEST_ASSERT_FALSE(parseHolidaysJson(kJson2026, strlen(kJson2026), 2027, NOW, l));   // wrong year
}

static void test_parse_caps_at_max_days() {
  char j[4096] = "[";
  for (int i = 0; i < 40; ++i) {
    char e[48]; snprintf(e, sizeof e, "%s{\"date\":\"2026-%02d-%02d\"}", i ? "," : "", 1 + i / 28, 1 + i % 28);
    strcat(j, e);
  }
  strcat(j, "]");
  HolidayList l;
  TEST_ASSERT_TRUE(parseHolidaysJson(j, strlen(j), 2026, NOW, l));
  TEST_ASSERT_EQUAL_UINT8(HOLIDAY_MAX_DAYS, l.n);
}

static void test_cache_staleness() {
  HolidayList l;
  parseHolidaysJson(kJson2026, strlen(kJson2026), 2026, NOW, l);
  TEST_ASSERT_TRUE(holidayCacheStale(false, l, NOW));                  // nothing cached
  TEST_ASSERT_FALSE(holidayCacheStale(true, l, NOW));                  // fresh
  TEST_ASSERT_FALSE(holidayCacheStale(true, l, NOW + 29UL * 86400UL)); // 29 days: keep
  TEST_ASSERT_TRUE(holidayCacheStale(true, l, NOW + 31UL * 86400UL));  // 31 days: refresh
  TEST_ASSERT_FALSE(holidayCacheStale(true, l, 1000));                 // clock not set: use cache
  TEST_ASSERT_TRUE(holidayCacheStale(true, l, NOW - 86400UL));         // clock went back: refresh
  HolidayList empty{};
  TEST_ASSERT_TRUE(holidayCacheStale(true, empty, NOW));               // empty blob
  HolidayList broken = l; broken.n = 200;
  TEST_ASSERT_TRUE(holidayCacheStale(true, broken, NOW));              // corrupt blob
}

static void test_calendar_cat_from_form() {
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg("0"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_TRICOLOR, calendarCatFromArg("1"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_LARANJA, calendarCatFromArg("2"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_CINZA, calendarCatFromArg("3"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_RANDOM, calendarCatFromArg("4"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg("5"));      // out of range
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg("-1"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg("2x"));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg(""));
  TEST_ASSERT_EQUAL_UINT8(CAL_CAT_FRAJOLA, calendarCatFromArg(nullptr));
}

static void test_calendar_cat_names() {
  TEST_ASSERT_EQUAL_STRING("Frajola", calendarCatName(CAL_CAT_FRAJOLA));
  TEST_ASSERT_EQUAL_STRING("Tricolor", calendarCatName(CAL_CAT_TRICOLOR));
  TEST_ASSERT_EQUAL_STRING("Laranja", calendarCatName(CAL_CAT_LARANJA));
  TEST_ASSERT_EQUAL_STRING("Cinza rajado", calendarCatName(CAL_CAT_CINZA));
  TEST_ASSERT_EQUAL_STRING("Aleat\xC3\xB3rio", calendarCatName(CAL_CAT_RANDOM));
  TEST_ASSERT_EQUAL_STRING("Frajola", calendarCatName(99));
}

static void test_calendar_cat_resolve() {
  for (uint8_t c = 0; c < CAL_CAT_ART_COUNT; ++c)                      // fixed choice: always that cat
    TEST_ASSERT_EQUAL_UINT8(c, calendarCatResolve(c, 2, 12345));
  bool seen[CAL_CAT_ART_COUNT] = {};
  uint8_t prev = CAL_CAT_ART_COUNT;
  for (uint32_t r = 0; r < 200; ++r) {                                  // random: valid, never repeats
    const uint8_t c = calendarCatResolve(CAL_CAT_RANDOM, prev, r * 2654435761u);
    TEST_ASSERT_LESS_THAN_UINT8(CAL_CAT_ART_COUNT, c);
    if (prev < CAL_CAT_ART_COUNT) TEST_ASSERT_NOT_EQUAL(prev, c);
    seen[c] = true; prev = c;
  }
  for (bool s : seen) TEST_ASSERT_TRUE(s);                              // every cat shows up
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid_year);
  RUN_TEST(test_parse_ignores_other_years_and_bad_dates);
  RUN_TEST(test_parse_rejects_invalid_input);
  RUN_TEST(test_parse_caps_at_max_days);
  RUN_TEST(test_cache_staleness);
  RUN_TEST(test_calendar_cat_from_form);
  RUN_TEST(test_calendar_cat_names);
  RUN_TEST(test_calendar_cat_resolve);
  return UNITY_END();
}
