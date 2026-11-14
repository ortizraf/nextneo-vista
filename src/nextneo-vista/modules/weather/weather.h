#pragma once
// Weather module: Open-Meteo forecast (network task) and WMO code helpers.
#include <Arduino.h>

#include "modules/weather/weather_codes.h"

// ---- network task
bool weatherFetch();                                  // forecast for cfg.lat/lon into sh
bool weatherGeocodeFetch();                           // sh.pendingCity -> cfg.lat/lon
bool applyManualLocation(const String &input);       // "Nome @ lat,lon", no network

bool cachedWeatherCode(int &code);                    // last forecast saved by the Clock screen
