// Native suite (computer, no board): Flight Radar distance, filter, projection and
// the ADSB.lol JSON parser. Uses public airport coordinates (Congonhas, CGH).
#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "modules/flight_radar/flight_filter.h"
#include "modules/flight_radar/flight_parser.h"
#include "modules/flight_radar/flight_projection.h"

static constexpr double CGH_LAT = -23.626111, CGH_LON = -46.656389;

void setUp() {}
void tearDown() {}

static Aircraft at(const char *hex, double lat, double lon) {
  Aircraft a; strncpy(a.icao24, hex, sizeof a.icao24 - 1); a.latitude = lat; a.longitude = lon; return a;
}

static void test_distance_zero_and_one_degree() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, aircraftDistanceNm(CGH_LAT, CGH_LON, CGH_LAT, CGH_LON));
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 60.0f, aircraftDistanceNm(0, 0, 1, 0));   // 1° of latitude ~ 60 NM
}

static void test_filter_keeps_nearest_first_within_radius() {
  Aircraft list[3] = {at("far", CGH_LAT + 0.5, CGH_LON),      // ~30 NM: out
                      at("mid", CGH_LAT + 0.05, CGH_LON),     // ~3 NM
                      at("near", CGH_LAT + 0.01, CGH_LON)};   // ~0.6 NM
  const uint8_t kept = filterAircraft(list, 3, CGH_LAT, CGH_LON, 10.0f);
  TEST_ASSERT_EQUAL(2, kept);
  TEST_ASSERT_EQUAL_STRING("near", list[0].icao24);
  TEST_ASSERT_EQUAL_STRING("mid", list[1].icao24);
}

static void test_filter_drops_stale_positions() {
  Aircraft list[1] = {at("old", CGH_LAT, CGH_LON)};
  list[0].hasPositionAge = true; list[0].positionAgeSeconds = 200;
  TEST_ASSERT_EQUAL(0, filterAircraft(list, 1, CGH_LAT, CGH_LON, 10.0f));
}

static void test_insert_nearest_ignores_duplicates_and_caps() {
  Aircraft list[2];
  Aircraft a = at("aaa", 0, 0); a.distanceNm = 5;
  Aircraft b = at("bbb", 0, 0); b.distanceNm = 2;
  Aircraft c = at("ccc", 0, 0); c.distanceNm = 9;
  uint8_t n = insertNearest(list, 0, 2, a);
  n = insertNearest(list, n, 2, b);
  n = insertNearest(list, n, 2, a);                 // duplicate
  n = insertNearest(list, n, 2, c);                 // farther than the last: dropped
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("bbb", list[0].icao24);
  TEST_ASSERT_EQUAL_STRING("aaa", list[1].icao24);
}

static void test_projection_north_is_up_east_is_right() {
  int x, y;
  projectAircraft(at("c", CGH_LAT, CGH_LON), CGH_LAT, CGH_LON, 10, 100, 100, 50, x, y);
  TEST_ASSERT_EQUAL(100, x); TEST_ASSERT_EQUAL(100, y);
  projectAircraft(at("n", CGH_LAT + 0.05, CGH_LON), CGH_LAT, CGH_LON, 10, 100, 100, 50, x, y);
  TEST_ASSERT_EQUAL(100, x); TEST_ASSERT_LESS_THAN(100, y);
  projectAircraft(at("e", CGH_LAT, CGH_LON + 0.05), CGH_LAT, CGH_LON, 10, 100, 100, 50, x, y);
  TEST_ASSERT_GREATER_THAN(100, x); TEST_ASSERT_EQUAL(100, y);
}

static void test_parser_reads_fields_and_trims() {
  JsonDocument doc;
  deserializeJson(doc, R"({"ac":[
    {"hex":"e48a1c","flight":"TAM4732 ","t":"A320","lat":-23.6,"lon":-46.6,"alt_baro":3300,"gs":140,"track":170,"squawk":"1234"},
    {"hex":"bad","lat":95,"lon":0},
    {"hex":"gnd","flight":"GLO1","lat":-23.62,"lon":-46.65,"alt_baro":"ground"},
    {"hex":"sos","lat":-23.6,"lon":-46.6,"squawk":"7700"}]})");
  Aircraft out[8]; uint8_t n = 0;
  TEST_ASSERT_TRUE(parseAdsbLolAircraft(doc["ac"].as<JsonArrayConst>(), out, 8, n));
  TEST_ASSERT_EQUAL(3, n);                          // the invalid latitude is skipped
  TEST_ASSERT_EQUAL_STRING("TAM4732", out[0].callsign);
  TEST_ASSERT_TRUE(out[0].hasAltitude);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 3300.0f, out[0].altitudeFeet);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 170.0f, out[0].headingDegrees);
  TEST_ASSERT_TRUE(out[1].onGround);
  TEST_ASSERT_TRUE(out[2].emergency);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_distance_zero_and_one_degree);
  RUN_TEST(test_filter_keeps_nearest_first_within_radius);
  RUN_TEST(test_filter_drops_stale_positions);
  RUN_TEST(test_insert_nearest_ignores_duplicates_and_caps);
  RUN_TEST(test_projection_north_is_up_east_is_right);
  RUN_TEST(test_parser_reads_fields_and_trims);
  return UNITY_END();
}
