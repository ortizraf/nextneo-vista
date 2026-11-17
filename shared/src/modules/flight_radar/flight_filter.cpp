#include "modules/flight_radar/flight_filter.h"
#include <math.h>
#include <string.h>

float aircraftDistanceNm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double rad = 0.017453292519943295;
  const double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
  const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * rad) * cos(lat2 * rad) *
                   sin(dLon / 2) * sin(dLon / 2);
  return (float)(6371.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)) / 1.852);
}

uint8_t insertNearest(Aircraft *list, uint8_t count, uint8_t capacity, const Aircraft &a) {
  if (a.icao24[0])
    for (uint8_t j = 0; j < count; ++j)
      if (!strcmp(list[j].icao24, a.icao24)) return count;
  if (count == capacity && a.distanceNm >= list[count - 1].distanceNm) return count;
  int pos = count < capacity ? count : capacity - 1;
  while (pos > 0 && list[pos - 1].distanceNm > a.distanceNm) {
    if (pos < capacity) list[pos] = list[pos - 1];
    --pos;
  }
  list[pos] = a;
  return count < capacity ? count + 1 : count;
}

uint8_t filterAircraft(Aircraft *aircraft, uint8_t count, double centerLat,
                       double centerLon, float radiusNm) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count; ++i) {
    Aircraft a = aircraft[i];
    if (a.hasPositionAge && a.positionAgeSeconds > 180.0f) continue;
    a.distanceNm = aircraftDistanceNm(centerLat, centerLon, a.latitude, a.longitude);
    if (a.distanceNm > radiusNm) continue;
    // kept <= i, so the slots being overwritten were already consumed.
    kept = insertNearest(aircraft, kept, count, a);
  }
  return kept;
}
