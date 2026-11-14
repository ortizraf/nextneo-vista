// Generative Art screen ("Arte"): procedural animations computed on the ESP32
// (GenerativeArtEngine) shown full screen through one lv_image, like the Clock sky.
// No network: this screen never posts API jobs.
//
// Touch: tap shows the controls (they hide after a few seconds); press/drag is also
// fed to the engine (Particles: attract / repel). Controls: previous / pause / next
// pattern, ambient mode, settings panel. Settings are applied live and saved in
// Preferences when the panel closes or the screen exits (no flash writes per slider step).
//
// Everything runs on the LVGL thread: the frame timer is an lv_timer, so the
// animation never blocks the UI and needs no locking. Leaving the screen pauses the
// timer and frees the frame buffer + pattern state (~300 KB PSRAM).
#include "../../app_screens.h"
#include "../../services/app_log.h"
#include "art_engine.h"
#include <esp_heap_caps.h>

lv_obj_t *artView;

static constexpr uint32_t ART_FRAME_MS = 66;          // ~15 fps: every frame pushes the full panel (see clock.cpp)
static constexpr uint32_t ART_CONTROLS_HIDE_MS = 5000;
static constexpr float ART_AMBIENT_TIME_SCALE = 0.4f;
static const uint16_t kAutoMinutes[3] = {1, 5, 15};

static GenerativeArtEngine engine;
static lv_image_dsc_t frameDsc;
static lv_obj_t *frameImg, *backBtn, *bar, *titleLbl, *pauseLbl, *ambientBtn, *errLbl, *capLbl;
static uint32_t capLastMs = 0;
static lv_obj_t *panel, *bmPattern, *bmPalette, *bmAuto, *slSpeed, *slDensity, *slAmbient, *swRepel, *swAuto;
static lv_timer_t *frameTimer, *hideTimer;

// settings (Preferences namespace "clock", keys prefixed "ga")
static ArtParams params;
static uint8_t savedPattern = 0;
static bool autoCycle = false;
static uint8_t autoIdx = 1;                           // kAutoMinutes index
static uint8_t ambientBl = 30;                        // % of the user's brightness in ambient mode
static bool settingsDirty = false;

static bool paused = false, ambient = false;
static uint32_t lastFrameMs = 0, autoElapsedMs = 0, pressStartMs = 0;
static uint32_t perfSteps = 0, perfStepUs = 0, perfLastMs = 0;

