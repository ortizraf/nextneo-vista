// Suite: the 13 app screens. Each one opens, keeps running (timers, render,
// first API request after the debounce) without crashing, and its back button
// returns to the menu.
#include "../support/ui_harness.h"

static constexpr uint32_t STAY_MS = 1500;   // > SCREEN_ENTER_DEBOUNCE_MS: the first API job is queued

void setUp() { ui::goToMenu(); }
void tearDown() { ui::goToMenu(); }

static void screenHasBackButton(const ui::ScreenCase &c) {
  TEST_ASSERT_NOT_NULL_MESSAGE(ui::backButton(*c.view), c.caption);
}
UI_TEST_PER_SCREEN(test_has_back_button, screenHasBackButton)

static void screenStaysAliveAndActive(const ui::ScreenCase &c) {
  switchScreen(c.id);
  ui::pump(STAY_MS);
  ui::assertOnScreen(c);   // nothing (timer, render, job) moved us elsewhere
}
UI_TEST_PER_SCREEN(test_stays_active, screenStaysAliveAndActive)

static void backButtonReturnsToMenu(const ui::ScreenCase &c) {
  switchScreen(c.id);
  ui::pump(300);
  lv_obj_t *back = ui::backButton(*c.view);
  TEST_ASSERT_NOT_NULL_MESSAGE(back, c.caption);
  ui::tap(back);
  ui::assertOnMenu();
}
UI_TEST_PER_SCREEN(test_back_returns_to_menu, backButtonReturnsToMenu)

void setup() {
  ui::bootApp();
  UNITY_BEGIN();
  UI_RUN_PER_SCREEN(test_has_back_button);
  UI_RUN_PER_SCREEN(test_stays_active);
  UI_RUN_PER_SCREEN(test_back_returns_to_menu);
  UNITY_END();
}

void loop() { delay(1000); }
