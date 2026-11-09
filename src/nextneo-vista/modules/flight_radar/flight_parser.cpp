#include "flight_parser.h"
#include <math.h>
#include <string.h>

static bool readNumber(JsonObjectConst obj, const char *key, float &value) {
  JsonVariantConst v = obj[key];
  if (v.isNull() || !v.is<float>()) return false;
  value = v.as<float>();
  return isfinite(value);
}

static bool readDouble(JsonObjectConst obj, const char *key, double &value) {
  JsonVariantConst v = obj[key];
  if (v.isNull() || !v.is<double>()) return false;
  value = v.as<double>();
  return isfinite(value);
}

static void copyTrimmed(JsonVariantConst value, char *target, size_t size) {
  target[0] = '\0';
  if (value.isNull() || !value.is<const char *>()) return;
  const char *source = value.as<const char *>();
  size_t length = strlen(source);
  while (length && source[length - 1] == ' ') --length;
  while (length && *source == ' ') { ++source; --length; }
  if (length >= size) length = size - 1;
  memcpy(target, source, length);
  target[length] = '\0';
}

bool parseAdsbLolAircraftObject(JsonObjectConst obj, Aircraft &a) {
  a = Aircraft();
  double lat, lon;  // double: float loses ~1 m precision, irrelevant, but keeps API parity
  if (!readDouble(obj, "lat", lat) || !readDouble(obj, "lon", lon) ||
      lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  copyTrimmed(obj["hex"], a.icao24, sizeof a.icao24);
  copyTrimmed(obj["flight"], a.callsign, sizeof a.callsign);
  copyTrimmed(obj["t"], a.type, sizeof a.type);
  copyTrimmed(obj["r"], a.registration, sizeof a.registration);
  copyTrimmed(obj["category"], a.category, sizeof a.category);
  copyTrimmed(obj["squawk"], a.squawk, sizeof a.squawk);
  a.latitude = lat;
  a.longitude = lon;
  float n;
  if (readNumber(obj, "alt_baro", n)) {
    if (n < 50) a.onGround = true;
    else { a.altitudeFeet = n; a.hasAltitude = true; }
  } else if (obj["alt_baro"].is<const char *>()) {
    if (!strcmp(obj["alt_baro"].as<const char *>(), "ground")) a.onGround = true;
  }
  if (readNumber(obj, "gs", n) && n >= 0) { a.speedKnots = n; a.hasSpeed = true; }
  if (readNumber(obj, "track", n) && n >= 0 && n <= 360) {
    a.headingDegrees = n; a.hasHeading = true;
  } else if (readNumber(obj, "true_heading", n) && n >= 0 && n <= 360) {
    a.headingDegrees = n; a.hasHeading = true;
  }
  if (readNumber(obj, "baro_rate", n) || readNumber(obj, "geom_rate", n)) {
    a.verticalRateFpm = n; a.hasVerticalRate = true;
  }
  if (readNumber(obj, "seen_pos", n) && n >= 0) {
    a.positionAgeSeconds = n; a.hasPositionAge = true;
  }
  const char *emergency = obj["emergency"] | "none";
  a.emergency = strcmp(emergency, "none") != 0 ||
                !strcmp(a.squawk, "7500") || !strcmp(a.squawk, "7600") || !strcmp(a.squawk, "7700");
  return true;
}

bool parseAdsbLolAircraft(JsonArrayConst aircraftJson, Aircraft *out, uint8_t capacity,
                          uint8_t &count) {
  count = 0;
  if (aircraftJson.isNull() || !out || !capacity) return false;
  for (JsonVariantConst item : aircraftJson) {
    if (!item.is<JsonObjectConst>()) continue;
    if (!parseAdsbLolAircraftObject(item.as<JsonObjectConst>(), out[count])) continue;
    if (++count >= capacity) break;
  }
  return true;
}
