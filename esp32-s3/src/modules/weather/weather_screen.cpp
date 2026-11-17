#include <esp_heap_caps.h>
#include <math.h>
#include "app_screens.h"
#include "core/screen_registry.h"
#include "modules/weather/weather.h"

static lv_obj_t *weatherView, *weatherTabs;
static lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
static lv_obj_t *lblRange, *lblHumidity, *lblWind, *weatherArt, *homeTab;
static lv_obj_t *fcDay[7], *fcCond[7], *fcTemp[7];
static lv_obj_t *hourTime[8], *hourCond[8], *hourTemp[8], *hourRain[8];
static lv_obj_t *hourArt[8], *dayArt[7];

#include <math.h>
#include <time.h>
// getLocalTime(&t, 0) fails at random: with a 0 ms timeout its loop is skipped whenever
// millis() ticks between two reads. Read the clock directly instead.

static int renderedCode = -999;
static int renderedHourCodes[8] = {-999,-999,-999,-999,-999,-999,-999,-999};
static int renderedDayCodes[7] = {-999,-999,-999,-999,-999,-999,-999};
static uint32_t renderedWeatherVersion = UINT32_MAX;
// Photo-like sky behind the tabs (same images as the Clock screen, clock.cpp).
const lv_image_dsc_t *skyImageFor(float hour, int code);
void applySkyVeil(lv_obj_t *veil);            // clock_screen.cpp
lv_obj_t *createSkyVeil(lv_obj_t *parent);
bool cachedWeatherCode(int &code);   // clock.cpp: last good forecast in flash
static lv_obj_t *wxSky, *wxVeil;
static int skyKey = -1;

// The sky photo can put the sun (or a bright cloud) right behind the clock: white digits
// on a white area. Measure the picture under the clock and switch the digits to a dark
// colour when that area is bright. Runs only when the picture changes (every 15 min).
static constexpr uint32_t CLOCK_LIGHT = 0xEFF4FA, CLOCK_DARK = 0x14243A;
static void adaptClockColor(const lv_image_dsc_t *img) {
  if (!img || !img->data || !lblClock) return;
  lv_obj_update_layout(lblClock);
  lv_area_t a; lv_obj_get_coords(lblClock, &a);
  // The clock lives in the "Agora" tab. If another tab is shown (or the tabs are sliding),
  // its screen coordinates are shifted sideways and the measurement fell outside the
  // picture (white digits kept until the next sky change, 15 min later). Measure where
  // the clock sits when "Agora" is the active tab instead.
  if (homeTab && lv_obj_get_parent(homeTab)) {
    lv_area_t tab, content;
    lv_obj_get_coords(homeTab, &tab);
    lv_obj_get_coords(lv_obj_get_parent(homeTab), &content);
    const int dx = tab.x1 - content.x1, dy = tab.y1 - content.y1;
    a.x1 -= dx; a.x2 -= dx; a.y1 -= dy; a.y2 -= dy;
  }
  const int w = img->header.w, h = img->header.h;
  const int stride = img->header.stride ? img->header.stride / 2 : w;
  const uint16_t *px = (const uint16_t *)img->data;
  const int x0 = a.x1 < 0 ? 0 : a.x1, x1 = a.x2 >= w ? w - 1 : a.x2;
  const int y0 = a.y1 < 0 ? 0 : a.y1, y1 = a.y2 >= h ? h - 1 : a.y2;
  uint32_t sum = 0, bright = 0, n = 0;
  for (int y = y0; y <= y1; y += 2)
    for (int x = x0; x <= x1; x += 2) {
      const uint16_t p = px[y * stride + x];
      const int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
      const int l = (r * 77 + g * 150 + b * 29) >> 8;   // luminance 0..255
      sum += l; bright += l > 190; ++n;
    }
  if (!n) return;
  const bool light = sum / n > 150 || bright * 100 / n > 30;   // sun or fog behind the clock (overcast stays white)
  lv_obj_set_style_text_color(lblClock, lv_color_hex(light ? CLOCK_DARK : CLOCK_LIGHT), 0);
}

