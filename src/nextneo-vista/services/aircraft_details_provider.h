#pragma once
#include "../modules/flight_radar/flight_types.h"
#include <stddef.h>

// Route / airline / model lookup on api.adsbdb.com (free, no key).
// Network task only. Fills `out` (state READY, NOT_FOUND or FAILED).
void fetchAircraftDetails(AircraftDetails &out);
