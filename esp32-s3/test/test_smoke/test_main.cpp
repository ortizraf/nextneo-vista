// Suite: smoke test for quick deploys (QUICK=1 ./tools/deploy_nextneo_vista.sh).
// One firmware, ~1 min: boots the app, opens every screen once and goes back to the
// menu. The full suites (menu, screens, lifecycle) check much more; this one only
// proves that nothing crashes or hangs after a small change.
#include "../support/ui_harness.h"

void setUp() {}
void tearDown() {}

static void test_boot_lands_on_menu() { ui::assertOnMenu(); }

static void test_every_screen_opens_and_returns() {
  for (const ui::ScreenCase &c : ui::kScreens) {
    switchScreen(c.id);
    ui::pump(400);                     // timers, first render, the first job after the debounce
    ui::assertOnScreen(c);
    ui::goToMenu();
    ui::assertOnMenu();
  }
}

void setup() {
  ui::bootApp();
  UNITY_BEGIN();
  RUN_TEST(test_boot_lands_on_menu);
  RUN_TEST(test_every_screen_opens_and_returns);
  UNITY_END();
}

void loop() { delay(1000); }
