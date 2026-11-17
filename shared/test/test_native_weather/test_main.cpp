// Native suite (computer, no board): WMO weather codes -> text / rain level.
#include <unity.h>
#include "modules/weather/weather_codes.h"

void setUp() {}
void tearDown() {}

static void test_clear_sky_depends_on_day_or_night() {
  TEST_ASSERT_EQUAL_STRING("Ensolarado", weatherText(0));
  TEST_ASSERT_EQUAL_STRING("C\xC3\xA9u limpo", weatherText(0, true));
}

static void test_known_code_groups() {
  TEST_ASSERT_EQUAL_STRING("Poucas nuvens", weatherText(1));
  TEST_ASSERT_EQUAL_STRING("Nublado", weatherText(3));
  TEST_ASSERT_EQUAL_STRING("Neblina", weatherText(45));
  TEST_ASSERT_EQUAL_STRING("Garoa", weatherText(53));
  TEST_ASSERT_EQUAL_STRING("Chuva", weatherText(63));
  TEST_ASSERT_EQUAL_STRING("Pancadas", weatherText(81));
  TEST_ASSERT_EQUAL_STRING("Tempestade", weatherText(95));
}

static void test_unknown_code_has_fallback_text() {
  TEST_ASSERT_EQUAL_STRING("Sem dados", weatherText(42));
  TEST_ASSERT_EQUAL_STRING("Sem dados", weatherText(-1));
}

static void test_is_rain_code() {
  TEST_ASSERT_TRUE(isRainCode(51));
  TEST_ASSERT_TRUE(isRainCode(65));
  TEST_ASSERT_TRUE(isRainCode(82));
  TEST_ASSERT_TRUE(isRainCode(99));
  TEST_ASSERT_FALSE(isRainCode(3));
  TEST_ASSERT_FALSE(isRainCode(71));   // snow is not rain
}

static void test_rain_level() {
  TEST_ASSERT_EQUAL(0, rainLevel(2));
  TEST_ASSERT_EQUAL(1, rainLevel(55));   // garoa
  TEST_ASSERT_EQUAL(2, rainLevel(61));   // chuva
  TEST_ASSERT_EQUAL(3, rainLevel(65));   // chuva forte
  TEST_ASSERT_EQUAL(3, rainLevel(96));   // tempestade
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clear_sky_depends_on_day_or_night);
  RUN_TEST(test_known_code_groups);
  RUN_TEST(test_unknown_code_has_fallback_text);
  RUN_TEST(test_is_rain_code);
  RUN_TEST(test_rain_level);
  return UNITY_END();
}
