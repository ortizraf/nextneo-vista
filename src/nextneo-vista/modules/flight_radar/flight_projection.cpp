#include "flight_projection.h"
#include <math.h>

void projectAircraft(const Aircraft &aircraft, double centerLat, double centerLon,
                     float radiusNm, int centerX, int centerY, int radiusPx,
                     int &screenX, int &screenY) {
  constexpr double rad = 0.017453292519943295;
  constexpr double kmPerNm = 1.852;
  const double northNm = (aircraft.latitude - centerLat) * rad * 6371.0 / kmPerNm;
  const double eastNm = (aircraft.longitude - centerLon) * rad * 6371.0 *
                        cos(centerLat * rad) / kmPerNm;
  const float scale = radiusPx / radiusNm;
  screenX = centerX + (int)lround(eastNm * scale);
  screenY = centerY - (int)lround(northNm * scale);
}
