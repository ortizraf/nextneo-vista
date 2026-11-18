// Suite: screen manager (registry + switchScreen). Every screen is registered in
// ScreenID order, invalid IDs are ignored, re-entering the
// active screen is a no-op, and cycling through every screen does not leak memory.
#include "../support/ui_harness.h"
#include "modules/calendar/holidays.h"

void setUp() { ui::goToMenu(); }
void tearDown() {}

static void test_registry_lists_every_screen_in_order() {
  for (int i = SCREEN_MENU; i < SCREEN_COUNT; ++i) {
    const ScreenLifecycle *s = screenLifecycle((ScreenID)i);
    TEST_ASSERT_EQUAL_MESSAGE(i, s->id, s->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(s->build, s->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(screenView((ScreenID)i), s->name);
    for (uint8_t j = 0; j < s->jobCount; ++j) TEST_ASSERT_NOT_NULL_MESSAGE(s->jobs[j].run, s->name);
  }
  TEST_ASSERT_EQUAL(SCREEN_NONE, screenLifecycle((ScreenID)200)->id);   // unknown id -> "none"
}

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

// Holidays API (BrasilAPI): only the Calendar screen owns the job, and the fetch never
// runs while another screen is active (nor right after leaving the Calendar).
static void test_holidays_api_only_on_calendar() {
  const ScreenLifecycle *cal = screenLifecycle(SCREEN_CALENDARIO);
  TEST_ASSERT_EQUAL_UINT8(1, cal->jobCount);
  TEST_ASSERT_EQUAL_PTR(holidaysFetch, cal->jobs[0].run);
  for (int id = SCREEN_NONE + 1; id < SCREEN_COUNT; ++id) {        // no other screen posts it
    const ScreenLifecycle *s = screenLifecycle((ScreenID)id);
    if (s == cal) continue;
    for (uint8_t j = 0; j < s->jobCount; ++j) TEST_ASSERT_NOT_EQUAL_MESSAGE(holidaysFetch, s->jobs[j].run, s->name);
  }
  holidaysWant(2099);                         // a year with no cache -> pending
  ui::goToMenu();
  TEST_ASSERT_FALSE_MESSAGE(holidaysNeedFetch(), "fetch requested outside the Calendar");
  TEST_ASSERT_FALSE_MESSAGE(holidaysFetch(), "API called outside the Calendar");
  switchScreen(SCREEN_CALENDARIO);
  ui::goToMenu();                             // left the Calendar
  TEST_ASSERT_FALSE_MESSAGE(holidaysNeedFetch(), "fetch requested after leaving the Calendar");
  TEST_ASSERT_FALSE_MESSAGE(holidaysFetch(), "API called after leaving the Calendar");
}

void setup() {
  ui::bootApp();
  UNITY_BEGIN();
  RUN_TEST(test_registry_lists_every_screen_in_order);
  RUN_TEST(test_invalid_ids_are_ignored);
  RUN_TEST(test_switch_to_active_screen_is_noop);
  RUN_TEST(test_direct_switch_between_screens);
  RUN_TEST(test_cycling_screens_does_not_leak);
  RUN_TEST(test_holidays_api_only_on_calendar);
  UNITY_END();
}

void loop() { delay(1000); }
