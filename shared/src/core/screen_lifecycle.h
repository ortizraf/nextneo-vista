#pragma once
// Ciclo de vida de uma tela, comum a todas as placas. Cada placa implementa as SUAS telas
// (modules/<tela>/<tela>_screen.cpp) e as lista em kScreenTable (core/screen_registry.cpp
// da placa). O screen_manager (compartilhado) usa esta tabela.
//
// Usa tipos do LVGL (lv_obj_t), que todas as placas têm; nenhuma medida de tela aqui.
#include <lvgl.h>
#include "core/app_core.h"

// Lifecycle contract of a screen: each module defines one and the screen manager drives
// it. UI callbacks run on the LVGL thread.
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

// Telas da placa, em qualquer ordem (o manager indexa por id). Uma placa pode não ter
// todas as telas de ScreenID: as que faltam ficam vazias e switchScreen() as ignora.
extern const ScreenLifecycle *const kScreenTable[];
extern const uint8_t kScreenTableCount;

const ScreenLifecycle *screenLifecycle(ScreenID id);   // never null (unknown id -> SCREEN_NONE)
lv_obj_t *screenView(ScreenID id);     // container of a screen (tests, diagnostics)
