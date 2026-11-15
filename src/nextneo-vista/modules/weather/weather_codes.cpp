// WMO weather codes (Open-Meteo) -> Portuguese text and rain helpers. Pure functions.
#include "modules/weather/weather_codes.h"
#include "core/i18n.h"

const char *weatherText(int c, bool night) {
  if (c == 0) return night ? T("C\xC3\xA9u limpo", "Clear sky") : T("Ensolarado", "Sunny");
  if (c == 1) return T("Poucas nuvens", "Mostly clear");
  if (c == 2) return T("Parcialmente nublado", "Partly cloudy");
  if (c == 3) return T("Nublado", "Overcast");
  if (c == 45 || c == 48) return T("Neblina", "Fog");
  if (c >= 51 && c <= 57) return T("Garoa", "Drizzle");
  if (c >= 61 && c <= 67) return T("Chuva", "Rain");
  if (c >= 71 && c <= 77) return T("Neve", "Snow");
  if (c >= 80 && c <= 82) return T("Pancadas", "Showers");
  if (c == 85 || c == 86) return T("Neve fraca", "Light snow");
  if (c >= 95) return T("Tempestade", "Thunderstorm");
  return T("Sem dados", "No data");
}

bool isRainCode(int c) {
  return (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
}

// Rain intensity for the artwork (WMO code): 0 none, 1 garoa, 2 chuva, 3 chuva forte/tempestade.
int rainLevel(int c) {
  if (c >= 51 && c <= 57) return 1;                                   // garoa
  if (c == 61 || c == 63 || c == 66 || c == 80 || c == 81) return 2;  // chuva fraca/moderada, pancadas
  if (c == 65 || c == 67 || c == 82 || c >= 95) return 3;             // chuva forte, pancadas fortes, tempestade
  return 0;
}
