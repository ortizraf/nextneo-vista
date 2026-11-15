#pragma once
// Garden ("Jardim") module: screen API used outside garden_screen.cpp (tests).
#include <lvgl.h>
#include "modules/garden/garden_model.h"

garden::State &gardenState();    // live plant state (UI thread only)
void gardenReload();             // re-read the saved state from Preferences and redraw
bool gardenNeedsWater();         // menu badge: the saved plant, moved to now, is thirsty (cached 10 min)
// Test hooks
const lv_draw_buf_t *gardenPlantPicture();   // decoded plant picture (nullptr: none)
lv_obj_t *gardenButterfly();                 // butterfly sprite object
void gardenButterflyGo();                    // start a butterfly visit now (plant in bloom; any hour)
lv_obj_t *gardenToast();
const lv_draw_buf_t *gardenBackdrop();          // test hook: decoded tree-line picture                     // "Nova fase" pill
