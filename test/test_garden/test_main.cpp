// Suite: Garden ("Jardim") screen on the board. Planting from the seed picker,
// watering, replanting a dried plant, the album, persistence and the menu position.
// The owner's real plant (Preferences "garden") is saved before and restored after.
#include <sys/time.h>
#include "../support/ui_harness.h"
#include "modules/garden/garden.h"
#include "modules/garden/garden_art.h"

using namespace garden;

static State ownerPlant;
static bool ownerHadPlant = false;

static lv_obj_t *view() { return screenView(SCREEN_GARDEN); }

static lv_obj_t *findLabel(lv_obj_t *root, const char *text) {
  if (!root) return nullptr;
  if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
  const uint32_t n = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < n; ++i)
    if (lv_obj_t *hit = findLabel(lv_obj_get_child(root, i), text)) return hit;
  return nullptr;
}
static lv_obj_t *seedCard(int species) {
  return ui::findButtonWithLabel(view(), T(kSpecies[species].pt, kSpecies[species].en));
}
static bool pickerOpen() {
  lv_obj_t *card = seedCard(0);
  return card && ui::isVisible(lv_obj_get_parent(card));
}
static lv_obj_t *waterButton() { return ui::findButtonWithLabel(view(), LV_SYMBOL_TINT); }
static lv_obj_t *replantButton() { return ui::findButtonWithLabel(view(), LV_SYMBOL_PLUS); }

static void saveAndOpen(const State &s) {
  ui::goToMenu();                    // re-entering runs onEnter (switching to the active screen is a no-op)
  prefs.putBytes("garden", &s, sizeof s);
  gardenReload();
  switchScreen(SCREEN_GARDEN);
  ui::pump(300);
}

void setUp() {
  ui::goToMenu();
  saveAndOpen(State());              // every test starts with an empty pot
}
void tearDown() {}

static void test_empty_pot_opens_the_seed_picker() {
  ui::assertOnScreen(ui::screenCase(SCREEN_GARDEN));
  TEST_ASSERT_TRUE(pickerOpen());
  for (int i = 0; i < SPECIES_COUNT; ++i) TEST_ASSERT_NOT_NULL_MESSAGE(seedCard(i), kSpecies[i].pt);
}

static void test_picking_a_seed_plants_it_and_saves() {
  ui::tap(seedCard(0));
  TEST_ASSERT_FALSE(pickerOpen());
  TEST_ASSERT_EQUAL(0, gardenState().species);
  TEST_ASSERT_EQUAL(1, gardenState().alive);
  TEST_ASSERT_NOT_NULL(findLabel(view(), T("Girassol", "Sunflower")));
  State saved;
  TEST_ASSERT_EQUAL(sizeof saved, prefs.getBytes("garden", &saved, sizeof saved));
  TEST_ASSERT_EQUAL(0, saved.species);
}

static void test_watering_adds_water_and_saves() {
  ui::tap(seedCard(1));
  gardenState().water = 20;
  ui::tap(waterButton());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 20 + WATER_STEP, gardenState().water);
  State saved;
  prefs.getBytes("garden", &saved, sizeof saved);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 20 + WATER_STEP, saved.water);
  ui::pump(900);                     // drop animation runs and ends without trouble
  ui::assertOnScreen(ui::screenCase(SCREEN_GARDEN));
}

static void test_dried_plant_offers_replanting() {
  State dead;
  plant(dead, 7, (uint32_t)time(nullptr));
  dead.alive = 0; dead.health = 0; dead.water = 0;
  saveAndOpen(dead);
  TEST_ASSERT_NULL(waterButton());
  ui::tap(replantButton());
  TEST_ASSERT_TRUE(pickerOpen());
  ui::tap(seedCard(2));
  TEST_ASSERT_EQUAL(2, gardenState().species);
  TEST_ASSERT_EQUAL(1, gardenState().alive);
  TEST_ASSERT_NOT_NULL(waterButton());
}

