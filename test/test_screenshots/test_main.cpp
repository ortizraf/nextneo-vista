// Screenshot tool (NOT part of the deploy tests): opens the menu and every screen,
// waits for its data and streams an LVGL snapshot over USB. Run it with
//   ./tools/capture_screenshots.sh
// which saves docs/screens/<screen>.png. Privacy: the city name and the location used
// on screen are replaced in RAM by generic values before anything is drawn, the Fotos
// screen is skipped, and tools/capture_screenshots.py paints over the Wi-Fi name / IP.
#include "../support/ui_harness.h"
#include "modules/weather/weather.h"
#include "modules/garden/garden.h"
#include <sys/time.h>

static constexpr uint32_t SETTLE_MS = 9000;      // data of the screen (first API job + render)

static void b64Line(const uint8_t *p, size_t n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static char out[1400];
  size_t o = 0;
  out[o++] = 'B'; out[o++] = ':';
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
    out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? T[v & 63] : '=';
  }
  out[o++] = '\n';
  Serial.write((const uint8_t *)out, o);
}

// Allocated once before any screen runs: screens like Arte take most of the PSRAM.
static lv_draw_buf_t *snap;

static void capture(const char *name) {
  lv_obj_update_layout(lv_screen_active());
  lv_refr_now(nullptr);
  if (!snap || lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, snap) != LV_RESULT_OK) {
    Serial.printf("SKIP %s (snapshot falhou)\n", name);
    return;
  }
  const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
  Serial.printf("IMG %s %lu %lu\n", name, (unsigned long)w, (unsigned long)h);
  for (uint32_t y = 0; y < h; ++y) b64Line(snap->data + y * stride, w * 2);   // RGB565 little-endian
  Serial.printf("END %s\n", name);
  Serial.flush();
}

static void hideRealLocation() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  wx.cityName = "Minha cidade";
  xSemaphoreGive(mtx);
  cfg.lat = -23.5505f; cfg.lon = -46.6333f;   // public city centre, not the real address
}

void setUp() {}
void tearDown() {}

// Not captured: private content (Fotos shows the user's own photos).
static bool isPrivate(ScreenID id) { return id == SCREEN_PHOTOS; }

// Garden: the plant at each stage (a sample of species), saved as garden-<species>-<stage>.png.
// The owner's real plant is put back afterwards.
static void captureGardenStages() {
  using namespace garden;
  if (time(nullptr) < 1700000000) { struct timeval tv = {1794700800, 0}; settimeofday(&tv, nullptr); }
  State owner; const bool had = prefs.getBytes("garden", &owner, sizeof owner) == sizeof owner;
  static const uint8_t kShots[][2] = {{0, STAGE_SPROUT}, {0, STAGE_YOUNG}, {0, STAGE_BUD}, {0, STAGE_BLOOM},
                                      {1, STAGE_BLOOM}, {2, STAGE_BLOOM}, {3, STAGE_BLOOM}, {4, STAGE_BLOOM},
                                      {5, STAGE_BLOOM}, {6, STAGE_BLOOM}, {7, STAGE_BLOOM}, {7, STAGE_GROWN}};
  static const float kAt[STAGE_COUNT] = {0.04f, 0.16f, 0.4f, 0.62f, 0.86f, 1.02f, 1.5f};
  for (const auto &k : kShots) {
    State s; plant(s, k[0], (uint32_t)time(nullptr));
    s.growDays = kSpecies[k[0]].bloomDays * kAt[k[1]];
    switchScreen(SCREEN_MENU); ui::pump(200);
    prefs.putBytes("garden", &s, sizeof s); gardenReload();
    switchScreen(SCREEN_GARDEN); ui::pump(2500);
    char n[40]; snprintf(n, sizeof n, "garden-%d-%d", k[0], k[1]);
    capture(n);
  }
  { State s; plant(s, 0, (uint32_t)time(nullptr)); s.growDays = 16; s.water = 5;   // thirsty: wilting
    switchScreen(SCREEN_MENU); ui::pump(200);
    prefs.putBytes("garden", &s, sizeof s); gardenReload();
    switchScreen(SCREEN_GARDEN); ui::pump(2500); capture("garden-wilt");
    switchScreen(SCREEN_MENU); ui::pump(1500); capture("menu-thirsty"); }
  { State s; plant(s, 0, (uint32_t)time(nullptr)); s.growDays = 22; s.seenStage = STAGE_BLOOM;
    switchScreen(SCREEN_MENU); ui::pump(200);
    prefs.putBytes("garden", &s, sizeof s); gardenReload();
    switchScreen(SCREEN_GARDEN); ui::pump(800); gardenButterflyGo(); ui::pump(5000); capture("garden-butterfly"); }
  if (had) prefs.putBytes("garden", &owner, sizeof owner); else prefs.remove("garden");
  gardenReload();
  switchScreen(SCREEN_MENU); ui::pump(500);
}

static void test_capture_all() {
  snap = lv_draw_buf_create(SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565, 0);
  TEST_ASSERT_NOT_NULL_MESSAGE(snap, "sem memória para o buffer da imagem");
  hideRealLocation();
#ifdef SHOT_GARDEN
  captureGardenStages();          // ONLY=garden ./tools/capture_screenshots.sh
  return;
#endif
  ui::goToMenu();
  ui::pump(3000);
  capture("menu");
  for (const ui::ScreenCase &c : ui::kScreens) {
    if (isPrivate(c.id)) continue;
    switchScreen(c.id);
    ui::pump(SETTLE_MS);
    hideRealLocation();          // a fetch may have written the real city name back
    ui::pump(600);
    capture(screenLifecycle(c.id)->name);
  }
  switchScreen(SCREEN_MENU);
}

void setup() {
  ui::bootApp();
  // wait (up to 2 min) for the host script, so no image is lost before it listens
  const uint32_t t0 = millis();
  while (millis() - t0 < 120000) {
    if (Serial.available() && Serial.read() == 'G') break;
    Serial.println("READY");
    delay(500);
  }
  UNITY_BEGIN();
  RUN_TEST(test_capture_all);
  UNITY_END();
  Serial.println("DONE");
}

void loop() { delay(1000); }
