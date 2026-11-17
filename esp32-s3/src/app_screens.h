#pragma once
// Contrato do app na placa ESP32-S3 (3,5", 480x320): inclui o núcleo compartilhado
// (shared/src/core/app_core.h: estado, telas, jobs de rede) e acrescenta a parte de UI
// desta placa: LVGL, ciclo de vida das telas e o kit de UI. APIs de cada tela ficam
// no header do módulo (modules/<tela>/<tela>.h).

#include <lvgl.h>
#include "core/app_core.h"

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
#define SCR_W 480
#define SCR_H 320

// Lifecycle contract of a screen: each module defines one (core/screen_registry.h)
// and the screen manager drives it. UI callbacks run on the LVGL thread.
struct ScreenLifecycle {
  ScreenID id;
  const char *name;
  lv_obj_t **view;              // container shown/hidden by the manager
  void (*build)(lv_obj_t *screen);   // create the widgets once at boot
  void (*onEnter)();            // screen became visible
  void (*onExit)();             // screen is about to be hidden
  void (*render)();             // refresh widgets from cached data (active screen only)
  void (*requestUpdate)();      // periodic refresh; nullptr = post jobs[0] (if any)
  uint32_t updateIntervalMs;    // periodic refresh while the user stays (0 = none)
  uint32_t retryIntervalMs;     // faster retry after a failed request
  const NetJobDef *jobs;        // jobs this screen may post (jobs[JOB_UPDATE] = periodic update)
  uint8_t jobCount;
  void (*netRelease)();         // network task: free keep-alive sessions while not active
};

const ScreenLifecycle *screenLifecycle(ScreenID id);   // never null (unknown id -> SCREEN_NONE)
lv_obj_t *screenView(ScreenID id);     // container of a screen (tests, diagnostics)

// ---------------------------------------------------------------- UI kit (core/ui_kit.cpp)
lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);     // the app's one back button (goes to the menu)
void setLabelIfChanged(lv_obj_t *label, const char *text);
void renderWifiIndicator(lv_obj_t *icon, bool up);

