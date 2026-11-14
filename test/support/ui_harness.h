#pragma once
// Shared harness for the on-device UI tests (header-only, included by every suite).
// Each suite under test/test_*/ is its own firmware: it boots the real app with
// appSetup() and drives the UI through LVGL events, the same path a finger takes.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <unity.h>
#include "../../src/nextneo-vista/app_screens.h"
#include "../../src/nextneo-vista/services/app_log.h"

namespace ui {

// One row per app screen reachable from the main menu (13 = every ScreenID but
// NONE and MENU). `caption` is the text under the menu icon.
struct ScreenCase {
  ScreenID id;
  const char *caption;
  lv_obj_t *view() const { return screenView(id); }   // container owned by the screen's module
};

static const ScreenCase kScreens[] = {
  {SCREEN_WEATHER,      "Clima"},
  {SCREEN_FINANCE,      "Finanças"},
  {SCREEN_CALENDARIO,   "Calendário"},
  {SCREEN_FLIGHT_RADAR, "Radar"},
  {SCREEN_SETTINGS,     "Ajustes"},
  {SCREEN_SERVERS,      "Servidores"},
  {SCREEN_CLOCK,        "Relógio"},
  {SCREEN_AIRPORT,      "Aeroporto"},
  {SCREEN_PHOTOS,       "Fotos"},
  {SCREEN_SKY,          "C\xC3\xA9u"},
  {SCREEN_F1,           "F1"},
  {SCREEN_ART,          "Arte"},
  {SCREEN_FOOTBALL,     "Futebol"},
};
static constexpr int kScreenCount = sizeof(kScreens) / sizeof(kScreens[0]);
static_assert(kScreenCount == SCREEN_COUNT - 2, "every app screen needs a test case");

inline const ScreenCase &screenCase(ScreenID id) {
  for (const ScreenCase &c : kScreens) if (c.id == id) return c;
  TEST_FAIL_MESSAGE("screen has no test case");
  return kScreens[0];
}

// ---- time: run the LVGL loop (timers, renders, animations) for `ms`
inline void pump(uint32_t ms) {
  const uint32_t start = millis();
  do {
    lv_timer_handler();
    delay(5);
  } while (millis() - start < ms);
}

// ---- boot: the real app, once per suite firmware
inline void bootApp() {
  delay(2000);           // lets the serial monitor attach before Unity prints
  appSetup();
  pump(500);
  // From here the serial belongs to Unity: app log lines mixed into its output
  // break PlatformIO's result parsing. The log is still kept for http://IP/log.
  Serial.flush();
  gLogSerialEcho = false;
  // appSetup() sets a 0 ms USB TX timeout so a full buffer never blocks the app; here
  // that would drop bytes of Unity's report (merged/missing results). Wait instead.
  Serial.setTxTimeoutMs(500);
}

// ---- widget lookup
inline bool isVisible(lv_obj_t *obj) { return obj && !lv_obj_is_hidden(obj); }

inline bool hasLabelChild(lv_obj_t *obj, const char *text) {
  const uint32_t n = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t *c = lv_obj_get_child(obj, i);
    if (lv_obj_check_type(c, &lv_label_class) && strcmp(lv_label_get_text(c), text) == 0) return true;
  }
  return false;
}

// Depth-first search for a button that holds a label with `text`.
inline lv_obj_t *findButtonWithLabel(lv_obj_t *root, const char *text) {
  if (!root) return nullptr;
  if (lv_obj_check_type(root, &lv_button_class) && hasLabelChild(root, text)) return root;
  const uint32_t n = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < n; ++i)
    if (lv_obj_t *hit = findButtonWithLabel(lv_obj_get_child(root, i), text)) return hit;
  return nullptr;
}

inline lv_obj_t *mainMenu() { return screenView(SCREEN_MENU); }
inline lv_obj_t *menuTile(const char *caption) { return findButtonWithLabel(mainMenu(), caption); }
// addBackButton(): the app's one back button: 40x34 at (4,4) with the LV_SYMBOL_LEFT
// arrow. Size matters: some screens (F1 map) have other left-arrow buttons.
inline bool isBackButton(lv_obj_t *o) {
  return lv_obj_check_type(o, &lv_button_class) && hasLabelChild(o, LV_SYMBOL_LEFT) &&
         lv_obj_get_width(o) == 40 && lv_obj_get_height(o) == 34;
}
inline lv_obj_t *findBack(lv_obj_t *root) {
  if (!root) return nullptr;
  if (isBackButton(root)) return root;
  const uint32_t n = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < n; ++i)
    if (lv_obj_t *hit = findBack(lv_obj_get_child(root, i))) return hit;
  return nullptr;
}
inline lv_obj_t *backButton(lv_obj_t *view) {
  lv_obj_update_layout(view);
  return findBack(view);
}

