#pragma once
// Weather module: Open-Meteo forecast (network task) and WMO code helpers.
#include <Arduino.h>

const char *weatherText(int code, bool night = false);   // night: "Céu limpo" instead of "Ensolarado"
bool isRainCode(int code);
int rainLevel(int code);   // 0 none, 1 garoa, 2 chuva, 3 chuva forte / tempestade

// ---- network task
bool weatherFetch();                                  // forecast for cfg.lat/lon into sh
bool weatherGeocodeFetch();                           // sh.pendingCity -> cfg.lat/lon
bool applyManualLocation(const String &input);       // "Nome @ lat,lon", no network

bool cachedWeatherCode(int &code);                    // last forecast saved by the Clock screen
