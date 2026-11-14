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
bool cachedWeatherCode(int &code);   // clock.cpp: last good forecast in flash
static lv_obj_t *wxSky;
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
  const int key = code * 100 + ti.tm_hour * 4 + ti.tm_min / 15;     // re-pick every 15 min or on new code
  if (!force && key == skyKey) return;
  skyKey = key;
  const lv_image_dsc_t *img = skyImageFor(ti.tm_hour + ti.tm_min / 60.0f, code);
  if (!img) return;
  lv_image_set_src(wxSky, img);
  lv_obj_invalidate(wxSky);
  lv_obj_remove_flag(wxSky, LV_OBJ_FLAG_HIDDEN);       // shown only with the right picture
  adaptClockColor(img);
}
static int renderedWifiState = -1;

// On-Enter: force one full repaint from the cached forecast.
static void weatherOnEnter() {
  renderedWeatherVersion = UINT32_MAX;
  skyKey = -1;            // Clock may have reused the buffer: reload
  lv_obj_add_flag(wxSky, LV_OBJ_FLAG_HIDDEN);   // never show a stale picture
  if (lblClock) lv_obj_set_style_text_color(lblClock, lv_color_hex(CLOCK_LIGHT), 0);
  renderedWifiState = -1;
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
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  }
}

static lv_obj_t *makeWeatherIcon(lv_obj_t *parent, int x, int y, int size, int code, bool night = false) {
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_set_size(icon, size, size);
  lv_obj_set_pos(icon, x, y);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  // Scale the exact shapes and relative positions used by the large Agora
  // illustration (designed on a 150 px canvas) so all forecast views match it.
  const float s = size / 150.0f;
  auto scaled = [s](int value) { return (int)(value * s + 0.5f); };
  if (code <= 2) {
    // Day: sun. Night: pale moon (crescent cut by a disc in the night-sky colour).
    lv_obj_t *sun = lv_obj_create(icon);
    lv_obj_set_size(sun, scaled(night ? 50 : 62), scaled(night ? 50 : 62));
    lv_obj_set_pos(sun, scaled(night ? 74 : 68), scaled(night ? 8 : 5));
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun, lv_color_hex(night ? 0xE9EEF7 : 0xFFC96B), 0);
    lv_obj_set_style_border_width(sun, 0, 0);
    lv_obj_move_to_index(sun, 0);
    if (night) {
      lv_obj_t *cut = lv_obj_create(icon);
      lv_obj_set_size(cut, scaled(42), scaled(42));
      lv_obj_set_pos(cut, scaled(92), scaled(0));
      lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(cut, lv_color_hex(0x1B2740), 0);
      lv_obj_set_style_border_width(cut, 0, 0);
      lv_obj_move_to_index(cut, 1);
    }
  }
  lv_obj_t *base = lv_obj_create(icon);
  lv_obj_set_size(base, scaled(106), scaled(42));
  lv_obj_set_pos(base, scaled(22), scaled(61));
  lv_obj_set_style_radius(base, scaled(22), 0);
  lv_obj_set_style_bg_color(base, lv_color_hex(0xBBD2EA), 0);
  lv_obj_set_style_border_width(base, 0, 0);
  const int puffX[] = {34, 68};
  const int puffY[] = {38, 49};
  const int puffSize[] = {54, 44};
  for (int j = 0; j < 2; j++) {
    lv_obj_t *puff = lv_obj_create(icon);
    lv_obj_set_size(puff, scaled(puffSize[j]), scaled(puffSize[j]));
    lv_obj_set_pos(puff, scaled(puffX[j]), scaled(puffY[j]));
    lv_obj_set_style_radius(puff, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puff, lv_color_hex(0xD8E7F5), 0);
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
  lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
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
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

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
  lblCond = makeLabel(t, "Aguardando clima", &lv_font_weather_20, 0xEAF2FF);
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
  lv_obj_remove_flag(weatherArt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(weatherArt, LV_OBJ_FLAG_CLICKABLE);

  detailCard(t, 0, "Chuva", &lblRainNow, true);
  detailCard(t, 1, "Umidade", &lblHumidity, false);
  detailCard(t, 2, "Vento", &lblWind, false);
  detailCard(t, 3, "Índice UV", &lblUv, false);

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
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
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
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
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
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = makeLabel(t, "Previsão por hora", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);
  lv_obj_t *every = makeLabel(t, "a cada 3 h", &lv_font_weather_14, 0x8FA6C2);
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
    lv_obj_add_flag(hourCond[i], LV_OBJ_FLAG_HIDDEN);
    hourTemp[i] = centeredLabel(card, &lv_font_weather_20, 0xFFFFFF, 110);
    rainRow(card, 148, &hourRain[i]);
  }
}

