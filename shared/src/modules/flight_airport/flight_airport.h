#pragma once
// Airport module: OSM vector map + aircraft on the field. Shares its map with the radar.
#include <stdint.h>

bool airportFetch();                                 // network task only
void airportReleaseConnection();
// Flight Radar overlay: runway/taxiway geometry of the airport (any thread).
bool airportEnsureRadarGeometry(const char *code, double lat, double lon, bool allowDownload);
void airportDrawRadarOverlay(float radiusKm, uint16_t *buf, int size);
