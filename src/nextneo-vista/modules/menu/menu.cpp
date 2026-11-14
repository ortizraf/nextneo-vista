// Main menu: iOS-like grid of app icons on swipeable pages. The icons are drawn once
// into a picture (lv_snapshot) and shown as one image; the tiles stay as touch areas.
#include <Arduino.h>
#include <string.h>
#include <lvgl.h>
#include "app_screens.h"
#include "core/screen_registry.h"
#include "modules/servers/servers.h"
#include "services/app_log.h"

static lv_obj_t *mainMenu;
lv_point_precise_t financeLinePoints[FINANCE_MAX_ASSETS][FINANCE_HISTORY] = {};
static lv_obj_t *menuWifiIcon;
static lv_obj_t *menuServersBadge;
static lv_obj_t *menuClock;
static lv_obj_t *menuCalDay, *menuCalWeek;
static constexpr int MENU_ART_MAX = 32;
static lv_obj_t *menuArt[MENU_ART_MAX];
static uint8_t menuArtPage[MENU_ART_MAX];
static int menuArtN = 0;
static int menuSnapKey = -100;
// Menu pages: page 0 = the original 12 apps, page 1 = new apps (Futebol, ...).
// Each page is its own 480x320 object, rendered once into its own picture; a long
// horizontal swipe (MENU_PAGE_SWIPE_PCT of the width) moves the strip between pages.
static constexpr int MENU_PAGES = 2;
static constexpr int MENU_PAGE_SWIPE_PCT = 30;        // drag at least 30 % of the width to change page
static constexpr int MENU_DRAG_START_PX = 10;         // beyond this the touch is a drag, never a tap
static constexpr uint32_t MENU_TAP_BLOCK_MS = 300;    // icons ignore taps right after a page change
static lv_obj_t *menuStrip, *menuPage[MENU_PAGES], *menuDot[MENU_PAGES];
static lv_obj_t *menuSnapImg[MENU_PAGES];
static lv_draw_buf_t *menuSnap[MENU_PAGES];
static int menuBuildPage = 0, menuCurPage = 0;
static bool menuSwDown = false, menuSwDrag = false, menuSwMoved = false;
static lv_point_t menuSwStart;
static int32_t menuSwDx = 0;
static uint32_t menuSwipeEndMs = 0;
static lv_timer_t *menuSwipeTimer = nullptr;

