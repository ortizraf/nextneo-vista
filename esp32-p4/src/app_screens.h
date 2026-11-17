#pragma once
// Contrato do app na placa ESP32-P4 5" (720x1280): núcleo compartilhado (shared/) + a
// parte de UI desta placa. Tamanhos de UI em dp() (core/board.h): mesmo tamanho FÍSICO
// que na S3, com 1,8x mais pixels.
#include <lvgl.h>
#include "core/app_core.h"
#include "core/board.h"
#include "core/screen_lifecycle.h"

// Resolução desta placa na orientação do app. VERIFICAR NA PLACA: retrato (720x1280,
// nativo do painel) ou paisagem (1280x720, com rotação). Começamos em retrato.
#define SCR_W 720
#define SCR_H 1280

// ---------------------------------------------------------------- UI kit (core/ui_kit.cpp)
lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);     // the app's one back button (goes to the menu)
void setLabelIfChanged(lv_obj_t *label, const char *text);
void renderWifiIndicator(lv_obj_t *icon, bool up);