static void show(lv_obj_t *o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void applyBacklight() {
  const int pct = ambient ? cfg.bright * ambientBl / 100 : cfg.bright;
  analogWrite(GFX_BL, map(pct, 0, 100, 25, 255));     // same mapping as Ajustes (config.cpp)
}

// ---------------------------------------------------------------- persistence
static void loadSettings() {
  savedPattern = prefs.getUChar("gaPat", 0);
  params.speed = prefs.getUChar("gaSpd", 100) / 100.0f;
  params.density = prefs.getUChar("gaDen", 60);
  params.palette = prefs.getUChar("gaPal", 0);
  params.touchRepels = prefs.getBool("gaRepel", false);
  autoCycle = prefs.getBool("gaAuto", false);
  autoIdx = prefs.getUChar("gaAutoI", 1);
  ambientBl = prefs.getUChar("gaAmbBl", 30);
  // safe limits even if prefs hold garbage
  if (savedPattern >= engine.patternCount()) savedPattern = 0;   // e.g. a removed pattern
  if (params.speed < 0.25f || params.speed > 2.0f) params.speed = 1.0f;
  if (params.density < 25 || params.density > 100) params.density = 60;
  if (params.palette >= art::PALETTE_COUNT) params.palette = 0;
  if (autoIdx > 2) autoIdx = 1;
  if (ambientBl < 10 || ambientBl > 60) ambientBl = 30;
}

static void saveSettings() {
  if (!settingsDirty) return;
  settingsDirty = false;
  prefs.putUChar("gaPat", engine.pattern());
  prefs.putUChar("gaSpd", (uint8_t)(params.speed * 100 + 0.5f));
  prefs.putUChar("gaDen", params.density);
  prefs.putUChar("gaPal", params.palette);
  prefs.putBool("gaRepel", params.touchRepels);
  prefs.putBool("gaAuto", autoCycle);
  prefs.putUChar("gaAutoI", autoIdx);
  prefs.putUChar("gaAmbBl", ambientBl);
}

// ---------------------------------------------------------------- controls
static void showControls(bool on) {
  show(backBtn, on); show(bar, on);
  show(titleLbl, on && lv_obj_has_flag(capLbl, LV_OBJ_FLAG_HIDDEN));   // the caption already names the scene
  if (on) { lv_timer_reset(hideTimer); lv_timer_resume(hideTimer); }
  else lv_timer_pause(hideTimer);
}
static void hideCb(lv_timer_t *) {
  if (!lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) { lv_timer_reset(hideTimer); return; }   // keep while editing
  showControls(false);
}
static void bumpControls() { if (!lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) lv_timer_reset(hideTimer); }

static void syncPatternUi() {
  lv_label_set_text(titleLbl, engine.patternName(engine.pattern()));
  if (bmPattern) {
    lv_buttonmatrix_clear_button_ctrl_all(bmPattern, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_ctrl(bmPattern, engine.pattern(), LV_BUTTONMATRIX_CTRL_CHECKED);
  }
}

static void selectPattern(uint8_t i) {
  engine.setPattern(i);
  autoElapsedMs = 0;
  capLastMs = 0;                                       // refresh/hide the overlay text right away
  settingsDirty = true;
  syncPatternUi();
  if (engine.running()) lv_obj_invalidate(frameImg);
}

static void prevEvent(lv_event_t *) {
  const uint8_t n = engine.patternCount();
  selectPattern((engine.pattern() + n - 1) % n); bumpControls();
}
static void nextEvent(lv_event_t *) { selectPattern((engine.pattern() + 1) % engine.patternCount()); bumpControls(); }
static void pauseEvent(lv_event_t *) {
  paused = !paused;
  lv_label_set_text(pauseLbl, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  lastFrameMs = 0;
  bumpControls();
}

void artSetAmbient(bool on) {
  if (ambient == on) return;
  ambient = on;
  engine.setTimeScale(on ? ART_AMBIENT_TIME_SCALE : 1.0f);
  lv_obj_set_style_bg_color(ambientBtn, lv_color_hex(on ? 0x1E6BFF : 0x0A1220), 0);
  if (activeScreenId() == SCREEN_ART) applyBacklight();
  if (on) showControls(false);
}
static void ambientEvent(lv_event_t *) { artSetAmbient(!ambient); }

static void settingsEvent(lv_event_t *) {
  show(panel, true);
  lv_obj_move_foreground(panel);
  bumpControls();
}
static void closePanelEvent(lv_event_t *) {
  show(panel, false);
  saveSettings();
  bumpControls();
}

// ---------------------------------------------------------------- settings widgets
static void paramsChanged() {
  engine.setParams(params);
  settingsDirty = true;
}
static void bmPatternEvent(lv_event_t *e) {
  const uint32_t i = lv_buttonmatrix_get_selected_button((lv_obj_t *)lv_event_get_target(e));
  if (i < engine.patternCount() && i != engine.pattern()) selectPattern((uint8_t)i);
}
static void bmPaletteEvent(lv_event_t *e) {
  const uint32_t i = lv_buttonmatrix_get_selected_button((lv_obj_t *)lv_event_get_target(e));
  if (i < art::PALETTE_COUNT) { params.palette = (uint8_t)i; paramsChanged(); }
}
static void bmAutoEvent(lv_event_t *e) {
  const uint32_t i = lv_buttonmatrix_get_selected_button((lv_obj_t *)lv_event_get_target(e));
  if (i < 3) { autoIdx = (uint8_t)i; autoElapsedMs = 0; settingsDirty = true; }
}
static void speedEvent(lv_event_t *e) {
  params.speed = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)) / 100.0f; paramsChanged();
}
// density restarts the pattern: apply on release only, not on every step of the drag
static void densityEvent(lv_event_t *e) {
  params.density = (uint8_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)); paramsChanged();
}
static void ambientBlEvent(lv_event_t *e) {
  ambientBl = (uint8_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
  settingsDirty = true;
  if (ambient) applyBacklight();
}
static void repelEvent(lv_event_t *e) {
  params.touchRepels = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED); paramsChanged();
}
static void autoEvent(lv_event_t *e) {
  autoCycle = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
  autoElapsedMs = 0; settingsDirty = true;
}

