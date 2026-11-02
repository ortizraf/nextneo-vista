#include "app_screens.h"

static int renderedCode = -999;
static int renderedHourCodes[8] = {-999,-999,-999,-999,-999,-999,-999,-999};
static int renderedDayCodes[7] = {-999,-999,-999,-999,-999,-999,-999};

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

static void build_hours(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x111C2E), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x1B2D47), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = makeLabel(t, "Previsão por hora", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);

  lv_obj_t *strip = lv_obj_create(t);
  lv_obj_set_size(strip, 452, 232);
  lv_obj_set_pos(strip, 14, 29);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 4, 0);
  lv_obj_set_style_pad_column(strip, 8, 0);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < 8; i++) {
    lv_obj_t *card = makeCard(strip, 92, 158);
    lv_obj_set_style_translate_y(card, -16, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    hourTime[i] = makeLabel(card, "--", &lv_font_weather_14, 0xAFC0D6);
    lv_obj_align(hourTime[i], LV_ALIGN_TOP_MID, 0, 5);
    hourArt[i] = NULL;
    hourCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xDCE8F7);
    lv_obj_add_flag(hourCond[i], LV_OBJ_FLAG_HIDDEN);
    hourTemp[i] = makeLabel(card, "--", &lv_font_weather_20, 0xFFFFFF);
    lv_obj_align(hourTemp[i], LV_ALIGN_CENTER, 0, 27);
    hourRain[i] = makeLabel(card, "", &lv_font_weather_14, 0x74D2FF);
    lv_obj_set_width(hourRain[i], 80);
    lv_label_set_long_mode(hourRain[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(hourRain[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hourRain[i], LV_ALIGN_BOTTOM_MID, 0, -5);
  }
  lv_obj_t *hint = makeLabel(t, "A cada 3 h  ·  deslize para ver mais", &lv_font_weather_14, 0x9FB0C7);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
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
  lv_obj_set_size(strip, 452, 232);
  lv_obj_set_pos(strip, 14, 21);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 4, 0);
  lv_obj_set_style_pad_column(strip, 8, 0);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < 7; i++) {
    lv_obj_t *card = makeCard(strip, 92, 158);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    fcDay[i] = makeLabel(card, "", &lv_font_weather_14, 0xAFC0D6);
    lv_obj_set_width(fcDay[i], 82);
    lv_obj_set_style_text_align(fcDay[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fcDay[i], LV_ALIGN_TOP_MID, 0, 5);
    dayArt[i] = NULL;
    fcCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xEAF2FF);
    lv_obj_add_flag(fcCond[i], LV_OBJ_FLAG_HIDDEN);
    fcTemp[i] = makeLabel(card, "", &lv_font_weather_20, 0xFFD477);
    lv_obj_set_style_text_align(fcTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fcTemp[i], LV_ALIGN_CENTER, 0, 27);
  }
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

void updateWeatherScreen() {
  String city;
  bool up, have;
  float temp, wind, mx[7], mn[7], htemp[8];
  int code, humidity, hourCount, hcode[8], hrain[8], dc[7];
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
  xSemaphoreGive(mtx);
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code));
    lv_label_set_text_fmt(lblRange, "%.0f° / %.0f°", mx[0], mn[0]);
    lv_label_set_text_fmt(lblHumidity, "%d%% umid.", humidity);
    lv_label_set_text_fmt(lblWind, "%.0f km/h vento", wind);
    for (int i = 0; i < 7; i++) {
      lv_label_set_text(fcDay[i], i == 0 ? "Hoje" : dn[i]);
      lv_label_set_text(fcCond[i], weatherText(dc[i]));
      lv_label_set_text_fmt(fcTemp[i], "%.0f° / %.0f°", mx[i], mn[i]);
      if (renderedDayCodes[i] != dc[i]) {
        if (dayArt[i]) lv_obj_del(dayArt[i]);
        dayArt[i] = makeWeatherIcon(lv_obj_get_parent(fcDay[i]), 17, 35, 40, dc[i]);
        renderedDayCodes[i] = dc[i];
      }
      lv_obj_move_foreground(fcDay[i]);
      lv_obj_move_foreground(fcTemp[i]);
    }
    for (int i = 0; i < 8; i++) {
      if (i < hourCount) {
        lv_label_set_text_fmt(hourTime[i], "%sh", hours[i]);
        lv_label_set_text(hourCond[i], weatherText(hcode[i]));
        lv_label_set_text_fmt(hourTemp[i], "%.0f°", htemp[i]);
        lv_label_set_text_fmt(hourRain[i], "%d%%", hrain[i]);
        if (renderedHourCodes[i] != hcode[i]) {
          if (hourArt[i]) lv_obj_del(hourArt[i]);
          hourArt[i] = makeWeatherIcon(lv_obj_get_parent(hourTime[i]), 17, 35, 40, hcode[i]);
          renderedHourCodes[i] = hcode[i];
        }
        lv_obj_move_foreground(hourTime[i]);
        lv_obj_move_foreground(hourTemp[i]);
        lv_obj_move_foreground(hourRain[i]);
      } else {
        lv_label_set_text(hourTime[i], "--");
        lv_label_set_text(hourCond[i], "--");
        lv_label_set_text(hourTemp[i], "--");
        lv_label_set_text(hourRain[i], "");
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
