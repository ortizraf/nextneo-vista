// Tela 5" 720x1280 MIPI-DSI + touch da Waveshare ESP32-P4-WIFI6-Touch-LCD-5, ligados ao LVGL.
//
// ESTADO: a parte LVGL está pronta (display, buffers em PSRAM, tick, dispositivo de toque);
// o envio dos pixels ao painel e a leitura do touch são TODO até a placa chegar:
//   1. VERIFICAR NA PLACA: controlador do painel DSI e do touch (wiki/esquemático Waveshare;
//      o exemplo oficial da placa mostra os nomes e a sequência de inicialização).
//   2. Inicializar o painel com o esp_lcd do ESP-IDF (esp_lcd_new_dsi_bus +
//      esp_lcd_new_panel_dpi + driver do controlador) e o backlight.
//   3. flush_cb: esp_lcd_panel_draw_bitmap(); com framebuffer do DPI, usar
//      LV_DISPLAY_RENDER_MODE_DIRECT e os buffers do próprio painel (sem cópia).
//   4. touch_cb: ler o controlador por I2C e preencher data->point / data->state.
//   5. Depois: acelerar com o PPA (preencher/rotacionar/combinar) e o JPEG por hardware.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "app_screens.h"
#include "core/display_dsi.h"
#include "services/app_log.h"

static void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *) {
  // TODO(P4): enviar a área ao painel DSI.
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  // TODO(P4): ler o touch (I2C) e converter para coordenadas de SCR_W x SCR_H.
  data->state = LV_INDEV_STATE_RELEASED;
}

void displayDsiBegin() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  // Buffer parcial de 1/10 da tela em PSRAM (32 MB). Com o painel pronto, trocar por
  // dois framebuffers completos (2 x 1,8 MB) para animação sem tremor.
  const size_t bufBytes = SCR_W * (SCR_H / 10) * sizeof(uint16_t);
  void *buf = heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) Log.println("ERRO: sem PSRAM para o buffer da tela");
  lv_display_set_buffers(disp, buf, NULL, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);
  Log.println("Tela DSI: LVGL pronto; painel e touch ainda sem driver (TODO P4)");
}