// ---------------------------------------------------------------- touch on the art
static void artTouchEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_t *indev = lv_indev_active();
  if (indev) lv_indev_get_point(indev, &p); else p = {0, 0};
  if (code == LV_EVENT_PRESSED) {
    pressStartMs = lv_tick_get();
    engine.setTouch(true, p.x, p.y);
  } else if (code == LV_EVENT_PRESSING) {
    engine.setTouch(true, p.x, p.y);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    engine.setTouch(false, p.x, p.y);
    // a short tap toggles the controls; a drag is pure interaction
    if (code == LV_EVENT_RELEASED && lv_tick_elaps(pressStartMs) < 350) {
      if (!lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) closePanelEvent(nullptr);
      else showControls(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
    }
  }
}

// ---------------------------------------------------------------- frame timer
static void frameCb(lv_timer_t *) {
  if (!engine.running()) return;
  const uint32_t now = lv_tick_get();
  const uint32_t dt = lastFrameMs ? now - lastFrameMs : ART_FRAME_MS;
  lastFrameMs = now;
  if (paused) return;                                  // nothing invalidated -> LVGL stays idle
  engine.update(dt);
  lv_obj_invalidate(frameImg);
  perfStepUs += engine.lastStepUs(); ++perfSteps;
  if (now - capLastMs >= 1000 || !capLastMs) {         // overlay text (e.g. Christmas countdown), 1x/s
    capLastMs = now;
    char cap[48];
    if (engine.caption(cap, sizeof cap)) { setLabelIfChanged(capLbl, cap); show(capLbl, true); }
    else show(capLbl, false);
  }
  if (now - perfLastMs >= 10000) {                     // fps itself is in the [perf] line (main.cpp)
    LogV.printf("[art] %s: passo %lu us (media) | PSRAM livre %u | interna livre %u\n",
                engine.patternName(engine.pattern()), (unsigned long)(perfSteps ? perfStepUs / perfSteps : 0),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    perfSteps = perfStepUs = 0; perfLastMs = now;
  }
  if (autoCycle) {
    autoElapsedMs += dt;
    if (autoElapsedMs >= kAutoMinutes[autoIdx] * 60000UL) selectPattern((engine.pattern() + 1) % engine.patternCount());
  }
}

// ---------------------------------------------------------------- build
static lv_obj_t *glassButton(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, lv_obj_t **labelOut = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 44, 34);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x0A1220), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);   // LV_SYMBOL_* glyphs
  lv_obj_set_style_text_color(l, lv_color_hex(0xCFE6FF), 0);
  lv_obj_center(l);
  if (labelOut) *labelOut = l;
  return b;
}

static lv_obj_t *panelRow(const char *caption) {
  lv_obj_t *l = makeLabel(panel, caption, &lv_font_weather_14, 0x8FB8F0);
  lv_obj_set_style_pad_top(l, 4, 0);
  return l;
}

