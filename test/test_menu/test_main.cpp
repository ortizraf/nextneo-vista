// Suite: main menu. Boot lands on the menu, every app has a tile, and each tile
// opens its own screen.
#include "../support/ui_harness.h"

void setUp() { ui::goToMenu(); }
void tearDown() {}

static void test_boot_lands_on_menu() {
  ui::assertOnMenu();
}

static void test_menu_has_a_tile_for_every_screen() {
  for (const ui::ScreenCase &c : ui::kScreens) {
    lv_obj_t *tile = ui::menuTile(c.caption());
    TEST_ASSERT_NOT_NULL_MESSAGE(tile, c.caption());
    TEST_ASSERT_TRUE_MESSAGE(lv_obj_is_clickable(tile), c.caption());
  }
}

static void test_menu_works_after_opening_page_two_app() {
  // Futebol lives on page 2; opening it and coming back must leave the menu usable.
  ui::tap(ui::menuTile(T("Futebol", "Football")));
  ui::goToMenu();
  ui::tap(ui::menuTile(T("Clima", "Weather")));
  TEST_ASSERT_EQUAL(SCREEN_WEATHER, activeScreenId());
}

static void tileOpensScreen(const ui::ScreenCase &c) {
  // Arrange: on the menu (setUp). Act: tap the tile. Assert: its screen, alone.
  ui::tap(ui::menuTile(c.caption()));
  ui::assertOnScreen(c);
}
UI_TEST_PER_SCREEN(test_tile_opens, tileOpensScreen)

void setup() {
  ui::bootApp();
  UNITY_BEGIN();
  RUN_TEST(test_boot_lands_on_menu);
  RUN_TEST(test_menu_has_a_tile_for_every_screen);
  RUN_TEST(test_menu_works_after_opening_page_two_app);
  UI_RUN_PER_SCREEN(test_tile_opens);
  UNITY_END();
}

void loop() { delay(1000); }
