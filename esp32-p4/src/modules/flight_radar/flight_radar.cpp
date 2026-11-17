// API de dados do radar (shared/src/modules/flight_radar/flight_radar.h) na P4.
// PROVISÓRIO até a tela do radar ser portada: na S3 a busca de dados e o mapa estão no
// mesmo arquivo da tela, desenhando em buffers do tamanho da tela de 3,5". Ao portar,
// separar a parte de dados (busca ADS-B, ajustes, centro) para shared/ e apagar este arquivo.
#include <Arduino.h>
#include <string.h>
#include "modules/flight_radar/flight_radar.h"

static char gCode[8] = "CGH";
static uint8_t gRadiusKm = 25;

bool flightRadarIsActive() { return false; }
bool updateFlightRadarData() { return false; }
bool updateFlightRadarDetails() { return false; }
bool setFlightRadarAirportCode(const char *code) { strlcpy(gCode, code ? code : "", sizeof gCode); return true; }
bool setFlightRadarRadiusKm(uint8_t radiusKm) { gRadiusKm = radiusKm; return true; }
void loadFlightRadarSettings() {}
void getFlightRadarSettings(char *code, size_t codeSize, char *name, size_t nameSize, uint8_t &radiusKm) {
  strlcpy(code, gCode, codeSize);
  strlcpy(name, gCode, nameSize);
  radiusKm = gRadiusKm;
}
void getFlightRadarCenter(double &lat, double &lon) { lat = -23.6261; lon = -46.6564; }   // CGH