static void build_days(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x050A14), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_20, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = makeLabel(t, "Próximos 7 dias", &lv_font_weather_20, 0xEAF2FF);
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
    lv_obj_add_flag(fcCond[i], LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    fcBarFill[i] = lv_obj_create(track);
    lv_obj_set_size(fcBarFill[i], FC_BAR_W, 6);
    lv_obj_set_pos(fcBarFill[i], 0, 0);
    lv_obj_set_style_radius(fcBarFill[i], 3, 0);
    lv_obj_set_style_border_width(fcBarFill[i], 0, 0);
    lv_obj_set_style_bg_opa(fcBarFill[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(fcBarFill[i], LV_GRAD_DIR_HOR, 0);
    lv_obj_remove_flag(fcBarFill[i], LV_OBJ_FLAG_CLICKABLE);
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
  lv_obj_remove_flag(wxSky, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(wxSky, LV_OBJ_FLAG_HIDDEN);
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
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x173D65), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(tabBar, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x20A7F2), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tabBar, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_remove_flag(lv_tabview_get_content(weatherTabs), LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_t *home = lv_tabview_add_tab(weatherTabs, "Agora");
  lv_obj_t *hours = lv_tabview_add_tab(weatherTabs, "Horas");
  lv_obj_t *days = lv_tabview_add_tab(weatherTabs, "Dias");
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
    lv_obj_set_style_bg_color(tabButton, lv_color_hex(0x087FC0), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabButton, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_CHECKED);
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
  city = sh.cityName;
  up = sh.wifiUp;
  have = sh.haveWeather;
  temp = sh.temp;
  code = sh.code;
  humidity = sh.humidity;
  wind = sh.wind;
  feels = sh.feels;
  uv = sh.uv;
  hourCount = sh.hourCount;
  memcpy(hours, sh.hourLabel, sizeof hours);
  memcpy(htemp, sh.hourTemp, sizeof htemp);
  memcpy(hcode, sh.hourCode, sizeof hcode);
  const bool isDay = sh.isDay; bool hday[8]; memcpy(hday, sh.hourIsDay, sizeof hday);
  memcpy(hrain, sh.hourRain, sizeof hrain);
  memcpy(dn, sh.dayName, sizeof dn);
  memcpy(mx, sh.tmax, sizeof mx);
  memcpy(mn, sh.tmin, sizeof mn);
  memcpy(dc, sh.dcode, sizeof dc);
  memcpy(drain, sh.dayRain, sizeof drain);
  version = sh.weatherVersion;
  xSemaphoreGive(mtx);
  // sky only with a known condition: live, else the cached one; nothing -> black
  { int c = code; if (have || cachedWeatherCode(c)) refreshSky(c, false); }
  if (version == renderedWeatherVersion && renderedWifiState == (int)up) return;
  renderedWeatherVersion = version;
  renderedWifiState = up;
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code, !isDay));
    lv_label_set_text_fmt(lblRange, LV_SYMBOL_UP " %.0f°   " LV_SYMBOL_DOWN " %.0f°   ·   sensação %.0f°", mx[0], mn[0], feels);
    lv_label_set_text_fmt(lblHumidity, "%d%%", humidity);
    lv_label_set_text_fmt(lblWind, "%.0f km/h", wind);
    lv_label_set_text_fmt(lblRainNow, "%d%%", hourCount ? hrain[0] : 0);
    if (uv >= 0) lv_label_set_text_fmt(lblUv, "%.0f", uv);
    else lv_label_set_text(lblUv, "--");
    float weekLo = mn[0], weekHi = mx[0];
    for (int i = 1; i < 7; i++) { weekLo = min(weekLo, mn[i]); weekHi = max(weekHi, mx[i]); }
    if (weekHi - weekLo < 1) weekHi = weekLo + 1;
    for (int i = 0; i < 7; i++) {
      lv_label_set_text(fcDay[i], i == 0 ? "Hoje" : dn[i]);
      lv_label_set_text(fcCond[i], weatherText(dc[i]));
      lv_label_set_text_fmt(fcTemp[i], "%.0f°", mx[i]);
      lv_label_set_text_fmt(fcMin[i], "mín %.0f°", mn[i]);
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
        if (i == 0) lv_label_set_text(hourTime[i], "Agora");
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
        lv_obj_remove_flag(hourCard[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(hourCard[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (renderedCode != code * 2 + !isDay) {
      renderedCode = code * 2 + !isDay;
      // Change the scene colours to suit the current condition.
      refreshSky(code, true);      // the sky photo now carries the condition
      lv_obj_clean(weatherArt);
      // Keep the original large weather artwork on Agora; card icons below
      // are a compact rendering of this same cloud/sun/drop visual language.
      lv_obj_t *cloudBase = lv_obj_create(weatherArt);
      lv_obj_set_pos(cloudBase, 22, 61); lv_obj_set_size(cloudBase, 106, 42);
      lv_obj_set_style_radius(cloudBase, 22, 0);
      lv_obj_set_style_bg_color(cloudBase, lv_color_hex(0xBBD2EA), 0);
      lv_obj_set_style_border_width(cloudBase, 0, 0);
      lv_obj_t *puff1 = lv_obj_create(weatherArt);
      lv_obj_set_pos(puff1, 34, 38); lv_obj_set_size(puff1, 54, 54);
      lv_obj_set_style_radius(puff1, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(puff1, lv_color_hex(0xD8E7F5), 0);
      lv_obj_set_style_border_width(puff1, 0, 0);
      lv_obj_t *puff2 = lv_obj_create(weatherArt);
      lv_obj_set_pos(puff2, 68, 49); lv_obj_set_size(puff2, 44, 44);
      lv_obj_set_style_radius(puff2, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(puff2, lv_color_hex(0xD8E7F5), 0);
      lv_obj_set_style_border_width(puff2, 0, 0);
      if (code <= 2) {                                         // sun by day, moon at night
        lv_obj_t *sun = lv_obj_create(weatherArt);
        if (isDay) { lv_obj_set_pos(sun, 68, 5); lv_obj_set_size(sun, 62, 62); }
        else       { lv_obj_set_pos(sun, 74, 8); lv_obj_set_size(sun, 50, 50); }
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sun, lv_color_hex(isDay ? 0xFFC96B : 0xE9EEF7), 0);
        lv_obj_set_style_border_width(sun, 0, 0);
        lv_obj_move_to_index(sun, 0);
        if (!isDay) {
          lv_obj_t *cut = lv_obj_create(weatherArt);
          lv_obj_set_pos(cut, 92, 0); lv_obj_set_size(cut, 42, 42);
          lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
          lv_obj_set_style_bg_color(cut, lv_color_hex(0x1B2740), 0);
          lv_obj_set_style_border_width(cut, 0, 0);
          lv_obj_move_to_index(cut, 1);
        }
      }
      addRainDrops(weatherArt, code, 1.0f);
    }
  } else {
    lv_label_set_text(lblTemp, "--");
    lv_label_set_text(lblCond, up ? "Carregando previsão..." : "Configure o Wi-Fi em Ajustes");
  }

}

static void renderWeather() {
  struct tm ti;
  char b[24];
  if (localNow(&ti)) {
    strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M %p", &ti);
    setLabelIfChanged(lblClock, b);
    static const char *days[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
    static const char *months[] = {"Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez"};
    snprintf(b, sizeof b, "%s, %02d %s", days[ti.tm_wday], ti.tm_mday, months[ti.tm_mon]);
    setLabelIfChanged(lblDate, b);
  } else {
    setLabelIfChanged(lblClock, "--:--");
    setLabelIfChanged(lblDate, "Aguardando hora...");
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  String status = sh.status;
  bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  // Agora tab: only show the status when something needs attention (errors,
  // searching); "Previsão atualizada" / "Conectado" stay hidden.
  const bool showStatus = status.startsWith("Falha") || status.startsWith("Buscando") || !up;
  setLabelIfChanged(lblStatus, showStatus ? status.c_str() : "");
  static int renderedDot = -1;
  if (renderedDot != (int)up) { renderWifiIndicator(lblWifiDot, up); renderedDot = up; }
  updateWeatherScreen();
}

static const NetJobDef kWeatherJobs[] = {{CMD_WEATHER, "weather", weatherFetch, true}};
const ScreenLifecycle kWeatherScreen = {
  SCREEN_WEATHER, "weather", &weatherView, buildWeatherScreen, weatherOnEnter, nullptr, renderWeather,
  nullptr, WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS, kWeatherJobs, 1, nullptr,
};
