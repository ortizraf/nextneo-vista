#pragma once
// Contrato do app na placa ESP32-S3 (3,5", 480x320): inclui o núcleo compartilhado
// (shared/src/core/app_core.h: estado, telas, jobs de rede) e acrescenta a parte de UI
// desta placa: LVGL, ciclo de vida das telas e o kit de UI. APIs de cada tela ficam
// no header do módulo (modules/<tela>/<tela>.h).

#include <lvgl.h>
#include "core/app_core.h"
#include "core/board.h"
#include "core/screen_lifecycle.h"

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
// Resolução desta placa (constante de compilação; o mesmo valor está em kBoard)
#define SCR_W 480
#define SCR_H 320

// ---------------------------------------------------------------- UI kit (core/ui_kit.cpp)
lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);     // the app's one back button (goes to the menu)
void setLabelIfChanged(lv_obj_t *label, const char *text);
void renderWifiIndicator(lv_obj_t *icon, bool up);

