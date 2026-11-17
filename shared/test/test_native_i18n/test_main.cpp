// Native suite (computer, no board): Portuguese (default) / English texts.
#include <unity.h>
#include "core/i18n.h"
#include "modules/weather/weather_codes.h"

void setUp() { gLang = LANG_PT; }
void tearDown() { gLang = LANG_PT; }

static void test_portuguese_is_the_default() {
  TEST_ASSERT_EQUAL(LANG_PT, gLang);
  TEST_ASSERT_EQUAL_STRING("Chuva", T("Chuva", "Rain"));
}

static void test_english_when_chosen() {
  gLang = LANG_EN;
  TEST_ASSERT_EQUAL_STRING("Rain", T("Chuva", "Rain"));
}

static void test_language_names_and_codes() {
  TEST_ASSERT_EQUAL_STRING("Portugu\xC3\xAAs", langName(LANG_PT));
  TEST_ASSERT_EQUAL_STRING("English", langName(LANG_EN));
  TEST_ASSERT_EQUAL_STRING("en", langCode(LANG_EN));
}

static void test_calendar_names_follow_the_language() {
  TEST_ASSERT_EQUAL_STRING("S\xC3\xA1" "b", weekdayShort(6));
  TEST_ASSERT_EQUAL_STRING("mar\xC3\xA7o", monthLong(2));
  TEST_ASSERT_EQUAL_STRING("DOM", weekdayUpper(0));
  gLang = LANG_EN;
  TEST_ASSERT_EQUAL_STRING("Sat", weekdayShort(6));
  TEST_ASSERT_EQUAL_STRING("March", monthLong(2));
  TEST_ASSERT_EQUAL_STRING("Wednesday", weekdayLong(3));
  TEST_ASSERT_EQUAL_STRING("Dec", monthShort(11));
}

static void test_out_of_range_is_empty_not_a_crash() {
  TEST_ASSERT_EQUAL_STRING("", weekdayShort(7));
  TEST_ASSERT_EQUAL_STRING("", monthLong(-1));
}

static void test_weather_texts_in_both_languages() {
  TEST_ASSERT_EQUAL_STRING("Garoa", weatherText(53));
  TEST_ASSERT_EQUAL_STRING("C\xC3\xA9u limpo", weatherText(0, true));
  gLang = LANG_EN;
  TEST_ASSERT_EQUAL_STRING("Drizzle", weatherText(53));
  TEST_ASSERT_EQUAL_STRING("Clear sky", weatherText(0, true));
  TEST_ASSERT_EQUAL_STRING("Sunny", weatherText(0));
  TEST_ASSERT_EQUAL_STRING("No data", weatherText(42));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_portuguese_is_the_default);
  RUN_TEST(test_english_when_chosen);
  RUN_TEST(test_language_names_and_codes);
  RUN_TEST(test_calendar_names_follow_the_language);
  RUN_TEST(test_out_of_range_is_empty_not_a_crash);
  RUN_TEST(test_weather_texts_in_both_languages);
  return UNITY_END();
}