static void refreshSky(int code, bool force) {
  struct tm ti;
  if (!localNow(&ti)) return;
  const int key = code * 1000 + ti.tm_hour * 12 + ti.tm_min / 5;     // re-pick every 5 min (twilight veil) or on new code
  if (!force && key == skyKey) return;
  skyKey = key;
  const lv_image_dsc_t *img = skyImageFor(ti.tm_hour + ti.tm_min / 60.0f, code);
  if (!img) return;
  lv_image_set_src(wxSky, img);
  lv_obj_invalidate(wxSky);
  lv_obj_set_hidden(wxSky, false);       // shown only with the right picture
  applySkyVeil(wxVeil);
  adaptClockColor(img);
}
static int renderedWifiState = -1;

// On-Enter: force one full repaint from the cached forecast.
static void weatherOnEnter() {
  renderedWeatherVersion = UINT32_MAX;
  skyKey = -1;            // Clock may have reused the buffer: reload
  lv_obj_set_hidden(wxSky, true);   // never show a stale picture
  if (wxVeil) lv_obj_set_hidden(wxVeil, true);
  if (lblClock) lv_obj_set_style_text_color(lblClock, lv_color_hex(CLOCK_LIGHT), 0);
  renderedWifiState = -1;
  if (weatherTabs) lv_tabview_set_active(weatherTabs, 0, LV_ANIM_OFF);   // always start on "Agora"
}

// Rain under the cloud, by intensity. Upright rounded capsules with a soft vertical
// gradient: LVGL anti-aliases them (the old rotated rectangles were jagged) and they
// cost no transform layer. Coordinates are on the 150 px design canvas.
static void addRainDrops(lv_obj_t *parent, int code, float s) {
  const int level = rainLevel(code);
  if (!level) return;
  struct Drop { int16_t x, y, w, h; };
  static const Drop kGaroa[] = {{40, 112, 4, 8}, {62, 120, 4, 8}, {84, 112, 4, 8}, {106, 120, 4, 8},
                                {51, 131, 4, 7}, {95, 131, 4, 7}};
  static const Drop kChuva[] = {{44, 108, 6, 20}, {74, 114, 6, 22}, {104, 108, 6, 20}};
  static const Drop kForte[] = {{34, 106, 6, 26}, {56, 113, 6, 28}, {78, 106, 6, 26}, {100, 113, 6, 28},
                                {122, 106, 6, 24}};
  const Drop *d = level == 1 ? kGaroa : level == 2 ? kChuva : kForte;
  const int n = level == 1 ? 6 : level == 2 ? 3 : 5;
  const uint32_t top = level == 1 ? 0xC4E4FF : level == 2 ? 0x9AD6FF : 0x7CC4FF;
  const uint32_t bottom = level == 1 ? 0x8CC6F2 : level == 2 ? 0x55AAF0 : 0x2F86E2;
  auto S = [s](int v) { const int r = (int)lroundf(v * s); return r < 1 ? 1 : r; };
  for (int i = 0; i < n; ++i) {
    lv_obj_t *o = lv_obj_create(parent);
    const int w = S(d[i].w) < 2 ? 2 : S(d[i].w), h = S(d[i].h) < 3 ? 3 : S(d[i].h);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, S(d[i].x), S(d[i].y));
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, level == 1 ? LV_OPA_80 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
  }
}