// ---- actions: same events a tap produces
inline void tap(lv_obj_t *obj) {
  TEST_ASSERT_NOT_NULL_MESSAGE(obj, "nothing to tap");
  lv_obj_send_event(obj, LV_EVENT_PRESSED, nullptr);
  lv_obj_send_event(obj, LV_EVENT_RELEASED, nullptr);
  lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
  pump(50);
}

inline void goToMenu() {
  switchScreen(SCREEN_MENU);
  pump(400);              // menu taps are ignored for 300 ms after a page swipe
}

// ---- assertions
// Exactly one view is visible: the expected one. `expectedView` may be the menu.
inline void assertOnlyVisible(lv_obj_t *expectedView, const char *name) {
  char msg[96];
  snprintf(msg, sizeof msg, "%s view should be visible", name);
  TEST_ASSERT_TRUE_MESSAGE(isVisible(expectedView), msg);
  lv_obj_t *all[kScreenCount + 1];
  all[0] = mainMenu();
  for (int i = 0; i < kScreenCount; ++i) all[i + 1] = kScreens[i].view();
  for (lv_obj_t *v : all) {
    if (v == expectedView) continue;
    snprintf(msg, sizeof msg, "another view is visible while on %s", name);
    TEST_ASSERT_FALSE_MESSAGE(isVisible(v), msg);
  }
}

inline void assertOnMenu() {
  TEST_ASSERT_EQUAL_MESSAGE(SCREEN_MENU, activeScreenId(), "active screen should be the menu");
  assertOnlyVisible(mainMenu(), "menu");
}

inline void assertOnScreen(const ScreenCase &c) {
  TEST_ASSERT_EQUAL_MESSAGE(c.id, activeScreenId(), c.caption);
  assertOnlyVisible(c.view(), c.caption);
}

// ---- memory
struct HeapSnapshot {
  size_t internal, psram;
};
inline HeapSnapshot heapNow() {
  return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM)};
}

}  // namespace ui

// One Unity test function per screen, from a body that takes the ScreenCase.
#define UI_TEST_PER_SCREEN(prefix, body)                                           \
  static void prefix##_weather()      { body(ui::screenCase(SCREEN_WEATHER)); }      \
  static void prefix##_finance()      { body(ui::screenCase(SCREEN_FINANCE)); }      \
  static void prefix##_calendario()   { body(ui::screenCase(SCREEN_CALENDARIO)); }   \
  static void prefix##_flight_radar() { body(ui::screenCase(SCREEN_FLIGHT_RADAR)); } \
  static void prefix##_settings()     { body(ui::screenCase(SCREEN_SETTINGS)); }     \
  static void prefix##_servers()      { body(ui::screenCase(SCREEN_SERVERS)); }      \
  static void prefix##_clock()        { body(ui::screenCase(SCREEN_CLOCK)); }        \
  static void prefix##_airport()      { body(ui::screenCase(SCREEN_AIRPORT)); }      \
  static void prefix##_photos()       { body(ui::screenCase(SCREEN_PHOTOS)); }       \
  static void prefix##_sky()          { body(ui::screenCase(SCREEN_SKY)); }          \
  static void prefix##_f1()           { body(ui::screenCase(SCREEN_F1)); }           \
  static void prefix##_art()          { body(ui::screenCase(SCREEN_ART)); }          \
  static void prefix##_football()     { body(ui::screenCase(SCREEN_FOOTBALL)); }

#define UI_RUN_PER_SCREEN(prefix)   \
  RUN_TEST(prefix##_weather);       \
  RUN_TEST(prefix##_finance);       \
  RUN_TEST(prefix##_calendario);    \
  RUN_TEST(prefix##_flight_radar);  \
  RUN_TEST(prefix##_settings);      \
  RUN_TEST(prefix##_servers);       \
  RUN_TEST(prefix##_clock);         \
  RUN_TEST(prefix##_airport);       \
  RUN_TEST(prefix##_photos);        \
  RUN_TEST(prefix##_sky);           \
  RUN_TEST(prefix##_f1);            \
  RUN_TEST(prefix##_art);           \
  RUN_TEST(prefix##_football)
