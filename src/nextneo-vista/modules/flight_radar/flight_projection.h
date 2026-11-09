#pragma once
#include "flight_types.h"

void projectAircraft(const Aircraft &aircraft, double centerLat, double centerLon,
                     float radiusNm, int centerX, int centerY, int radiusPx,
                     int &screenX, int &screenY);