static void test_album_marks_bloomed_species() {
  State s;
  plant(s, 0, (uint32_t)time(nullptr));
  s.album = 1u << 3;                 // orchid already bloomed once
  saveAndOpen(s);
  TEST_ASSERT_FALSE(pickerOpen());
  ui::tap(   // the name card (the label's parent) opens the picker
lv_obj_get_parent(findLabel(view(), T("Girassol", "Sunflower"))));
  TEST_ASSERT_TRUE(pickerOpen());
  lv_obj_t *orchidCheck = findLabel(seedCard(3), LV_SYMBOL_OK);
  lv_obj_t *sunCheck = findLabel(seedCard(0), LV_SYMBOL_OK);
  TEST_ASSERT_TRUE(ui::isVisible(orchidCheck));
  TEST_ASSERT_FALSE(ui::isVisible(sunCheck));
}

static void test_plant_survives_leaving_the_screen() {
  ui::tap(seedCard(5));
  ui::tap(ui::backButton(view()));
  ui::assertOnMenu();
  gardenReload();                    // as after a reboot: read from flash
  TEST_ASSERT_EQUAL(5, gardenState().species);
}

static void test_menu_order_garden_page_one_f1_and_football_page_two() {
  ui::goToMenu();
  lv_obj_t *garden = ui::menuTile(T("Jardim", "Garden"));
  lv_obj_t *f1 = ui::menuTile("F1");
  lv_obj_t *football = ui::menuTile(T("Futebol", "Football"));
  lv_obj_t *weather = ui::menuTile(T("Clima", "Weather"));
  TEST_ASSERT_NOT_NULL(garden); TEST_ASSERT_NOT_NULL(f1); TEST_ASSERT_NOT_NULL(football);
  lv_obj_t *page1 = lv_obj_get_parent(weather), *page2 = lv_obj_get_parent(football);
  TEST_ASSERT_TRUE(lv_obj_get_parent(garden) == page1);
  TEST_ASSERT_EQUAL(10, lv_obj_get_index(garden));            // 11th tile: F1's old place
  TEST_ASSERT_TRUE(lv_obj_get_parent(f1) == page2);
  TEST_ASSERT_EQUAL(0, lv_obj_get_index(f1));                 // 13th app
  TEST_ASSERT_EQUAL(1, lv_obj_get_index(football));           // 14th app
}

static State grown(uint8_t species, Stage at, uint8_t seen) {
  static const float kAt[STAGE_COUNT] = {0.04f, 0.16f, 0.4f, 0.62f, 0.86f, 1.02f, 1.5f};
  State s; plant(s, species, (uint32_t)time(nullptr));
  s.growDays = kSpecies[species].bloomDays * kAt[at];
  s.seenStage = seen;
  return s;
}

static void test_pre_rendered_plant_is_decoded() {
  saveAndOpen(grown(3, STAGE_BLOOM, STAGE_BLOOM));
  const lv_draw_buf_t *pic = gardenPlantPicture();
  TEST_ASSERT_NOT_NULL(pic);
  TEST_ASSERT_EQUAL(kGardenPlant[3][3].h, pic->header.h);
  TEST_ASSERT_EQUAL(kGardenPlant[3][3].w, pic->header.w);
  saveAndOpen(grown(3, STAGE_BLOOM, STAGE_BLOOM));
  State dry = gardenState(); dry.alive = 0; dry.health = 0;
  saveAndOpen(dry);                                  // dried: drooping picture is wider
  TEST_ASSERT_TRUE(gardenPlantPicture()->header.w > kGardenPlant[3][3].w);
}

static void test_tree_line_backdrop_is_decoded() {
  const lv_draw_buf_t *bg = gardenBackdrop();
  TEST_ASSERT_NOT_NULL(bg);
  TEST_ASSERT_EQUAL(kGardenBackdrop.w, bg->header.w);
  TEST_ASSERT_EQUAL(kGardenBackdrop.h, bg->header.h);
  const uint32_t *px = (const uint32_t *)bg->data;
  const uint32_t bottom = px[(bg->header.h - 1) * (bg->header.stride / 4) + bg->header.w / 2];
  TEST_ASSERT_TRUE_MESSAGE((bottom >> 24) > 200, "hedge meets the lawn (opaque bottom)");
}