// Crescent moon drawn into a small transparent canvas (ARGB8888): the lit disc minus an
// offset disc, anti-aliased. Real transparency, so it works over any sky photo or veil
// (the old version covered the moon with a second, dark disc).
static void freeCanvasBuf(lv_event_t *e) { heap_caps_free(lv_event_get_user_data(e)); }
static lv_obj_t *makeCrescent(lv_obj_t *parent, int x, int y, int d, uint32_t rgb) {
  if (d < 4) d = 4;
  const int W = d + 2;
  uint32_t *buf = (uint32_t *)heap_caps_malloc(W * W * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_obj_set_pos(cv, x - 1, y - 1);
  lv_obj_set_clickable(cv, false);
  if (!buf) return cv;
  const float r = d / 2.0f, cx = W / 2.0f, cy = W / 2.0f;
  const float rb = r * 0.86f, bx = cx + r * 0.62f, by = cy - r * 0.42f;   // the shadowed part
  for (int j = 0; j < W; ++j)
    for (int i = 0; i < W; ++i) {
      const float px = i + 0.5f, py = j + 0.5f;
      float a = r + 0.5f - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
      float b = sqrtf((px - bx) * (px - bx) + (py - by) * (py - by)) - rb + 0.5f;
      a = a < 0 ? 0 : a > 1 ? 1 : a; b = b < 0 ? 0 : b > 1 ? 1 : b;
      buf[j * W + i] = ((uint32_t)(a * b * 255) << 24) | (rgb & 0xFFFFFF);
    }
  lv_canvas_set_buffer(cv, buf, W, W, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_add_event_cb(cv, freeCanvasBuf, LV_EVENT_DELETE, buf);
  return cv;
}

static lv_obj_t *makeWeatherIcon(lv_obj_t *parent, int x, int y, int size, int code, bool night = false) {
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_set_size(icon, size, size);
  lv_obj_set_pos(icon, x, y);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_set_scrollable(icon, false);
  lv_obj_set_clickable(icon, false);
  // Scale the exact shapes and relative positions used by the large Agora
  // illustration (designed on a 150 px canvas) so all forecast views match it.
  const float s = size / 150.0f;
  auto scaled = [s](int value) { return (int)(value * s + 0.5f); };
  // Moon / sun behind the cloud. Overcast at night still shows a small moon peeking
  // out, so "nublado à noite" does not look like a daytime cloud.
  const bool moonPeek = night && code == 3;
  if (code <= 2 || moonPeek) {
    // Day: sun. Night: pale moon (crescent cut by a disc in the night-sky colour).
    const int d = moonPeek ? 40 : night ? 50 : 62, px = moonPeek ? 84 : night ? 74 : 68, py = moonPeek ? 14 : night ? 8 : 5;
    if (night) {
      lv_obj_t *moon = makeCrescent(icon, scaled(px), scaled(py), scaled(d), moonPeek ? 0xCFD8E6 : 0xE9EEF7);
      lv_obj_move_to_index(moon, 0);
    } else {
      lv_obj_t *sun = lv_obj_create(icon);
      lv_obj_set_size(sun, scaled(d), scaled(d));
      lv_obj_set_pos(sun, scaled(px), scaled(py));
      lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFC96B), 0);
      lv_obj_set_style_border_width(sun, 0, 0);
      lv_obj_move_to_index(sun, 0);
    }
  }
  // Night clouds: cooler, dimmer greys (the bright day cloud glowed on the dark sky).
  const uint32_t baseCol = night ? 0x8E9DB0 : 0xBBD2EA, puffCol = night ? 0xAEBBCB : 0xD8E7F5;
  lv_obj_t *base = lv_obj_create(icon);
  lv_obj_set_size(base, scaled(106), scaled(42));
  lv_obj_set_pos(base, scaled(22), scaled(61));
  lv_obj_set_style_radius(base, scaled(22), 0);
  lv_obj_set_style_bg_color(base, lv_color_hex(baseCol), 0);
  lv_obj_set_style_border_width(base, 0, 0);
  const int puffX[] = {34, 68};
  const int puffY[] = {38, 49};
  const int puffSize[] = {54, 44};
  for (int j = 0; j < 2; j++) {
    lv_obj_t *puff = lv_obj_create(icon);
    lv_obj_set_size(puff, scaled(puffSize[j]), scaled(puffSize[j]));
    lv_obj_set_pos(puff, scaled(puffX[j]), scaled(puffY[j]));
    lv_obj_set_style_radius(puff, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puff, lv_color_hex(puffCol), 0);
    lv_obj_set_style_border_width(puff, 0, 0);
  }
  addRainDrops(icon, code, s);
  return icon;
}


// Agora: city/date top-left, clock + Wi-Fi top-right, big temperature and the
// big weather artwork in the middle (unchanged emphasis), and four detail
// cards (chuva, umidade, vento, UV) above the tab bar.
extern "C" const lv_image_dsc_t weather_icon_drop;
static lv_obj_t *lblRainNow, *lblUv;