static bool renderedWifiUp = false;
static bool renderedWifiKnown = false;
static void refreshMenuSnapshot(int key) {
  if (!mainMenu || !menuPage[0]) return;
  menuSnapKey = key;
  const uint32_t t0 = millis();
  const bool badge = menuServersBadge && !lv_obj_has_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
  if (badge) lv_obj_add_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);   // live overlay, not baked in
  bool allOk = true;
  for (int p = 0; p < MENU_PAGES; ++p) {
    if (menuSnapImg[p]) lv_obj_add_flag(menuSnapImg[p], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < menuArtN; ++i) if (menuArtPage[i] == p) lv_obj_remove_flag(menuArt[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(menuPage[p]);
    lv_draw_buf_t *nb = lv_snapshot_take(menuPage[p], LV_COLOR_FORMAT_RGB565);
    if (nb) {
      if (!menuSnapImg[p]) {
        menuSnapImg[p] = lv_image_create(menuPage[p]);
        lv_obj_set_pos(menuSnapImg[p], 0, 0);
        lv_obj_remove_flag(menuSnapImg[p], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(menuSnapImg[p]);
      }
      lv_image_set_src(menuSnapImg[p], nb);
      if (menuSnap[p]) lv_draw_buf_destroy(menuSnap[p]);      // image already points to the new one
      menuSnap[p] = nb;
      lv_obj_remove_flag(menuSnapImg[p], LV_OBJ_FLAG_HIDDEN);
      for (int i = 0; i < menuArtN; ++i) if (menuArtPage[i] == p) lv_obj_add_flag(menuArt[i], LV_OBJ_FLAG_HIDDEN);
    } else allOk = false;                                       // no memory: keep that page's live icons
  }
  if (badge) lv_obj_remove_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
  LogV.printf("Menu: imagens %s em %lums\n", allOk ? "geradas" : "FALHARAM", (unsigned long)(millis() - t0));
}

static void renderMenu() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  if (!renderedWifiKnown || renderedWifiUp != up) {
    renderWifiIndicator(menuWifiIcon, up);
    if (up) lv_obj_add_flag(menuWifiIcon, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_remove_flag(menuWifiIcon, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(menuWifiIcon); }
    renderedWifiUp = up;
    renderedWifiKnown = true;
  }
  if (menuClock) {
    struct tm ti; char b[12];
    if (localNow(&ti)) {
      strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M", &ti); setLabelIfChanged(menuClock, b);
      static const char *wd[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
      if (menuCalWeek) setLabelIfChanged(menuCalWeek, wd[ti.tm_wday]);
      if (menuCalDay) { snprintf(b, sizeof b, "%d", ti.tm_mday); setLabelIfChanged(menuCalDay, b); }
      if (menuSnapKey != ti.tm_mday) refreshMenuSnapshot(ti.tm_mday);
    } else if (menuSnapKey == -100) refreshMenuSnapshot(-1);
  }
  if (menuServersBadge) {
    const bool alert = serversAlert();
    if (alert != !lv_obj_has_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN)) {
      if (alert) lv_obj_remove_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void showWeatherEvent(lv_event_t *) { switchScreen(SCREEN_WEATHER); }
static void showSettingsEvent(lv_event_t *) { switchScreen(SCREEN_SETTINGS); }

static void showFinanceEvent(lv_event_t *) { switchScreen(SCREEN_FINANCE); }
static void showCalendarEvent(lv_event_t *) { switchScreen(SCREEN_CALENDARIO); }
static void showFlightRadarEvent(lv_event_t *) { switchScreen(SCREEN_FLIGHT_RADAR); }
static void showServersEvent(lv_event_t *) { switchScreen(SCREEN_SERVERS); }
static void showClockEvent(lv_event_t *) { switchScreen(SCREEN_CLOCK); }
static void showAirportEvent(lv_event_t *) { switchScreen(SCREEN_AIRPORT); }
static void showPhotosEvent(lv_event_t *) { switchScreen(SCREEN_PHOTOS); }
static void showSkyEvent(lv_event_t *) { switchScreen(SCREEN_SKY); }
static void showF1Event(lv_event_t *) { switchScreen(SCREEN_F1); }
static void showArtEvent(lv_event_t *) { switchScreen(SCREEN_ART); }
static void showFootballEvent(lv_event_t *) { switchScreen(SCREEN_FOOTBALL); }

// iOS-style app icon: 64 px squircle-ish tile, vertical gradient (lighter on top),
// 1 px top highlight, soft drop shadow, press = shrink to 92 % + darken.
static constexpr int TILE_W = 112, TILE_H = 94, ICON = 72;

static void noClickBox(lv_obj_t *o) {
  lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- hand-drawn app glyphs (shapes only, no bitmaps) --------------------------

static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius = LV_RADIUS_CIRCLE,
                      lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *o = lv_obj_create(p); noClickBox(o);
  lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0); lv_obj_set_style_bg_opa(o, opa, 0);
  return o;
}
static lv_obj_t *polyline(lv_obj_t *p, const lv_point_precise_t *pts, int n, int width, uint32_t color) {
  lv_obj_t *l = lv_line_create(p);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_pos(l, 0, 0);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}
static void iconBg(lv_obj_t *box, uint32_t top, uint32_t bottom) {
  lv_obj_set_style_bg_color(box, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(box, lv_color_hex(bottom), 0);
}

LV_IMAGE_DECLARE(icon_financas);
static void drawAppGlyph(lv_obj_t *box, const char *app, const char *symbol) {
  if (!strcmp(app, "Arte")) {                        // generative art: glowing waves + particles
    iconBg(box, 0x0B2A6B, 0x020A24);
    static const lv_point_precise_t w1[] = {{8, 40}, {18, 30}, {28, 38}, {38, 48}, {48, 40}, {58, 28}, {64, 32}};
    static const lv_point_precise_t w2[] = {{8, 48}, {18, 42}, {28, 50}, {38, 56}, {48, 50}, {58, 42}, {64, 46}};
    polyline(box, w2, 7, 3, 0x1E6BFF);
    polyline(box, w1, 7, 3, 0x38D6F5);
    blob(box, 16, 14, 5, 5, 0x9FEFFF); blob(box, 34, 18, 4, 4, 0x38D6F5);
    blob(box, 50, 12, 6, 6, 0x9FEFFF); blob(box, 26, 22, 3, 3, 0x1E90FF);
  } else if (!strcmp(app, "Futebol")) {                     // pitch + ball
    iconBg(box, 0x2FB55A, 0x167A36);
    blob(box, 8, 10, 56, 2, 0xFFFFFF, 0, LV_OPA_60); blob(box, 8, 60, 56, 2, 0xFFFFFF, 0, LV_OPA_60);
    blob(box, 35, 10, 2, 52, 0xFFFFFF, 0, LV_OPA_60);
    lv_obj_t *ring = blob(box, 24, 24, 24, 24, 0xFFFFFF, LV_RADIUS_CIRCLE, LV_OPA_TRANSP);
    lv_obj_set_style_border_width(ring, 2, 0); lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
    lv_obj_t *ball = blob(box, 40, 36, 20, 20, 0xFFFFFF);
    lv_obj_set_style_border_width(ball, 1, 0); lv_obj_set_style_border_color(ball, lv_color_hex(0x1B1B1B), 0);
    blob(ball, 6, 6, 7, 7, 0x1B1B1B); blob(ball, 1, 2, 4, 4, 0x1B1B1B); blob(ball, 14, 2, 4, 4, 0x1B1B1B);
    blob(ball, 1, 13, 4, 4, 0x1B1B1B); blob(ball, 14, 13, 4, 4, 0x1B1B1B);
  } else if (!strcmp(app, "Clima")) {                       // sun behind a cloud
    iconBg(box, 0x4FB3FF, 0x1A6FE0);
    blob(box, 34, 12, 24, 24, 0xFFD43B);
    blob(box, 14, 34, 22, 22, 0xFFFFFF); blob(box, 26, 26, 26, 26, 0xFFFFFF);
    blob(box, 42, 34, 18, 18, 0xFFFFFF); blob(box, 14, 42, 46, 14, 0xFFFFFF, 7);
  } else if (!strcmp(app, "Finanças")) {             // NXTN pixel chart (icon_financas.c)
    iconBg(box, 0x44484F, 0x14161A);                   // same grey as the picture's corners
    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, &icon_financas);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);            // centred: the 1 px top highlight border shifts the content area
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  } else if (!strcmp(app, "Servidores")) {           // 3-unit rack with LEDs
    iconBg(box, 0x3A4558, 0x161B24);
    for (int r = 0; r < 3; ++r) {
      lv_obj_t *u = blob(box, 14, 14 + r * 16, 44, 12, 0xDDE3EC, 4);
      lv_obj_set_style_bg_grad_color(u, lv_color_hex(0xA9B3C2), 0); lv_obj_set_style_bg_grad_dir(u, LV_GRAD_DIR_VER, 0);
      blob(u, 6, 4, 4, 4, r == 2 ? 0xFFB020 : 0x2EE07A);
      blob(u, 14, 5, 22, 2, 0x6B7585, 1);
    }
  } else if (!strcmp(app, "Ajustes")) {              // iOS-like grey gear
    iconBg(box, 0xB7BDC6, 0x7D848F);
    lv_obj_t *g = lv_label_create(box); lv_label_set_text(g, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g, lv_color_hex(0x3B4049), 0); lv_obj_center(g);
  } else if (!strcmp(app, "Calendário")) {           // live: weekday + day number
    iconBg(box, 0xFFFFFF, 0xF1F2F5);
    menuCalWeek = lv_label_create(box); lv_label_set_text(menuCalWeek, "SEG");
    lv_obj_set_style_text_font(menuCalWeek, &lv_font_weather_14, 0);
    lv_obj_set_style_text_color(menuCalWeek, lv_color_hex(0xF23B38), 0);
    lv_obj_align(menuCalWeek, LV_ALIGN_TOP_MID, 0, 6);
    menuCalDay = lv_label_create(box); lv_label_set_text(menuCalDay, "--");
    lv_obj_set_style_text_font(menuCalDay, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(menuCalDay, lv_color_hex(0x15171C), 0);
    lv_obj_align(menuCalDay, LV_ALIGN_CENTER, 0, 8);
  } else if (!strcmp(app, "Radar")) {                // rings + sweep + blips
    iconBg(box, 0x1FA27A, 0x0A4F3C);
    for (int i = 0; i < 3; ++i) {
      const int d = 56 - i * 18;
      lv_obj_t *ring = blob(box, (ICON - d) / 2, (ICON - d) / 2, d, d, 0, LV_RADIUS_CIRCLE, LV_OPA_TRANSP);
      lv_obj_set_style_border_width(ring, 2, 0);
      lv_obj_set_style_border_color(ring, lv_color_hex(0xB9FBE0), 0);
      lv_obj_set_style_border_opa(ring, i == 0 ? LV_OPA_70 : LV_OPA_40, 0);
    }
    static const lv_point_precise_t sw[] = {{36, 36}, {56, 18}};
    polyline(box, sw, 2, 3, 0xE9FFF6);
    blob(box, 33, 33, 6, 6, 0xFFFFFF);
    blob(box, 46, 42, 5, 5, 0xFFE066); blob(box, 20, 22, 4, 4, 0xFFE066);
  } else if (!strcmp(app, "Aeroporto")) {           // night runway seen from above + departing jet
    iconBg(box, 0x1B3A63, 0x081426);
    // runway: diagonal strip with edge lights, dashed centreline and threshold bars
    static const lv_point_precise_t rw[] = {{12, 64}, {62, 10}};
    lv_obj_t *strip = polyline(box, rw, 2, 20, 0x2E4560);
    lv_obj_set_style_line_rounded(strip, false, 0);
    lv_obj_t *centre = polyline(box, rw, 2, 2, 0xD9E4F0);
    lv_obj_set_style_line_dash_width(centre, 5, 0); lv_obj_set_style_line_dash_gap(centre, 5, 0);
    lv_obj_set_style_line_opa(centre, LV_OPA_80, 0);
    for (int k = 0; k < 4; ++k) {                       // edge lights along both sides
      const int t = 14 + k * 11;
      blob(box, t - 1, 66 - t - 9, 2, 2, 0xFFD56B);
      blob(box, t + 8, 66 - t, 2, 2, 0xFFD56B);
    }
    // jet climbing out along the runway (same artwork as the radar), white
    extern const lv_image_dsc_t radar_icon_jet;
    lv_obj_t *jet = lv_image_create(box);
    lv_image_set_src(jet, &radar_icon_jet);
    lv_obj_set_size(jet, 28, 28);
    lv_image_set_pivot(jet, 14, 14);
    lv_image_set_antialias(jet, true);
    lv_image_set_scale(jet, 380);                       // ~42 px
    lv_image_set_rotation(jet, 430);                    // nose towards the upper right (43 deg)
    lv_obj_set_style_image_recolor(jet, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_image_recolor_opa(jet, LV_OPA_COVER, 0);
    lv_obj_set_pos(jet, 30, 14);
    lv_obj_remove_flag(jet, LV_OBJ_FLAG_CLICKABLE);
  } else if (!strcmp(app, "Relógio")) {              // white face, 10:10 hands
    iconBg(box, 0x22252B, 0x0B0C0F);
    lv_obj_t *face = blob(box, 8, 8, 56, 56, 0xFFFFFF);
    for (int k = 0; k < 12; ++k) {
      const float a = k * 0.5235988f;
      blob(face, 27 + (int)lroundf(sinf(a) * 23), 27 - (int)lroundf(cosf(a) * 23), 2, 2, 0x2A2D34, 1);
    }
    static const lv_point_precise_t hh[] = {{28, 28}, {17, 21}}, mh[] = {{28, 28}, {44, 17}};
    polyline(face, hh, 2, 4, 0x15171C); polyline(face, mh, 2, 3, 0x15171C);
    blob(face, 25, 25, 6, 6, 0xF49B35);
  } else if (!strcmp(app, "C\xC3\xA9u")) {          // night sky: stars + crescent moon
    iconBg(box, 0x0B1638, 0x27407A);
    static const int st[7][3] = {{10, 12, 3}, {40, 9, 2}, {18, 34, 2}, {44, 40, 3}, {30, 22, 2}, {8, 46, 2}, {50, 24, 2}};
    for (int k = 0; k < 7; ++k) blob(box, st[k][0], st[k][1], st[k][2], st[k][2], 0xEAF0FF);
    blob(box, 26, 14, 24, 24, 0xF4E9C0);
    blob(box, 33, 10, 22, 22, 0x13224C);
  } else if (!strcmp(app, "F1")) {                   // bold italic block "F1" in red + grey speed line
    iconBg(box, 0xFFFFFF, 0xF1F2F5);
    struct Bar { lv_point_precise_t a, b; int w; uint32_t c; };
    static const Bar kBars[] = {
      {{18, 52}, {28, 16}, 10, 0xE10600},               // F stem
      {{22, 20}, {48, 20}, 9, 0xE10600},                // F top arm
      {{18, 35}, {39, 35}, 8, 0xE10600},                // F middle arm
      {{50, 52}, {60, 16}, 10, 0xE10600},               // 1 stem
      {{45, 27}, {55, 19}, 6, 0xE10600},                // 1 flag
      {{9, 58}, {59, 58}, 2, 0xAAAAAF},                 // speed line
    };
    for (const auto &bar : kBars) {
      static lv_point_precise_t pts[sizeof kBars / sizeof kBars[0]][2];
      const int i = &bar - kBars;
      pts[i][0] = bar.a; pts[i][1] = bar.b;
      lv_obj_set_style_line_rounded(polyline(box, pts[i], 2, bar.w, bar.c), false, 0);   // square ends: block letters
    }
  } else if (!strcmp(app, "Fotos")) {                // 8-petal colour flower
    iconBg(box, 0xFFFFFF, 0xF3F4F7);
    static const uint32_t pc[8] = {0xFF9F0A, 0xFFD60A, 0x8FD14F, 0x34C759, 0x32ADE6, 0x5E5CE6, 0xBF5AF2, 0xFF375F};
    for (int k = 0; k < 8; ++k) {
      const float a = k * 0.7853982f;
      blob(box, 26 + (int)lroundf(sinf(a) * 14), 26 - (int)lroundf(cosf(a) * 14), 20, 20, pc[k], LV_RADIUS_CIRCLE, LV_OPA_80);
    }
  } else {
    lv_obj_t *icon = lv_label_create(box); lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0); lv_obj_center(icon);
  }
}

// Menu performance: the icons are ~150 small LVGL objects (gradients, shadows, round
// blobs). Drawing them every time the menu shows/refreshes is slow, so the whole menu
// is rendered ONCE into a 480x320 RGB565 picture (lv_snapshot, PSRAM) and shown as a
// single opaque image; the tiles stay as invisible touch areas on top. The picture is
// rebuilt only when the calendar icon changes (new day).
static void tilePressFx(lv_event_t *e) {
  lv_obj_t *shade = (lv_obj_t *)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
}

// A drag (or the moment right after a page change) never opens an app.
static void menuTileClick(lv_event_t *e) {
  if (menuSwMoved || menuSwDrag || lv_tick_elaps(menuSwipeEndMs) < MENU_TAP_BLOCK_MS) return;
  ((lv_event_cb_t)lv_event_get_user_data(e))(e);
}

static lv_obj_t *menuTile(lv_obj_t *parent, const char *symbol, const char *caption,
                          uint32_t tileColor, lv_event_cb_t callback) {
  lv_obj_t *tile = callback ? lv_button_create(parent) : lv_obj_create(parent);
  lv_obj_set_size(tile, TILE_W, TILE_H);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_shadow_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  if (callback) lv_obj_add_event_cb(tile, menuTileClick, LV_EVENT_CLICKED, (void *)callback);
  else lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);

  const lv_color_t base = lv_color_hex(tileColor);
  lv_obj_t *iconBox = lv_obj_create(tile);
  lv_obj_set_size(iconBox, ICON, ICON);
  lv_obj_align(iconBox, LV_ALIGN_TOP_MID, 0, 0);
  noClickBox(iconBox);
  lv_obj_set_style_radius(iconBox, 17, 0);                       // ~23 % like iOS
  lv_obj_set_style_bg_color(iconBox, lv_color_mix(lv_color_white(), base, LV_OPA_20), 0);
  lv_obj_set_style_bg_grad_color(iconBox, lv_color_mix(lv_color_black(), base, LV_OPA_10), 0);
  lv_obj_set_style_bg_grad_dir(iconBox, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(iconBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(iconBox, LV_BORDER_SIDE_TOP, 0);    // glassy top highlight
  lv_obj_set_style_border_width(iconBox, 1, 0);
  lv_obj_set_style_border_color(iconBox, lv_color_white(), 0);
  lv_obj_set_style_border_opa(iconBox, LV_OPA_30, 0);
  lv_obj_set_style_shadow_width(iconBox, 6, 0);            // small blur: menu redraw ~200 ms -> much less
  lv_obj_set_style_shadow_offset_y(iconBox, 3, 0);
  lv_obj_set_style_shadow_opa(iconBox, LV_OPA_50, 0);
  lv_obj_set_style_shadow_color(iconBox, lv_color_hex(0x01040C), 0);

  drawAppGlyph(iconBox, caption, symbol);
  if (menuArtN < MENU_ART_MAX) { menuArtPage[menuArtN] = menuBuildPage; menuArt[menuArtN++] = iconBox; }
  if (callback) {                                   // press feedback: darken the icon (cheap, no transform layer)
    lv_obj_t *shade = lv_obj_create(tile);
    lv_obj_set_size(shade, ICON, ICON); lv_obj_align(shade, LV_ALIGN_TOP_MID, 0, 0); noClickBox(shade);
    lv_obj_set_style_radius(shade, 17, 0);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0); lv_obj_set_style_bg_opa(shade, LV_OPA_40, 0);
    lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(tile, tilePressFx, LV_EVENT_PRESSED, shade);
    lv_obj_add_event_cb(tile, tilePressFx, LV_EVENT_RELEASED, shade);
    lv_obj_add_event_cb(tile, tilePressFx, LV_EVENT_PRESS_LOST, shade);
  }
  lv_obj_t *label = lv_label_create(tile);
  lv_label_set_text(label, caption);
  lv_obj_set_width(label, TILE_W);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, &lv_font_weather_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF4F7FF), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, ICON + 4);
  if (menuArtN < MENU_ART_MAX) { menuArtPage[menuArtN] = menuBuildPage; menuArt[menuArtN++] = label; }
  return tile;
}

static void menuStripAnim(void *obj, int32_t x) { lv_obj_set_x((lv_obj_t *)obj, x); }
static void menuGoPage(int p, bool anim) {
  if (p < 0) p = 0;
  if (p >= MENU_PAGES) p = MENU_PAGES - 1;
  menuCurPage = p;
  const int32_t target = -p * SCR_W;
  lv_anim_delete(menuStrip, menuStripAnim);
  if (anim && lv_obj_get_x(menuStrip) != target) {
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, menuStrip);
    lv_anim_set_exec_cb(&a, menuStripAnim);
    lv_anim_set_values(&a, lv_obj_get_x(menuStrip), target);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  } else lv_obj_set_x(menuStrip, target);
  for (int i = 0; i < MENU_PAGES; ++i) if (menuDot[i]) {
    lv_obj_set_style_bg_color(menuDot[i], lv_color_hex(i == p ? 0xFFFFFF : 0x5A6E96), 0);
    lv_obj_set_style_bg_opa(menuDot[i], LV_OPA_COVER, 0);
  }
}
// Swipe between pages: polls the touch while the menu is shown (no events needed from
// the icons). Only a long, mostly horizontal drag changes the page.
static void menuSwipeCb(lv_timer_t *) {
  lv_indev_t *ind = lv_indev_get_next(NULL);
  if (!ind || !menuStrip) return;
  const bool pressed = lv_indev_get_state(ind) == LV_INDEV_STATE_PRESSED;
  lv_point_t pt; lv_indev_get_point(ind, &pt);
  if (pressed && !menuSwDown) {                              // finger down
    menuSwDown = true; menuSwDrag = false; menuSwMoved = false; menuSwStart = pt; menuSwDx = 0;
    lv_anim_delete(menuStrip, menuStripAnim);
  } else if (pressed) {                                      // finger moving
    menuSwDx = pt.x - menuSwStart.x;
    const int32_t dy = pt.y - menuSwStart.y;
    if (abs(menuSwDx) > MENU_DRAG_START_PX || abs(dy) > MENU_DRAG_START_PX) menuSwMoved = true;
    if (!menuSwDrag && abs(menuSwDx) > MENU_DRAG_START_PX && abs(menuSwDx) > abs(dy) * 13 / 10) menuSwDrag = true;
    if (menuSwDrag) {
      int32_t x = -menuCurPage * SCR_W + menuSwDx;
      if (x > 40) x = 40;                                    // a little give at the ends
      if (x < -(MENU_PAGES - 1) * SCR_W - 40) x = -(MENU_PAGES - 1) * SCR_W - 40;
      lv_obj_set_x(menuStrip, x);
    }
  } else if (menuSwDown) {                                   // finger up
    menuSwDown = false;
    if (menuSwDrag) {
      int p = menuCurPage;
      if (abs(menuSwDx) >= SCR_W * MENU_PAGE_SWIPE_PCT / 100) p += menuSwDx < 0 ? 1 : -1;
      menuGoPage(p, true);                                   // short drag: springs back
      menuSwipeEndMs = lv_tick_get();
    }
    menuSwDrag = false;
  }
}
static void menuOnEnter() {
  menuSwDown = menuSwDrag = menuSwMoved = false;
  if (menuSwipeTimer) { lv_timer_reset(menuSwipeTimer); lv_timer_resume(menuSwipeTimer); }
}
static void menuOnExit() {
  if (menuSwipeTimer) lv_timer_pause(menuSwipeTimer);
  menuSwDown = menuSwDrag = false;
}

static void buildMenuScreen(lv_obj_t *scr) {
  mainMenu = lv_obj_create(scr);
  lv_obj_set_size(mainMenu, SCR_W, SCR_H);
  lv_obj_set_pos(mainMenu, 0, 0);
  lv_obj_set_style_pad_all(mainMenu, 0, 0);
  lv_obj_set_style_border_width(mainMenu, 0, 0);
  lv_obj_t *menu = mainMenu;
  lv_obj_set_style_bg_color(menu, lv_color_hex(0x06163C), 0);
  lv_obj_set_style_bg_grad_color(menu, lv_color_hex(0x073B86), 0);
  lv_obj_set_style_bg_grad_dir(menu, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
  lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

  // status bar (clock left, Wi-Fi right) like the iPhone
  menuClock = makeLabel(menu, "--:--", &lv_font_weather_14, 0xEAF2FF);
  lv_obj_add_flag(menuClock, LV_OBJ_FLAG_HIDDEN);   // no status bar: more room between rows

  // strip with one 480x320 page per screenful of apps (4 x 3 grid each)
  menuStrip = lv_obj_create(menu);
  lv_obj_set_size(menuStrip, SCR_W * MENU_PAGES, SCR_H);
  lv_obj_set_pos(menuStrip, 0, 0);
  lv_obj_set_style_bg_opa(menuStrip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(menuStrip, 0, 0);
  lv_obj_set_style_pad_all(menuStrip, 0, 0);
  lv_obj_set_style_radius(menuStrip, 0, 0);
  lv_obj_remove_flag(menuStrip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(menuStrip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *grid[MENU_PAGES];
  for (int p = 0; p < MENU_PAGES; ++p) {
    lv_obj_t *pg = menuPage[p] = lv_obj_create(menuStrip);
    lv_obj_set_size(pg, SCR_W, SCR_H);
    lv_obj_set_pos(pg, p * SCR_W, 0);
    lv_obj_set_style_bg_color(pg, lv_color_hex(0x06163C), 0);      // same gradient as the menu (opaque snapshot)
    lv_obj_set_style_bg_grad_color(pg, lv_color_hex(0x073B86), 0);
    lv_obj_set_style_bg_grad_dir(pg, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_set_style_radius(pg, 0, 0);
    lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *g = grid[p] = lv_obj_create(pg);
    lv_obj_set_size(g, 4 * TILE_W + 3 * 6, 3 * TILE_H + 2 * 11);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_pad_row(g, 11, 0);
    lv_obj_set_style_pad_column(g, 6, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
  }
  // ---- page 1: the original 12 apps (unchanged order)
  menuBuildPage = 0;
  lv_obj_t *gridA = grid[0];
  menuTile(gridA, "$", "Finanças", 0x15171D, showFinanceEvent);
  menuTile(gridA, LV_SYMBOL_IMAGE, "Fotos", 0x39B982, showPhotosEvent);
  {
    lv_obj_t *srv = menuTile(gridA, LV_SYMBOL_DRIVE, "Servidores", 0x2E3A4C, showServersEvent);
    menuServersBadge = lv_obj_create(srv);   // red dot when a server is offline
    lv_obj_set_size(menuServersBadge, 16, 16); lv_obj_align(menuServersBadge, LV_ALIGN_TOP_RIGHT, -12, -4);
    lv_obj_set_style_radius(menuServersBadge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(menuServersBadge, lv_color_hex(0xF0453A), 0);
    lv_obj_set_style_border_color(menuServersBadge, lv_color_hex(0x06163C), 0);
    lv_obj_set_style_border_width(menuServersBadge, 2, 0);
    lv_obj_remove_flag(menuServersBadge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
  }
  menuTile(gridA, LV_SYMBOL_SETTINGS, "Ajustes", 0x8993A3, showSettingsEvent);
  menuTile(gridA, "", "Calendário", 0xF8F8FA, showCalendarEvent);
  menuTile(gridA, "", "Radar", 0x126E58, showFlightRadarEvent);
  menuTile(gridA, LV_SYMBOL_TINT, "Clima", 0x078BDD, showWeatherEvent);
  menuTile(gridA, LV_SYMBOL_HOME, "Relógio", 0x22252B, showClockEvent);
  menuTile(gridA, "", "Aeroporto", 0x0B1A2E, showAirportEvent);
  menuTile(gridA, LV_SYMBOL_EYE_OPEN, "C\xC3\xA9u", 0x1B2C5E, showSkyEvent);
  menuTile(gridA, "", "F1", 0x15171D, showF1Event);
  menuTile(gridA, "", "Arte", 0x07112A, showArtEvent);
  // ---- page 2: new apps
  menuBuildPage = 1;
  menuTile(grid[1], "", "Futebol", 0x167A36, showFootballEvent);
  menuBuildPage = 0;
  // page dots (live objects over the pictures; tap a dot to jump)
  for (int p = 0; p < MENU_PAGES; ++p) {
    lv_obj_t *d = menuDot[p] = lv_obj_create(menu);
    lv_obj_set_size(d, 7, 7);
    lv_obj_set_pos(d, SCR_W / 2 - (MENU_PAGES * 15) / 2 + p * 15 + 4, SCR_H - 9);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_set_ext_click_area(d, 10);
    lv_obj_add_event_cb(d, [](lv_event_t *e) { menuGoPage((int)(intptr_t)lv_event_get_user_data(e), true); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)p);
  }
  menuGoPage(0, false);
  menuWifiIcon = makeLabel(menu, LV_SYMBOL_WIFI, &lv_font_weather_14, 0xEAF2FF);
  lv_obj_align(menuWifiIcon, LV_ALIGN_TOP_RIGHT, -3, 3);
  menuSwipeTimer = lv_timer_create(menuSwipeCb, 15, NULL);
  lv_timer_pause(menuSwipeTimer);
}

const ScreenLifecycle kMenuScreen = {
  SCREEN_MENU, "menu", &mainMenu, buildMenuScreen, menuOnEnter, menuOnExit, renderMenu,
  nullptr, 0, 0, nullptr, 0, nullptr,
};
