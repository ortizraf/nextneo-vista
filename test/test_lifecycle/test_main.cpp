// Suite: screen manager (switchScreen). Invalid IDs are ignored, re-entering the
// active screen is a no-op, and cycling through every screen does not leak memory.
#include "../support/ui_harness.h"

void setUp() { ui::goToMenu(); }
void tearDown() {}

static void test_invalid_ids_are_ignored() {
  switchScreen(SCREEN_NONE);
  switchScreen(SCREEN_COUNT);
  switchScreen((ScreenID)200);
  ui::pump(50);
  ui::assertOnMenu();
}

static void test_switch_to_active_screen_is_noop() {
  switchScreen(SCREEN_WEATHER);
  switchScreen(SCREEN_WEATHER);
  ui::pump(50);
  ui::assertOnScreen(ui::screenCase(SCREEN_WEATHER));
}

static void test_direct_switch_between_screens() {
  // Screen -> screen without the menu in between (e.g. web page / future idle manager).
  for (int i = 0; i < ui::kScreenCount; ++i) {
    const ui::ScreenCase &c = ui::kScreens[i];
    switchScreen(c.id);
    ui::pump(100);
    ui::assertOnScreen(c);
  }
}

static void cycleAll(int rounds) {
  for (int r = 0; r < rounds; ++r)
    for (const ui::ScreenCase &c : ui::kScreens) {
      switchScreen(c.id);
      ui::pump(150);       // < debounce: no API job, so memory reflects the UI only
      switchScreen(SCREEN_MENU);
      ui::pump(50);
    }
}

static void test_cycling_screens_does_not_leak() {
  cycleAll(1);                                  // warm-up: first-visit caches, fonts, snapshots
  const ui::HeapSnapshot before = ui::heapNow();
  cycleAll(5);
  const ui::HeapSnapshot after = ui::heapNow();
  const long lostInternal = (long)before.internal - (long)after.internal;
  const long lostPsram = (long)before.psram - (long)after.psram;
  char info[96];
  snprintf(info, sizeof info, "heap lost after 5 rounds: internal %ld B, PSRAM %ld B", lostInternal, lostPsram);
  TEST_MESSAGE(info);
  // Wi-Fi / the worker task allocate on their own, so allow a small margin.
  TEST_ASSERT_LESS_THAN_MESSAGE(8 * 1024, lostInternal, "internal RAM leak while switching screens");
  TEST_ASSERT_LESS_THAN_MESSAGE(64 * 1024, lostPsram, "PSRAM leak while switching screens");
}

void setup() {
  ui::bootApp();
  UNITY_BEGIN();
  RUN_TEST(test_invalid_ids_are_ignored);
  RUN_TEST(test_switch_to_active_screen_is_noop);
  RUN_TEST(test_direct_switch_between_screens);
  RUN_TEST(test_cycling_screens_does_not_leak);
  UNITY_END();
}

void loop() { delay(1000); }