static void test_new_stage_celebrates_once() {
  saveAndOpen(grown(0, STAGE_YOUNG, STAGE_SPROUT));
  TEST_ASSERT_TRUE(ui::isVisible(gardenToast()));
  TEST_ASSERT_NOT_NULL(findLabel(gardenToast(), T("Nova fase: muda!", "New stage: seedling!")));
  State saved;
  prefs.getBytes("garden", &saved, sizeof saved);
  TEST_ASSERT_EQUAL(STAGE_YOUNG, saved.seenStage);
  ui::pump(3800);
  TEST_ASSERT_FALSE(ui::isVisible(gardenToast()));   // goes away by itself
  saveAndOpen(saved);
  TEST_ASSERT_FALSE(ui::isVisible(gardenToast()));   // only once per stage
}

static void test_butterfly_visits_only_flowers() {
  saveAndOpen(grown(0, STAGE_GROWN, STAGE_GROWN));
  gardenButterflyGo();
  ui::pump(300);
  TEST_ASSERT_FALSE(ui::isVisible(gardenButterfly()));
  saveAndOpen(grown(0, STAGE_BLOOM, STAGE_BLOOM));
  gardenButterflyGo();
  ui::pump(300);
  TEST_ASSERT_TRUE(ui::isVisible(gardenButterfly()));
  ui::tap(ui::backButton(view()));
  TEST_ASSERT_FALSE(ui::isVisible(gardenButterfly()));   // leaves with the screen
}

static lv_obj_t *thirstBadge() {
  lv_obj_t *drop = findLabel(ui::menuTile(T("Jardim", "Garden")), LV_SYMBOL_TINT);
  return drop ? lv_obj_get_parent(drop) : nullptr;
}
static void test_menu_drop_badge_when_thirsty() {
  State s = grown(0, STAGE_YOUNG, STAGE_YOUNG);
  s.water = 10;
  saveAndOpen(s);
  ui::goToMenu(); ui::pump(300);
  TEST_ASSERT_NOT_NULL(thirstBadge());
  TEST_ASSERT_TRUE(ui::isVisible(thirstBadge()));
  switchScreen(SCREEN_GARDEN); ui::pump(300);
  ui::tap(waterButton());                                // watering clears it right away
  ui::goToMenu(); ui::pump(300);
  TEST_ASSERT_FALSE(ui::isVisible(thirstBadge()));
  saveAndOpen(State());                                  // no plant: no badge
  ui::goToMenu(); ui::pump(300);
  TEST_ASSERT_FALSE(ui::isVisible(thirstBadge()));
}

void setup() {
  ui::bootApp();
  if (time(nullptr) < 1700000000) {   // planting needs a clock; no NTP needed for the test
    struct timeval tv = {1794700800, 0};   // 2026-11-15
    settimeofday(&tv, nullptr);
  }
  ownerHadPlant = prefs.getBytes("garden", &ownerPlant, sizeof ownerPlant) == sizeof ownerPlant;
  UNITY_BEGIN();
  RUN_TEST(test_empty_pot_opens_the_seed_picker);
  RUN_TEST(test_picking_a_seed_plants_it_and_saves);
  RUN_TEST(test_watering_adds_water_and_saves);
  RUN_TEST(test_dried_plant_offers_replanting);
  RUN_TEST(test_album_marks_bloomed_species);
  RUN_TEST(test_plant_survives_leaving_the_screen);
  RUN_TEST(test_menu_order_garden_page_one_f1_and_football_page_two);
  RUN_TEST(test_pre_rendered_plant_is_decoded);
  RUN_TEST(test_tree_line_backdrop_is_decoded);
  RUN_TEST(test_new_stage_celebrates_once);
  RUN_TEST(test_butterfly_visits_only_flowers);
  RUN_TEST(test_menu_drop_badge_when_thirsty);
  if (ownerHadPlant) prefs.putBytes("garden", &ownerPlant, sizeof ownerPlant);   // give the real plant back
  else prefs.remove("garden");
  gardenReload();
  UNITY_END();
}

void loop() { delay(1000); }