static lv_obj_t *detailCard(lv_obj_t *t, int index, const char *title, lv_obj_t **value, bool drop) {
  // The tab already has ~16 px side padding: 4 x 104 + 3 x 10 = 446 px.
  lv_obj_t *card = makeCard(t, 104, 56);
  lv_obj_set_pos(card, index * 114, 196);
  lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_t *head = lv_obj_create(card);
  lv_obj_set_size(head, 92, 18);
  lv_obj_set_pos(head, 10, 6);
  lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(head, 0, 0);
  lv_obj_set_style_pad_all(head, 0, 0);
  lv_obj_set_style_pad_column(head, 4, 0);
  lv_obj_set_scrollable(head, false);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (drop) {
    lv_obj_t *icon = lv_image_create(head);
    lv_image_set_src(icon, &weather_icon_drop);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(0x7FB6E8), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
  }
  makeLabel(head, title, &lv_font_weather_14, 0xE3EAF3);     // brighter: readable over the photo
  *value = makeLabel(card, "--", &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(*value, 10, 26);
  return card;
}

static void build_home(lv_obj_t *t) {
  homeTab = t;
  lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);                 // sky shown as is (same as Clock)
  lv_obj_set_scrollable(t, false);

  lblCity = makeLabel(t, "São Paulo", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_width(lblCity, 250);
  lv_label_set_long_mode(lblCity, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lblCity, 52, 8);
  lblDate = makeLabel(t, "", &lv_font_weather_14, 0xF2F6FB);
  lv_obj_set_pos(lblDate, 52, 34);
  // Wi-Fi icon: same size and screen position as on the main menu
  // (font 14, 15 px from the right edge, 7 px from the top). The tab has its
  // own padding, so compensate for it to land on the same pixels.
  lblWifiDot = makeLabel(t, LV_SYMBOL_WIFI, &lv_font_weather_14, 0xD05050);
  lv_obj_align(lblWifiDot, LV_ALIGN_TOP_RIGHT,
               -15 + lv_obj_get_style_pad_right(t, LV_PART_MAIN),
               7 - lv_obj_get_style_pad_top(t, LV_PART_MAIN));
  lblClock = makeLabel(t, "--:--", &lv_font_montserrat_42, 0xEFF4FA);   // ~15 % bigger than 36
  lv_obj_align(lblClock, LV_ALIGN_TOP_RIGHT, -31, 10);   // a bit away from the Wi-Fi icon (top-right corner)

  // Big temperature + condition: same font/size and emphasis as before.
  lblTemp = makeLabel(t, "--°", &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_set_pos(lblTemp, 20, 60);
  lblCond = makeLabel(t, T("Aguardando clima", "Waiting for weather"), &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(lblCond, 24, 118);
  lblRange = makeLabel(t, "", &lv_font_weather_14, 0xF2F6FB);
  lv_obj_set_pos(lblRange, 24, 148);

  // Big weather artwork (same drawing, drawn in updateWeatherScreen).
  weatherArt = lv_obj_create(t);
  lv_obj_set_size(weatherArt, 136, 136);
  lv_obj_align(weatherArt, LV_ALIGN_TOP_RIGHT, -20, 48);
  lv_obj_set_style_bg_opa(weatherArt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weatherArt, 0, 0);
  lv_obj_set_style_pad_all(weatherArt, 0, 0);
  lv_obj_set_scrollable(weatherArt, false);
  lv_obj_set_clickable(weatherArt, false);

  detailCard(t, 0, T("Chuva", "Rain"), &lblRainNow, true);
  detailCard(t, 1, T("Umidade", "Humidity"), &lblHumidity, false);
  detailCard(t, 2, T("Vento", "Wind"), &lblWind, false);
  detailCard(t, 3, T("Índice UV", "UV index"), &lblUv, false);

  // Only shown when something needs attention (see renderWeather in main.cpp).
  lblStatus = makeLabel(t, "", &lv_font_weather_14, 0xFFD477);
  lv_obj_set_width(lblStatus, 200);
  lv_label_set_long_mode(lblStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lblStatus, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(lblStatus, LV_ALIGN_TOP_RIGHT, -16, 42);
}

// ---- Forecast tabs (Horas / Dias): same horizontally scrolling cards as
// before, refined: aligned cards, first card highlighted, rain shown only when
// relevant, discreet scrollbar and a one-line summary below the cards.
static lv_obj_t *fcMin[7], *fcRain[7], *fcBarFill[7], *fcCard[7], *hourCard[8];
static constexpr int FC_BAR_W = 68;

static void styleForecastStrip(lv_obj_t *strip) {
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_hor(strip, 4, 0);
  lv_obj_set_style_pad_top(strip, 4, 0);
  lv_obj_set_style_pad_bottom(strip, 10, 0);          // room for the scrollbar under the cards
  lv_obj_set_style_pad_column(strip, 8, 0);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  // Thin, translucent scrollbar instead of the default thick grey bar.
  lv_obj_set_style_width(strip, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_height(strip, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(strip, lv_color_hex(0xFFFFFF), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(strip, LV_OPA_30, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(strip, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_bottom(strip, 2, LV_PART_SCROLLBAR);
}

static lv_obj_t *makeForecastCard(lv_obj_t *strip, int h, bool highlight) {
  lv_obj_t *card = makeCard(strip, 92, h);
  lv_obj_set_style_bg_opa(card, highlight ? LV_OPA_COVER : LV_OPA_80, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(highlight ? 0x2C4566 : 0x243247), 0);
  lv_obj_set_style_border_width(card, highlight ? 1 : 0, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x5AA9FF), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
  lv_obj_set_clickable(card, false);
  return card;
}

static lv_obj_t *centeredLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y) {
  lv_obj_t *l = makeLabel(parent, "", font, color);
  lv_obj_set_width(l, 76);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

// Rain chance row: drop icon + "30%", centred, in grey.
extern "C" const lv_image_dsc_t weather_icon_drop;
static constexpr uint32_t RAIN_COLOR = 0x9AA8BA;
static lv_obj_t *rainRow(lv_obj_t *card, int y, lv_obj_t **label) {
  lv_obj_t *row = lv_obj_create(card);
  lv_obj_set_size(row, 76, 18);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 4, 0);
  lv_obj_set_scrollable(row, false);
  lv_obj_set_clickable(row, false);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *drop = lv_image_create(row);
  lv_image_set_src(drop, &weather_icon_drop);
  lv_obj_set_style_image_recolor(drop, lv_color_hex(0x7FB6E8), 0);
  lv_obj_set_style_image_recolor_opa(drop, LV_OPA_COVER, 0);
  *label = makeLabel(row, "", &lv_font_weather_14, RAIN_COLOR);
  return row;
}

static void build_hours(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x050A14), 0);      // very light veil: lists stay readable
  lv_obj_set_style_bg_opa(t, LV_OPA_20, 0);
  lv_obj_set_scrollable(t, false);
  lv_obj_t *title = makeLabel(t, T("Previsão por hora", "Hourly forecast"), &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);
  lv_obj_t *every = makeLabel(t, T("a cada 3 h", "every 3 h"), &lv_font_weather_14, 0x8FA6C2);
  lv_obj_align(every, LV_ALIGN_TOP_RIGHT, -16, 13);

  lv_obj_t *strip = lv_obj_create(t);
  lv_obj_set_size(strip, 452, 226);
  lv_obj_set_pos(strip, 14, 40);
  styleForecastStrip(strip);
  for (int i = 0; i < 8; i++) {
    lv_obj_t *card = hourCard[i] = makeForecastCard(strip, 196, i == 0);
    hourTime[i] = centeredLabel(card, &lv_font_weather_14, i == 0 ? 0xFFFFFF : 0xAFC0D6, 6);
    hourArt[i] = NULL;                                   // icon drawn at (14, 40), 48 px
    hourCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xDCE8F7);
    lv_obj_set_hidden(hourCond[i], true);
    hourTemp[i] = centeredLabel(card, &lv_font_weather_20, 0xFFFFFF, 110);
    rainRow(card, 148, &hourRain[i]);
  }
}

static void build_days(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x050A14), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_20, 0);
  lv_obj_set_scrollable(t, false);
  lv_obj_t *title = makeLabel(t, T("Próximos 7 dias", "Next 7 days"), &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);
  lv_obj_t *strip = lv_obj_create(t);
  lv_obj_set_size(strip, 452, 226);
  lv_obj_set_pos(strip, 14, 40);
  styleForecastStrip(strip);
  for (int i = 0; i < 7; i++) {
    lv_obj_t *card = fcCard[i] = makeForecastCard(strip, 196, i == 0);
    fcDay[i] = centeredLabel(card, &lv_font_weather_14, i == 0 ? 0xFFFFFF : 0xAFC0D6, 6);
    dayArt[i] = NULL;                                    // icon drawn at (14, 36), 48 px
    fcCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xEAF2FF);
    lv_obj_set_hidden(fcCond[i], true);
    fcTemp[i] = centeredLabel(card, &lv_font_weather_20, 0xFFFFFF, 98);      // máxima
    fcMin[i] = centeredLabel(card, &lv_font_weather_14, 0x8FA6C2, 124);      // mínima
    // Week-scaled range bar: where this day's min..max sits in the week.
    lv_obj_t *track = lv_obj_create(card);
    lv_obj_set_size(track, FC_BAR_W, 6);
    lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x0E1828), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_scrollable(track, false);
    lv_obj_set_clickable(track, false);
    fcBarFill[i] = lv_obj_create(track);
    lv_obj_set_size(fcBarFill[i], FC_BAR_W, 6);
    lv_obj_set_pos(fcBarFill[i], 0, 0);
    lv_obj_set_style_radius(fcBarFill[i], 3, 0);
    lv_obj_set_style_border_width(fcBarFill[i], 0, 0);
    lv_obj_set_style_bg_opa(fcBarFill[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(fcBarFill[i], LV_GRAD_DIR_HOR, 0);
    lv_obj_set_clickable(fcBarFill[i], false);
    rainRow(card, 162, &fcRain[i]);
  }
}

// Cold -> hot colour used by the range bars.
static lv_color_t tempColor(float c) {
  if (c < 12) return lv_color_hex(0x5AA9FF);
  if (c < 18) return lv_color_hex(0x4FD1C5);
  if (c < 24) return lv_color_hex(0xB5E36B);
  if (c < 29) return lv_color_hex(0xFFB547);
  return lv_color_hex(0xFF6B4A);
}


static void buildWeatherScreen(lv_obj_t *scr) {
  weatherView = lv_obj_create(scr);
  lv_obj_set_size(weatherView, SCR_W, SCR_H);
  lv_obj_set_pos(weatherView, 0, 0);
  lv_obj_set_style_pad_all(weatherView, 0, 0);
  lv_obj_set_style_border_width(weatherView, 0, 0);
  lv_obj_set_style_bg_color(weatherView, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(weatherView, LV_OPA_COVER, 0);   // black until the sky is ready
  wxSky = lv_image_create(weatherView);
  lv_obj_set_pos(wxSky, 0, 0);
  lv_obj_set_size(wxSky, SCR_W, SCR_H);
  lv_obj_set_clickable(wxSky, false);
  lv_obj_set_hidden(wxSky, true);
  wxVeil = createSkyVeil(weatherView);    // twilight tint over the grey day photos
  weatherTabs = lv_tabview_create(weatherView);
  lv_obj_set_size(weatherTabs, SCR_W, SCR_H);
  lv_obj_set_pos(weatherTabs, 0, 0);
  lv_tabview_set_tab_bar_position(weatherTabs, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(weatherTabs, 42);
  lv_obj_set_style_bg_opa(weatherTabs, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(lv_tabview_get_content(weatherTabs), LV_OPA_TRANSP, 0);
  // Instant tab switch: sliding three full-screen pages over the photo redraws the
  // whole screen every frame.
  lv_obj_set_style_anim_duration(lv_tabview_get_content(weatherTabs), 0, 0);
  lv_obj_t *tabBar = lv_tabview_get_tab_bar(weatherTabs);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x0B1424), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(tabBar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x203C5C), LV_PART_ITEMS);
  lv_obj_set_style_text_color(tabBar, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(tabBar, 2, LV_PART_ITEMS);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x8AB9DD), LV_PART_ITEMS);
  lv_obj_set_style_text_opa(tabBar, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x173D65), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(tabBar, lv_color_hex(0xFFFFFF), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x20A7F2), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tabBar, 2, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_scroll_chain_hor(lv_tabview_get_content(weatherTabs), false);
  lv_obj_t *home = lv_tabview_add_tab(weatherTabs, T("Agora", "Now"));
  lv_obj_t *hours = lv_tabview_add_tab(weatherTabs, T("Horas", "Hours"));
  lv_obj_t *days = lv_tabview_add_tab(weatherTabs, T("Dias", "Days"));
  // Tab buttons are created by add_tab(), so apply their visible idle style afterwards.
  lv_obj_t *tabs = lv_tabview_get_tab_bar(weatherTabs);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *tabButton = lv_obj_get_child(tabs, i);
    if (!tabButton) continue;
    lv_obj_set_style_bg_color(tabButton, lv_color_hex(0x17263B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabButton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(tabButton, lv_color_hex(0xD7E2EF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(tabButton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tabButton, lv_color_hex(0x334A64), LV_PART_MAIN);
    lv_obj_set_style_border_width(tabButton, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tabButton, lv_color_hex(0x087FC0), (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabButton, lv_color_hex(0xFFFFFF), (lv_style_selector_t)LV_PART_MAIN | LV_STATE_CHECKED);
  }
  build_home(home);
  build_hours(hours);
  build_days(days);
  // One back button on the view itself, above the tabview: buttons inside the
  // scrollable tab pages lost their clicks to the tab content/swipe handling.
  lv_obj_move_foreground(addBackButton(weatherView));   // standard back button (main.cpp)

}

// UI thread, active Weather screen only. Widgets are rewritten only when the
// forecast changed (version) or the Wi-Fi state changed.
static void updateWeatherScreen() {
  String city;
  bool up, have;
  uint32_t version;
  float temp, wind, feels, uv, mx[7], mn[7], htemp[8];
  int code, humidity, hourCount, hcode[8], hrain[8], dc[7], drain[7];
  char hours[8][6], dn[7][6];
  xSemaphoreTake(mtx, portMAX_DELAY);
  up = sh.wifiUp;
  xSemaphoreGive(mtx);
  xSemaphoreTake(wxMtx, portMAX_DELAY);
  city = wx.cityName;
  have = wx.haveWeather;
  temp = wx.temp;
  code = wx.code;
  humidity = wx.humidity;
  wind = wx.wind;
  feels = wx.feels;
  uv = wx.uv;
  hourCount = wx.hourCount;
  memcpy(hours, wx.hourLabel, sizeof hours);
  memcpy(htemp, wx.hourTemp, sizeof htemp);
  memcpy(hcode, wx.hourCode, sizeof hcode);
  const bool isDay = wx.isDay; bool hday[8]; memcpy(hday, wx.hourIsDay, sizeof hday);
  memcpy(hrain, wx.hourRain, sizeof hrain);
  memcpy(dn, wx.dayName, sizeof dn);
  memcpy(mx, wx.tmax, sizeof mx);
  memcpy(mn, wx.tmin, sizeof mn);
  memcpy(dc, wx.dcode, sizeof dc);
  memcpy(drain, wx.dayRain, sizeof drain);
  version = wx.weatherVersion;
  xSemaphoreGive(wxMtx);
  // sky only with a known condition: live, else the cached one; nothing -> black
  { int c = code; if (have || cachedWeatherCode(c)) refreshSky(c, false); }
  if (version == renderedWeatherVersion && renderedWifiState == (int)up) return;
  renderedWeatherVersion = version;
  renderedWifiState = up;
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code, !isDay));
    lv_label_set_text_fmt(lblRange, T(LV_SYMBOL_UP " %.0f°   " LV_SYMBOL_DOWN " %.0f°   ·   sensação %.0f°",
                                      LV_SYMBOL_UP " %.0f°   " LV_SYMBOL_DOWN " %.0f°   ·   feels %.0f°"), mx[0], mn[0], feels);
    lv_label_set_text_fmt(lblHumidity, "%d%%", humidity);
    lv_label_set_text_fmt(lblWind, "%.0f km/h", wind);
    lv_label_set_text_fmt(lblRainNow, "%d%%", hourCount ? hrain[0] : 0);
    if (uv >= 0) lv_label_set_text_fmt(lblUv, "%.0f", uv);
    else lv_label_set_text(lblUv, "--");
    float weekLo = mn[0], weekHi = mx[0];
    for (int i = 1; i < 7; i++) { weekLo = min(weekLo, mn[i]); weekHi = max(weekHi, mx[i]); }
    if (weekHi - weekLo < 1) weekHi = weekLo + 1;
    for (int i = 0; i < 7; i++) {
      lv_label_set_text(fcDay[i], i == 0 ? T("Hoje", "Today") : dn[i]);
      lv_label_set_text(fcCond[i], weatherText(dc[i]));
      lv_label_set_text_fmt(fcTemp[i], "%.0f°", mx[i]);
      lv_label_set_text_fmt(fcMin[i], T("mín %.0f°", "min %.0f°"), mn[i]);
      const int x0 = (int)((mn[i] - weekLo) * FC_BAR_W / (weekHi - weekLo));
      const int x1 = (int)((mx[i] - weekLo) * FC_BAR_W / (weekHi - weekLo));
      lv_obj_set_pos(fcBarFill[i], x0, 0);
      lv_obj_set_width(fcBarFill[i], max(6, x1 - x0));
      lv_obj_set_style_bg_color(fcBarFill[i], tempColor(mn[i]), 0);
      lv_obj_set_style_bg_grad_color(fcBarFill[i], tempColor(mx[i]), 0);
      lv_label_set_text_fmt(fcRain[i], "%d%%", drain[i]);
      if (renderedDayCodes[i] != dc[i]) {
        if (dayArt[i]) lv_obj_del(dayArt[i]);
        dayArt[i] = makeWeatherIcon(fcCard[i], 14, 36, 48, dc[i]);
        renderedDayCodes[i] = dc[i];
      }
    }
    for (int i = 0; i < 8; i++) {
      if (i < hourCount) {
        if (i == 0) lv_label_set_text(hourTime[i], T("Agora", "Now"));
        else lv_label_set_text_fmt(hourTime[i], "%sh", hours[i]);
        lv_label_set_text(hourCond[i], weatherText(hcode[i], !hday[i]));
        lv_label_set_text_fmt(hourTemp[i], "%.0f°", htemp[i]);
        lv_label_set_text_fmt(hourRain[i], "%d%%", hrain[i]);
        const int hkey = hcode[i] * 2 + !hday[i];               // icon changes with day/night too
        if (renderedHourCodes[i] != hkey) {
          if (hourArt[i]) lv_obj_del(hourArt[i]);
          hourArt[i] = makeWeatherIcon(hourCard[i], 14, 40, 48, hcode[i], !hday[i]);
          renderedHourCodes[i] = hkey;
        }
        lv_obj_set_hidden(hourCard[i], false);
      } else {
        lv_obj_set_hidden(hourCard[i], true);
      }
    }
    if (renderedCode != code * 2 + !isDay) {
      renderedCode = code * 2 + !isDay;
      // Change the scene colours to suit the current condition.
      refreshSky(code, true);      // the sky photo now carries the condition
      lv_obj_clean(weatherArt);
      // Same artwork as the forecast cards, at full size (150 px design canvas).
      makeWeatherIcon(weatherArt, 0, 0, 150, code, !isDay);
    }
  } else {
    lv_label_set_text(lblTemp, "--");
    lv_label_set_text(lblCond, up ? T("Carregando previsão...", "Loading forecast...") : T("Configure o Wi-Fi em Ajustes", "Set up Wi-Fi in Settings"));
  }

}

static void renderWeather() {
  struct tm ti;
  char b[24];
  if (localNow(&ti)) {
    strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M %p", &ti);
    setLabelIfChanged(lblClock, b);
    snprintf(b, sizeof b, "%s, %02d %s", weekdayShort(ti.tm_wday), ti.tm_mday, monthShort(ti.tm_mon));
    setLabelIfChanged(lblDate, b);
  } else {
    setLabelIfChanged(lblClock, "--:--");
    setLabelIfChanged(lblDate, T("Aguardando hora...", "Waiting for the time..."));
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  String status = sh.status;
  bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  // Agora tab: only show the status when something needs attention (errors,
  // searching: they start with T("Falha","Failed") / T("Buscando","Searching")); others stay hidden.
  const bool showStatus = status.startsWith(T("Falha", "Failed")) || status.startsWith(T("Buscando", "Searching")) || !up;
  setLabelIfChanged(lblStatus, showStatus ? status.c_str() : "");
  static int renderedDot = -1;
  if (renderedDot != (int)up) { renderWifiIndicator(lblWifiDot, up); renderedDot = up; }
  updateWeatherScreen();
}

static const NetJobDef kWeatherJobs[] = {{"weather", weatherFetch, true, true}};
const ScreenLifecycle kWeatherScreen = {
  SCREEN_WEATHER, "weather", &weatherView, buildWeatherScreen, weatherOnEnter, nullptr, renderWeather,
  nullptr, WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS, kWeatherJobs, 1, nullptr,
};
