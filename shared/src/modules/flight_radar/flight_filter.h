#pragma once
#include "modules/flight_radar/flight_types.h"

uint8_t filterAircraft(Aircraft *aircraft, uint8_t count, double centerLat,
                       double centerLon, float radiusNm);
float aircraftDistanceNm(double lat1, double lon1, double lat2, double lon2);
// Inserts `a` into `list` (sorted by distanceNm, nearest first) keeping at most
// `capacity` entries. Duplicated ICAO addresses are ignored. Returns the new count.
uint8_t insertNearest(Aircraft *list, uint8_t count, uint8_t capacity, const Aircraft &a);
