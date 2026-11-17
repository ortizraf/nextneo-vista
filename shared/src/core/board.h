#pragma once
// Contrato de placa: o que CADA placa (esp32-s3/, esp32-p4/, ...) precisa fornecer para o
// app compartilhado rodar. Implementado em <placa>/src/core/board.cpp.
//
// O app comum (core/app_main.cpp, screen_manager, net_worker, página web) só conhece a
// placa por aqui. Para criar uma placa nova: implemente estas funções e a tabela de telas
// (core/screen_lifecycle.h), mais o cartão web kBoardWeb (core/web_ui.h).
#include <stdint.h>

struct BoardInfo {
  const char *name;        // ex.: "ESP32-S3 3.5\"" (aparece na página web)
  uint16_t width;          // resolução na orientação usada pelo app (pixels)
  uint16_t height;
  uint16_t dpi;            // densidade física (pixels por polegada): base dos tamanhos em dp()
  // Recursos opcionais. O código comum consulta estes campos antes de usar o recurso.
  bool hasAudio;           // codec de áudio / alto-falante
  bool hasSdCard;          // slot TF/microSD
  bool hasCamera;          // câmera (MIPI-CSI)
  bool hasBattery;         // conector de bateria (medição de carga)
};

extern const BoardInfo kBoard;

// Inicia tela, touch e LVGL (display + dispositivo de entrada). Chamada uma vez no boot,
// antes de as telas serem construídas.
void boardDisplayBegin();

// Ajustes de hardware antes do Wi-Fi (ex.: na P4, pinos SDIO do coprocessador ESP32-C6).
// Placas com Wi-Fi no próprio chip não precisam fazer nada.
void boardWifiPrepare();

// Densidade de referência: a tela de 3,5" 480x320 da primeira placa (~165 dpi).
// Tamanhos escritos em "dp" mantêm o mesmo tamanho FÍSICO em qualquer tela:
// dp(48) = 48 px na S3 e ~86 px na P4 de 5" 720x1280 (~294 dpi).
static constexpr uint16_t kBaseDpi = 165;
inline int dp(int v) { return (v * (int)kBoard.dpi + kBaseDpi / 2) / (int)kBaseDpi; }
