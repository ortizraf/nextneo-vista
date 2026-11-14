#pragma once
// Weather module: Open-Meteo forecast (network task) and WMO code helpers.
#include <Arduino.h>
#include "app_screens.h"

// Forecast published by the network task (weatherFetch) for the Weather and Clock
// screens. Always read/written under mtx.
struct WeatherData {
  bool haveWeather = false;
  String cityName = "São Paulo";
  float temp = 0;
  int code = 0;
  int hourCount = 0;
  char hourLabel[8][6] = {{0}};
  float hourTemp[8] = {0};
  int hourCode[8] = {0};
  bool isDay = true;                  // Open-Meteo is_day for "now"
  bool hourIsDay[8] = {true, true, true, true, true, true, true, true};
  int hourRain[8] = {0};
  int humidity = 0;
  float wind = 0;
  float feels = 0;                    // apparent temperature
  float uv = -1;                      // UV index (-1 = unknown)
  char dayName[7][6] = {{0}};
  float tmax[7] = {0}, tmin[7] = {0};
  int dayRain[7] = {0};               // daily precipitation_probability_max (%)
  int dcode[7] = {0};
  uint32_t weatherVersion = 0;        // bumped on every new forecast / location
};
extern WeatherData wx;

#include "modules/weather/weather_codes.h"

// ---- network task
bool weatherFetch();                                  // forecast for cfg.lat/lon into sh
bool weatherGeocodeFetch();                           // sh.pendingCity -> cfg.lat/lon
bool applyManualLocation(const String &input);       // "Nome @ lat,lon", no network

bool cachedWeatherCode(int &code);                    // last forecast saved by the Clock screen
