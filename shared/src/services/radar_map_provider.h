#pragma once
#include <stdint.h>
#include <stddef.h>

// Downloads a square basemap centred on (lat, lon) covering `radiusKm` from
// the Esri World Street Map export endpoint (JPEG, no API key), decodes it
// into `out` (size x size RGB565, LVGL byte order), tints it to the radar
// palette and paints everything outside the scope circle with `bgColor565`.
// Network task only. The pixel scale matches projectAircraft().
bool fetchRadarMap(double lat, double lon, float radiusKm, uint16_t *out, int size,
                   uint16_t bgColor565, char *error, size_t errorCapacity);