static lv_obj_t *panelButtons(const char **map, uint8_t checked, lv_event_cb_t cb) {
  lv_obj_t *bm = lv_buttonmatrix_create(panel);
  lv_buttonmatrix_set_map(bm, map);
  lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(bm, true);
  lv_buttonmatrix_set_button_ctrl(bm, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_size(bm, lv_pct(100), 38);
  lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bm, 0, 0);
  lv_obj_set_style_pad_all(bm, 0, 0);
  lv_obj_set_style_pad_column(bm, 6, 0);
  lv_obj_set_style_bg_color(bm, lv_color_hex(0x0B1E44), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(bm, lv_color_hex(0x1E6BFF), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(bm, 10, LV_PART_ITEMS);
  lv_obj_set_style_text_font(bm, &lv_font_weather_14, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bm, lv_color_hex(0xDCEBFF), LV_PART_ITEMS);
  lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return bm;
}

static lv_obj_t *panelSlider(int min, int max, int value, lv_event_cb_t cb, lv_event_code_t when) {
  lv_obj_t *s = lv_slider_create(panel);
  lv_slider_set_range(s, min, max);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  lv_obj_set_width(s, lv_pct(92));
  lv_obj_set_style_margin_left(s, 10, 0);
  lv_obj_set_style_bg_color(s, lv_color_hex(0x0B1E44), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(0x1E90FF), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(0x7DD3FC), LV_PART_KNOB);
  lv_obj_add_event_cb(s, cb, when, NULL);
  return s;
}

static lv_obj_t *panelSwitch(const char *caption, bool on, lv_event_cb_t cb) {
  lv_obj_t *row = lv_obj_create(panel);
  lv_obj_set_size(row, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = makeLabel(row, caption, &lv_font_weather_14, 0x8FB8F0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 46, 24);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0x0B1E44), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0x1E6BFF), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}

static void buildPanel() {
  panel = lv_obj_create(artView);
  lv_obj_set_size(panel, 360, 292);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x050E24), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x1E4FD8), 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_50, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 16, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 12, 0);
  lv_obj_set_style_pad_row(panel, 4, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(panel, LV_DIR_VER);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x1E4FD8), LV_PART_SCROLLBAR);

  lv_obj_t *head = lv_obj_create(panel);
  lv_obj_set_size(head, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(head, 0, 0);
  lv_obj_set_style_pad_all(head, 0, 0);
  lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = makeLabel(head, "Arte generativa", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *close = glassButton(head, LV_SYMBOL_OK, closePanelEvent);
  lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);

  static const char *kPatternMap[] = {"Ondas", "Part\xC3\xAD" "culas", "Fluxo", "Natal", ""};
  static const char *kPaletteMap[] = {"Oceano", "Aurora", "El\xC3\xA9trico", ""};
  static const char *kAutoMap[] = {"1 min", "5 min", "15 min", ""};

  panelRow("Padr\xC3\xA3o");
  bmPattern = panelButtons(kPatternMap, savedPattern, bmPatternEvent);
  panelRow("Paleta");
  bmPalette = panelButtons(kPaletteMap, params.palette, bmPaletteEvent);
  panelRow("Velocidade");
  slSpeed = panelSlider(25, 200, (int)(params.speed * 100 + 0.5f), speedEvent, LV_EVENT_VALUE_CHANGED);
  panelRow("Densidade");
  slDensity = panelSlider(25, 100, params.density, densityEvent, LV_EVENT_RELEASED);
  swRepel = panelSwitch("Toque repele part\xC3\xAD" "culas", params.touchRepels, repelEvent);
  swAuto = panelSwitch("Troca autom\xC3\xA1tica", autoCycle, autoEvent);
  panelRow("Intervalo da troca");
  bmAuto = panelButtons(kAutoMap, autoIdx, bmAutoEvent);
  panelRow("Brilho no modo ambiente");
  slAmbient = panelSlider(10, 60, ambientBl, ambientBlEvent, LV_EVENT_VALUE_CHANGED);

  show(panel, false);
}

