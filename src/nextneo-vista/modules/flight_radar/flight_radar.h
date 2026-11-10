#pragma once
#include <lvgl.h>
#include "flight_types.h"

void buildFlightRadarScreen(lv_obj_t *screen);
// UI thread: lifecycle hooks used by the screen manager.
void flightRadarOnEnter();
void flightRadarOnExit();
void updateFlightRadarScreen();          // render cached snapshot (active screen only)
void setFlightRadarRefreshRequester(void (*requester)());
void setFlightRadarDetailsRequester(void (*requester)());
bool flightRadarIsActive();
// Network task: fetch + parse ADS-B data (and the basemap when needed).
bool updateFlightRadarData();
// Network task: route/airline/model of the selected aircraft (adsbdb.com).
bool updateFlightRadarDetails();
bool setFlightRadarAirportCode(const char *code);
// Accepted radii: 5, 10, 25 or 50 km.
bool setFlightRadarRadiusKm(uint8_t radiusKm);
void loadFlightRadarSettings();
// Current airport code/name and radius (any thread).
void getFlightRadarSettings(char *code, size_t codeSize, char *name, size_t nameSize, uint8_t &radiusKm);
// Reference point of the selected airport (any thread). Shared with the Airport screen.
void getFlightRadarCenter(double &lat, double &lon);
