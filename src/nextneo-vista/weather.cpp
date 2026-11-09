#include "app_screens.h"

static int renderedCode = -999;
static int renderedHourCodes[8] = {-999,-999,-999,-999,-999,-999,-999,-999};
static int renderedDayCodes[7] = {-999,-999,-999,-999,-999,-999,-999};
static uint32_t renderedWeatherVersion = UINT32_MAX;
static int renderedWifiState = -1;

// On-Enter: force one full repaint from the cached forecast.
void weatherOnEnter() {
  renderedWeatherVersion = UINT32_MAX;
  renderedWifiState = -1;
}

static lv_obj_t *makeWeatherIcon(lv_obj_t *parent, int x, int y, int size, int code) {
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
    lv_obj_t *sun = lv_obj_create(icon);
    lv_obj_set_size(sun, scaled(62), scaled(62));
    lv_obj_set_pos(sun, scaled(68), scaled(5));
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFC96B), 0);
    lv_obj_set_style_border_width(sun, 0, 0);
    lv_obj_move_to_index(sun, 0);
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
  if (isRainCode(code)) {
    for (int r = 0; r < 3; r++) {
      lv_obj_t *drop = lv_obj_create(icon);
      lv_obj_set_pos(drop, scaled(42 + r * 35), scaled(110));
      lv_obj_set_size(drop, scaled(6), scaled(22));
      lv_obj_set_style_radius(drop, scaled(4), 0);
      lv_obj_set_style_bg_color(drop, lv_color_hex(0x6FC8FF), 0);
      lv_obj_set_style_border_width(drop, 0, 0);
      lv_obj_set_style_transform_rotation(drop, 120, 0);
    }
  }
  return icon;
}


