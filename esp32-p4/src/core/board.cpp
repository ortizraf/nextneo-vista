// Contrato de placa (shared/src/core/board.h) da Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
#include <WiFi.h>
#include "core/board.h"
#include "core/display_dsi.h"

// 720x1280 em 5": sqrt(720² + 1280²) / 5 ≈ 294 dpi -> dp(x) ≈ 1,8x.
const BoardInfo kBoard = {
  "ESP32-P4 5\"",
  720, 1280,
  294,
  true,    // hasAudio   (codec de áudio onboard; driver: a fazer)
  true,    // hasSdCard  (slot TF, SD_MMC; a fazer)
  true,    // hasCamera  (MIPI-CSI, ex. OV5647; a fazer)
  true,    // hasBattery (conector MX1.25 3,7 V; medição: a fazer)
};

void boardDisplayBegin() { displayDsiBegin(); }

// Wi-Fi 6 vem do ESP32-C6 (ESP-Hosted por SDIO). O core Arduino usa o WiFi.h normal por
// cima dele; só os pinos SDIO podem precisar ser informados.
// VERIFICAR NA PLACA: pinos no esquemático/wiki da Waveshare. Os valores abaixo são os da
// placa de referência da Espressif (ESP32-P4-Function-EV-Board) e estão desligados até a
// confirmação; sem setPins, valem os padrões do core.
void boardWifiPrepare() {
  // WiFi.setPins(/*clk*/ 18, /*cmd*/ 19, /*d0*/ 14, /*d1*/ 15, /*d2*/ 16, /*d3*/ 17, /*rst*/ 54);
}
