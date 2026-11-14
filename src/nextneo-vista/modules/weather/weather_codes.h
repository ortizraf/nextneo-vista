#pragma once
// WMO weather codes (Open-Meteo) -> Portuguese text and rain helpers.
// Pure C++ (no Arduino): unit-tested on the computer (test/test_native_logic).

const char *weatherText(int code, bool night = false);   // night: "Céu limpo" instead of "Ensolarado"
bool isRainCode(int code);
int rainLevel(int code);   // 0 none, 1 garoa, 2 chuva, 3 chuva forte / tempestade
