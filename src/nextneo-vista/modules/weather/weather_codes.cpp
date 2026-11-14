// WMO weather codes (Open-Meteo) -> Portuguese text and rain helpers. Pure functions.
#include "modules/weather/weather.h"

const char *weatherText(int c, bool night) {
  if (c == 0) return night ? "C\xC3\xA9u limpo" : "Ensolarado";
  if (c == 1) return "Poucas nuvens";
  if (c == 2) return "Parcialmente nublado";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if (c >= 51 && c <= 57) return "Garoa";
  if (c >= 61 && c <= 67) return "Chuva";
  if (c >= 71 && c <= 77) return "Neve";
  if (c >= 80 && c <= 82) return "Pancadas";
  if (c == 85 || c == 86) return "Neve fraca";
  if (c >= 95) return "Tempestade";
  return "Sem dados";
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
