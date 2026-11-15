// Native suite (computer, no board): Sky screen orbits (TLE parsing + SGP4).
// Uses the classic public ISS example TLE from 2008 (format documentation).
#include <unity.h>
#include <math.h>
#include <string.h>
#include "modules/sky/sky_orbit.h"

using namespace sky;

static const char *kStations =
  "ISS (ZARYA)\n"
  "1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927\n"
  "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537\n"
  "SOME DEBRIS\n"
  "1 99999U 98067B   08264.51782528 -.00002182  00000-0 -11606-4 0  2920\n"
  "2 99999  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563530\n";

static const char *kLast30Csv =
  "OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,EPHEMERIS_TYPE,CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT\n"
  "PULSAR-01,2026-237A,2026-11-14T01:47:00.575808,14.92872186,.00069779,52.9879,214.5103,327.3557,32.6993,0,U,101111,999,368,.16380381E-3,.1465E-4,0\n"
  "STARLINK-35001,2026-240A,2026-11-14T06:00:00.000000,15.80000000,.00012000,43.0020,10.5000,90.0000,270.0000,0,U,101200,999,20,.50000000E-3,.1E-4,0\n";

static Sat sats[4];

void setUp() { memset(sats, 0, sizeof sats); }
void tearDown() {}

static void test_stations_mode_keeps_only_the_iss() {
  TEST_ASSERT_EQUAL(1, parseTle(kStations, sats, 0, 4, 0));
  TEST_ASSERT_EQUAL_STRING("ISS", sats[0].name);
  TEST_ASSERT_EQUAL(SAT_ISS, sats[0].kind);
}

static void test_orbital_elements_are_read() {
  parseTle(kStations, sats, 0, 4, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 51.6416, sats[0].inclo * 180.0 / M_PI);
  TEST_ASSERT_FLOAT_WITHIN(0.000001, 0.0006703, sats[0].ecco);
  // mean motion: 15.72 rev/day in rad/min; sgp4Init then removes the Kozai correction
  // ("un-Kozai", standard SGP4), so it moves slightly: 0.068597 -> 0.068589
  TEST_ASSERT_FLOAT_WITHIN(2e-5, 15.72125391 * 2 * M_PI / 1440.0, sats[0].no);
  TEST_ASSERT_TRUE(sats[0].bstar < 0);              // "-11606-4" -> -0.11606e-4
}

static void test_visual_mode_skips_the_iss() {
  // the ISS comes from "stations"; "visual" must not add it twice
  TEST_ASSERT_EQUAL(1, parseTle(kStations, sats, 0, 4, 1));
  TEST_ASSERT_EQUAL_STRING("SOME DEBRIS", sats[0].name);
}

static void test_capacity_is_respected() {
  TEST_ASSERT_EQUAL(1, parseTle(kStations, sats, 0, 1, 1));
  TEST_ASSERT_EQUAL(3, parseTle(kStations, sats, 3, 3, 1));   // already full: unchanged
}

static void test_sgp4_puts_the_iss_in_low_orbit() {
  parseTle(kStations, sats, 0, 4, 0);
  double r[3];
  const double times[] = {0.0, 45.0, 90.0};                    // minutes after the epoch
  for (double t : times) {
    TEST_ASSERT_TRUE(sgp4(sats[0], t, r));
    const double radius = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    TEST_ASSERT_FLOAT_WITHIN(150.0, RE_KM + 350.0, radius);  // ~350 km up in 2008
  }
}

static void test_garbage_is_ignored() {
  TEST_ASSERT_EQUAL(0, parseTle("<html>erro</html>\n", sats, 0, 4, 1));
  TEST_ASSERT_EQUAL(0, parseTle("", sats, 0, 4, 1));
}

// New launches (catalog >= 100000) only come as CSV: the Starlink row is kept, others not.
static void test_csv_last30_keeps_starlinks_with_6_digit_ids() {
  TEST_ASSERT_EQUAL(1, parseOmmCsv(kLast30Csv, sats, 0, 4, 2));
  TEST_ASSERT_EQUAL_STRING("STARLINK-35001", sats[0].name);
  TEST_ASSERT_EQUAL(SAT_STARLINK, sats[0].kind);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 43.0020, sats[0].inclo * 180.0 / M_PI);
  double r[3];
  TEST_ASSERT_TRUE(sgp4(sats[0], 0, r));
  const double radius = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  TEST_ASSERT_FLOAT_WITHIN(200.0, RE_KM + 300.0, radius);   // ~15.8 rev/day: very low orbit
}

static void test_csv_error_page_is_ignored() {
  TEST_ASSERT_EQUAL(0, parseOmmCsv("No GP data found", sats, 0, 4, 2));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_stations_mode_keeps_only_the_iss);
  RUN_TEST(test_orbital_elements_are_read);
  RUN_TEST(test_visual_mode_skips_the_iss);
  RUN_TEST(test_capacity_is_respected);
  RUN_TEST(test_sgp4_puts_the_iss_in_low_orbit);
  RUN_TEST(test_garbage_is_ignored);
  RUN_TEST(test_csv_last30_keeps_starlinks_with_6_digit_ids);
  RUN_TEST(test_csv_error_page_is_ignored);
  return UNITY_END();
}
