#pragma once
#include "../modules/flight_radar/flight_types.h"
#include <stddef.h>

class AircraftDataProvider {
 public:
  virtual ~AircraftDataProvider() = default;
  // shouldContinue (optional) is polled after the HTTP response arrives; when it
  // returns false the response is dropped without parsing.
  // On success `aircraft` holds up to `capacity` entries, nearest first.
  virtual bool fetch(double centerLat, double centerLon, float radiusNm,
                     Aircraft *aircraft, uint8_t capacity, uint8_t &count,
                     char *error, size_t errorCapacity,
                     bool (*shouldContinue)() = nullptr) = 0;
};

AircraftDataProvider &adsbLolProvider();
// Closes the keep-alive TLS session (network task only). Cheap when already closed.
void adsbLolReleaseConnection();