static void build_home(lv_obj_t *t) {
  homeTab = t;
  lv_obj_set_style_bg_color(t, lv_color_hex(0x17253D), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x334C70), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

  lblCity = makeLabel(t, "São Paulo", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_width(lblCity, 370);
  lv_label_set_long_mode(lblCity, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lblCity, 52, 13);
  lblDate = makeLabel(t, "", &lv_font_weather_14, 0xAFC0D6);
  lv_obj_set_pos(lblDate, 22, 42);
  lblClock = makeLabel(t, "--:--", &lv_font_montserrat_28, 0xEFF4FA);
  lv_obj_set_pos(lblClock, 20, 59);
  lblWifiDot = makeLabel(t, LV_SYMBOL_WIFI, &lv_font_weather_20, 0xD05050);
  lv_obj_align(lblWifiDot, LV_ALIGN_TOP_RIGHT, -18, 12);

  lblTemp = makeLabel(t, "--°", &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_set_pos(lblTemp, 20, 105);
  lblCond = makeLabel(t, "Aguardando clima", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(lblCond, 24, 158);
  lblRange = makeLabel(t, "--° / --°", &lv_font_weather_14, 0xC2D0E3);
  lv_obj_set_pos(lblRange, 24, 185);

  weatherArt = lv_obj_create(t);
  lv_obj_set_size(weatherArt, 130, 118);
  lv_obj_align(weatherArt, LV_ALIGN_TOP_RIGHT, -24, 66);
  lv_obj_set_style_bg_opa(weatherArt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weatherArt, 0, 0);
  lv_obj_set_style_pad_all(weatherArt, 0, 0);
  lv_obj_remove_flag(weatherArt, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *humidityCard = makeCard(t, 142, 38);
  lv_obj_align(humidityCard, LV_ALIGN_BOTTOM_LEFT, 18, -1);
  lblHumidity = makeLabel(humidityCard, "Umidade  --%", &lv_font_weather_14, 0xE1EAF6);
  lv_obj_center(lblHumidity);
  lv_obj_t *windCard = makeCard(t, 142, 38);
  lv_obj_align(windCard, LV_ALIGN_BOTTOM_LEFT, 168, -1);
  lblWind = makeLabel(windCard, "Vento  -- km/h", &lv_font_weather_14, 0xE1EAF6);
  lv_obj_center(lblWind);
  lblStatus = makeLabel(t, "", &lv_font_weather_14, 0x9FB0C7);
  lv_obj_align(lblStatus, LV_ALIGN_BOTTOM_RIGHT, 8, -10);
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
  lv_obj_set_style_bg_color(t, lv_color_hex(0x111C2E), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x1B2D47), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
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
  lv_obj_set_style_bg_color(t, lv_color_hex(0x111C2E), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x253750), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
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


void buildWeatherScreen(lv_obj_t *scr) {
  weatherView = lv_obj_create(scr);
  lv_obj_set_size(weatherView, SCR_W, SCR_H);
  lv_obj_set_pos(weatherView, 0, 0);
  lv_obj_set_style_pad_all(weatherView, 0, 0);
  lv_obj_set_style_border_width(weatherView, 0, 0);
  lv_obj_set_style_bg_opa(weatherView, LV_OPA_TRANSP, 0);
  weatherTabs = lv_tabview_create(weatherView);
  lv_obj_set_size(weatherTabs, SCR_W, SCR_H);
  lv_obj_set_pos(weatherTabs, 0, 0);
  lv_tabview_set_tab_bar_position(weatherTabs, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(weatherTabs, 42);
  lv_obj_set_style_bg_color(weatherTabs, lv_color_hex(0x101820), 0);
  lv_obj_t *tabBar = lv_tabview_get_tab_bar(weatherTabs);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x0B1424), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, LV_PART_MAIN);
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
  addBackButton(home);
  addBackButton(hours);
  addBackButton(days);

}

// UI thread, active Weather screen only. Widgets are rewritten only when the
// forecast changed (version) or the Wi-Fi state changed.
void updateWeatherScreen() {
  String city;
  bool up, have;
  uint32_t version;
  float temp, wind, mx[7], mn[7], htemp[8];
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
  hourCount = sh.hourCount;
  memcpy(hours, sh.hourLabel, sizeof hours);
  memcpy(htemp, sh.hourTemp, sizeof htemp);
  memcpy(hcode, sh.hourCode, sizeof hcode);
  memcpy(hrain, sh.hourRain, sizeof hrain);
  memcpy(dn, sh.dayName, sizeof dn);
  memcpy(mx, sh.tmax, sizeof mx);
  memcpy(mn, sh.tmin, sizeof mn);
  memcpy(dc, sh.dcode, sizeof dc);
  memcpy(drain, sh.dayRain, sizeof drain);
  version = sh.weatherVersion;
  xSemaphoreGive(mtx);
  if (version == renderedWeatherVersion && renderedWifiState == (int)up) return;
  renderedWeatherVersion = version;
  renderedWifiState = up;
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code));
    lv_label_set_text_fmt(lblRange, "%.0f° / %.0f°", mx[0], mn[0]);
    lv_label_set_text_fmt(lblHumidity, "%d%% umid.", humidity);
    lv_label_set_text_fmt(lblWind, "%.0f km/h vento", wind);
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
        lv_label_set_text(hourCond[i], weatherText(hcode[i]));
        lv_label_set_text_fmt(hourTemp[i], "%.0f°", htemp[i]);
        lv_label_set_text_fmt(hourRain[i], "%d%%", hrain[i]);
        if (renderedHourCodes[i] != hcode[i]) {
          if (hourArt[i]) lv_obj_del(hourArt[i]);
          hourArt[i] = makeWeatherIcon(hourCard[i], 14, 40, 48, hcode[i]);
          renderedHourCodes[i] = hcode[i];
        }
        lv_obj_remove_flag(hourCard[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(hourCard[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (renderedCode != code) {
      renderedCode = code;
      // Change the scene colours to suit the current condition.
      uint32_t top = (code == 0) ? 0x17253D : (isRainCode(code) ? 0x101B30 : 0x1A263B);
      uint32_t bottom = (code == 0) ? 0x334C70 : (isRainCode(code) ? 0x203B5C : 0x344960);
      lv_obj_set_style_bg_color(homeTab, lv_color_hex(top), 0);
      lv_obj_set_style_bg_grad_color(homeTab, lv_color_hex(bottom), 0);
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
      if (code <= 2) {
        lv_obj_t *sun = lv_obj_create(weatherArt);
        lv_obj_set_pos(sun, 68, 5); lv_obj_set_size(sun, 62, 62);
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFC96B), 0);
        lv_obj_set_style_border_width(sun, 0, 0);
        lv_obj_move_to_index(sun, 0);
      }
      if (isRainCode(code)) {
        for (int r = 0; r < 3; r++) {
          lv_obj_t *drop = lv_obj_create(weatherArt);
          lv_obj_set_pos(drop, 42 + r * 35, 110); lv_obj_set_size(drop, 6, 22);
          lv_obj_set_style_radius(drop, 4, 0);
          lv_obj_set_style_bg_color(drop, lv_color_hex(0x6FC8FF), 0);
          lv_obj_set_style_border_width(drop, 0, 0);
          lv_obj_set_style_transform_rotation(drop, 120, 0);
        }
      }
    }
  } else {
    lv_label_set_text(lblTemp, "--");
    lv_label_set_text(lblCond, up ? "Carregando previsão..." : "Configure o Wi-Fi em Ajustes");
  }

}