void buildArtScreen(lv_obj_t *screen) {
  loadSettings();

  artView = lv_obj_create(screen);
  lv_obj_set_size(artView, SCR_W, SCR_H);
  lv_obj_set_pos(artView, 0, 0);
  lv_obj_set_style_bg_color(artView, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(artView, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(artView, 0, 0);
  lv_obj_set_style_radius(artView, 0, 0);
  lv_obj_set_style_pad_all(artView, 0, 0);
  lv_obj_remove_flag(artView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(artView, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(artView, artTouchEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(artView, artTouchEvent, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(artView, artTouchEvent, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(artView, artTouchEvent, LV_EVENT_PRESS_LOST, NULL);

  frameImg = lv_image_create(artView);                 // src set in artOnEnter (buffer lives only while visible)
  lv_obj_set_pos(frameImg, 0, 0);
  lv_obj_set_size(frameImg, SCR_W, SCR_H);
  lv_obj_remove_flag(frameImg, LV_OBJ_FLAG_CLICKABLE);

  errLbl = makeLabel(artView, "Mem\xC3\xB3ria insuficiente para a arte", &lv_font_weather_14, 0x8FB8F0);
  lv_obj_center(errLbl);
  show(errLbl, false);

  capLbl = makeLabel(artView, "", &lv_font_weather_14, 0xFFD278);   // pill, top right (Natal countdown)
  lv_obj_set_style_bg_color(capLbl, lv_color_hex(0x050E24), 0);
  lv_obj_set_style_bg_opa(capLbl, LV_OPA_60, 0);
  lv_obj_set_style_border_color(capLbl, lv_color_hex(0xFFC23C), 0);
  lv_obj_set_style_border_opa(capLbl, LV_OPA_50, 0);
  lv_obj_set_style_border_width(capLbl, 1, 0);
  lv_obj_set_style_radius(capLbl, 12, 0);
  lv_obj_set_style_pad_hor(capLbl, 10, 0);
  lv_obj_set_style_pad_ver(capLbl, 5, 0);
  lv_obj_align(capLbl, LV_ALIGN_TOP_RIGHT, -10, 8);
  show(capLbl, false);
  titleLbl = makeLabel(artView, "", &lv_font_weather_14, 0x8FB8F0);
  lv_obj_set_style_text_opa(titleLbl, LV_OPA_80, 0);
  lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 12);

  bar = lv_obj_create(artView);
  lv_obj_set_size(bar, 5 * 44 + 4 * 8 + 16, 34 + 12);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x050E24), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(0x1E4FD8), 0);
  lv_obj_set_style_border_opa(bar, LV_OPA_40, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_radius(bar, 16, 0);
  lv_obj_set_style_pad_all(bar, 6, 0);
  lv_obj_set_style_pad_column(bar, 8, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  glassButton(bar, LV_SYMBOL_PREV, prevEvent);
  glassButton(bar, LV_SYMBOL_PAUSE, pauseEvent, &pauseLbl);
  glassButton(bar, LV_SYMBOL_NEXT, nextEvent);
  ambientBtn = glassButton(bar, LV_SYMBOL_EYE_CLOSE, ambientEvent);
  glassButton(bar, LV_SYMBOL_SETTINGS, settingsEvent);

  backBtn = addBackButton(artView);                    // standard back button (main.cpp)
  buildPanel();

  frameTimer = lv_timer_create(frameCb, ART_FRAME_MS, NULL);
  lv_timer_pause(frameTimer);
  hideTimer = lv_timer_create(hideCb, ART_CONTROLS_HIDE_MS, NULL);
  lv_timer_pause(hideTimer);
  lv_obj_add_flag(artView, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- lifecycle
void artOnEnter() {
  engine.setParams(params);
  engine.setTimeScale(ambient ? ART_AMBIENT_TIME_SCALE : 1.0f);
  engine.setPattern(savedPattern);                     // only records the index: nothing allocated yet
  if (!engine.begin(SCR_W, SCR_H)) {
    Log.println("[art] sem PSRAM para o quadro (300 KB)");
    show(errLbl, true);
    show(frameImg, false);
    showControls(true);
    return;
  }
  show(errLbl, false);
  frameDsc = {};
  frameDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  frameDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  frameDsc.header.w = SCR_W; frameDsc.header.h = SCR_H; frameDsc.header.stride = SCR_W * 2;
  frameDsc.data_size = SCR_W * SCR_H * 2;
  frameDsc.data = (const uint8_t *)engine.pixels();
  lv_image_set_src(frameImg, &frameDsc);
  show(frameImg, true);
  syncPatternUi();
  if (ambient) applyBacklight();
  lastFrameMs = 0; autoElapsedMs = 0; perfLastMs = lv_tick_get(); perfSteps = perfStepUs = 0;
  showControls(!ambient);
  lv_timer_resume(frameTimer);
}

void artOnExit() {
  lv_timer_pause(frameTimer);
  lv_timer_pause(hideTimer);
  show(panel, false);
  savedPattern = engine.pattern();
  saveSettings();
  lv_image_set_src(frameImg, NULL);                    // LVGL must not reference the buffer anymore
  engine.end();                                        // frees frame buffer + pattern state
  if (ambient) analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255));   // other screens: normal brightness
}
