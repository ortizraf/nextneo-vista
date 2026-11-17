// Contrato de placa (shared/src/core/board.h) da ESP32-S3 com tela de 3,5" 480x320
// (AXS15231B por QSPI, touch capacitivo). Wi-Fi no próprio chip.
#include "core/board.h"
#include "core/display_touch.h"

// 480x320 em 3,5": sqrt(480² + 320²) / 3,5 ≈ 165 dpi (a densidade de referência de dp()).
const BoardInfo kBoard = {
  "ESP32-S3 3.5\"",
  480, 320,
  165,
  false,   // hasAudio
  false,   // hasSdCard
  false,   // hasCamera
  false,   // hasBattery
};

void boardDisplayBegin() { displayBegin(); }
void boardWifiPrepare() {}
