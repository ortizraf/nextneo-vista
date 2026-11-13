#include "app_screens.h"
#include "modules/flight_radar/flight_radar.h"
#include <WiFi.h>
#include "services/web_auth.h"

static lv_obj_t *taAirport;
static lv_obj_t *airportConfigStatus;
static lv_obj_t *radarRadiusDropdown;

static constexpr int KB_H = 156;          // compact keyboard height (buildConfigKeyboard)

// Keeps the focused field fully above the keyboard. scroll_to_view alone only makes it
// visible inside the settings list, which may still be under the keyboard: the field's
// blue focus border then peeked out as a strip on top of the keys.
static void keepAboveKeyboard(lv_obj_t *ta) {
  lv_obj_t *scroller = lv_obj_get_parent(ta);
  while (scroller && !(lv_obj_has_flag(scroller, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(scroller) & LV_DIR_VER)))
    scroller = lv_obj_get_parent(scroller);
  if (!scroller) return;
  lv_obj_update_layout(scroller);
  lv_area_t a;
  lv_obj_get_coords(ta, &a);
  const int limit = SCR_H - KB_H - 8;                   // 8 px gap above the keys
  if (a.y2 > limit) lv_obj_scroll_by(scroller, 0, -(a.y2 - limit), LV_ANIM_OFF);
  else if (a.y1 < 44) lv_obj_scroll_by(scroller, 0, 44 - a.y1, LV_ANIM_OFF);   // not under the back button
}

static lv_obj_t *taWebUser, *taWebPass, *webAuthStatus;

static lv_obj_t *styledField(lv_obj_t *parent, const char *text, bool password);

static void web_auth_save_event(lv_event_t *) {
  const char *u = lv_textarea_get_text(taWebUser);
  const char *p = lv_textarea_get_text(taWebPass);
  if (webAuthSet(u, p)) {
    lv_textarea_set_text(taWebPass, "");
    lv_label_set_text(webAuthStatus, "Acesso salvo. Entre de novo na p\xC3\xA1gina com os dados novos.");
    lv_obj_set_style_text_color(webAuthStatus, lv_color_hex(0x8FE0B6), 0);
  } else {
    lv_label_set_text(webAuthStatus, "Preencha o usu\xC3\xA1rio e uma senha de 4 a 64 caracteres.");
    lv_obj_set_style_text_color(webAuthStatus, lv_color_hex(0xFFD477), 0);
  }
  if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// Text of the city field, identical to the web page: what the user typed when it was a
// manual location ("Nome @ lat,lon", prefs "cityRaw"), otherwise the geocoded name.
static String cityFieldText() {
  if (prefs.isKey("cityRaw")) return prefs.getString("cityRaw", "");
  xSemaphoreTake(mtx, portMAX_DELAY); String c = sh.cityName; xSemaphoreGive(mtx);
  return c;
}

// Settings tick: the city may have been changed on the web page or by a search;
// refresh the field when it changed (never while the user is editing it).
void configSyncCityUi() {
  static uint32_t seen = UINT32_MAX;
  if (!taCity) return;
  xSemaphoreTake(mtx, portMAX_DELAY); const uint32_t v = sh.weatherVersion; xSemaphoreGive(mtx);
  if (v == seen || lv_obj_has_state(taCity, LV_STATE_FOCUSED)) return;
  seen = v;
  const String t = cityFieldText();
  if (strcmp(lv_textarea_get_text(taCity), t.c_str())) lv_textarea_set_text(taCity, t.c_str());
}

static void ta_event(lv_event_t *e) {
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
    keepAboveKeyboard(ta);
  }
}

// One-line text field in the Settings look (same style as Cidade).
static lv_obj_t *styledField(lv_obj_t *parent, const char *text, bool password) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_width(ta, 430);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_text(ta, text);
  lv_obj_set_style_text_font(ta, &lv_font_weather_20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ta, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(ta, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(ta, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(ta, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(ta, ta_event, LV_EVENT_FOCUSED, NULL);
  return ta;
}

static void kb_event(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
  lv_obj_t *ta = lv_keyboard_get_textarea(kb);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_state(ta, LV_STATE_FOCUSED);
  if (c == LV_EVENT_READY && ta == taCity) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.pendingCity = lv_textarea_get_text(taCity);
    sh.status = "Buscando cidade...";
    xSemaphoreGive(mtx);
    post(CMD_GEOCODE);
  } else if (c == LV_EVENT_READY && ta == taAirport) {
    const bool ok = setFlightRadarAirportCode(lv_textarea_get_text(taAirport));
    lv_label_set_text(airportConfigStatus, ok ? "Aeroporto salvo para o radar." : "Código não reconhecido. Confira o código IATA/ICAO.");
  }
}

static void scan_event(lv_event_t *) { post(CMD_SCAN); }

static void connect_event(lv_event_t *) {
  char ssid[64] = "";
  lv_dropdown_get_selected_str(ddSsid, ssid, sizeof ssid);
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.pendingSsid = ssid;
  sh.pendingPass = lv_textarea_get_text(taPass);
  xSemaphoreGive(mtx);
  post(CMD_CONNECT);
}

static void apply_airport_event(lv_event_t *) {
  const bool ok = setFlightRadarAirportCode(lv_textarea_get_text(taAirport));
  lv_label_set_text(airportConfigStatus, ok ? "Aeroporto salvo para o radar." : "Código não reconhecido. Confira o código IATA/ICAO.");
}

static void radar_radius_event(lv_event_t *e) {
  lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_target(e);
  static const uint8_t radii[] = {5, 10, 25, 50};
  const uint32_t selected = lv_dropdown_get_selected(dropdown);
  const uint8_t radiusKm = radii[selected < 4 ? selected : 1];
  setFlightRadarRadiusKm(radiusKm);
  lv_label_set_text_fmt(airportConfigStatus, "Raio salvo: %u km", radiusKm);
}

static void h24_event(lv_event_t *e) {
  cfg.h24 = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
  prefs.putBool("h24", cfg.h24);
}

static void bright_event(lv_event_t *e) {
  cfg.bright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
  analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255));
  prefs.putInt("bl", cfg.bright);
}

// One titled block (card) of the Settings list. Widgets inside keep their 430 px width.
static lv_obj_t *settingsBlock(lv_obj_t *list, const char *icon, const char *title) {
  lv_obj_t *b = lv_obj_create(list);
  lv_obj_set_size(b, 456, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x131E33), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x24344F), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 12, 0);
  lv_obj_set_style_pad_row(b, 8, 0);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);      // the list scrolls, not the block
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *head = lv_obj_create(b);
  lv_obj_set_size(head, 430, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(head, 0, 0);
  lv_obj_set_style_pad_all(head, 0, 0);
  lv_obj_set_style_pad_column(head, 8, 0);
  lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *ic = makeLabel(head, icon, &lv_font_montserrat_20, 0x38D6F5);
  (void)ic;
  makeLabel(head, title, &lv_font_weather_20, 0xFFFFFF);
  return b;
}

static void build_settings(lv_obj_t *t) {
  lv_obj_t *content = lv_obj_create(t);
  lv_obj_set_size(content, SCR_W, SCR_H - 36);
  lv_obj_set_pos(content, 0, 36);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  addBackButton(t);
  t = content;
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(t, 8, 0);
  lv_obj_set_style_pad_bottom(t, 170, 0);  // room to scroll above the compact keyboard (156 px)
  lv_obj_set_style_pad_row(t, 12, 0);       // space between blocks
  lv_obj_set_style_pad_top(t, 6, 0);
  lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  t = settingsBlock(content, LV_SYMBOL_WIFI, "Wi-Fi");
  lv_obj_t *r1 = lv_obj_create(t);
  lv_obj_set_size(r1, 430, 50);
  lv_obj_set_style_bg_opa(r1, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r1, 0, 0);
  lv_obj_set_style_pad_all(r1, 0, 0);
  lv_obj_remove_flag(r1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ddSsid = lv_dropdown_create(r1);
  lv_dropdown_set_options(ddSsid, "(toque em Buscar redes)");
  lv_obj_set_width(ddSsid, 320);
  lv_obj_set_style_bg_color(ddSsid, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ddSsid, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_font(ddSsid, &lv_font_weather_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(ddSsid, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(ddSsid, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(ddSsid, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(ddSsid, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ddSsid, lv_color_hex(0x17243A), LV_PART_ITEMS);
  lv_obj_set_style_text_color(ddSsid, lv_color_hex(0xEAF2FF), LV_PART_ITEMS);
  lv_obj_set_style_text_font(ddSsid, &lv_font_weather_14, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(ddSsid, lv_color_hex(0x25476D), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_t *bScan = lv_button_create(r1);
  lv_obj_set_size(bScan, 96, 44);
  lv_obj_add_event_cb(bScan, scan_event, LV_EVENT_CLICKED, NULL);
  lv_obj_center(lv_label_create(bScan));
  lv_label_set_text(lv_obj_get_child(bScan, 0), "Buscar");

  taPass = lv_textarea_create(t);
  String savedPassword = prefs.getString("pass", "");
  if (!savedPassword.isEmpty()) lv_textarea_set_text(taPass, savedPassword.c_str());
  lv_obj_set_width(taPass, 430);
  lv_textarea_set_one_line(taPass, true);
  lv_textarea_set_password_mode(taPass, true);
  lv_textarea_set_placeholder_text(taPass, "Senha do Wi-Fi");
  lv_obj_set_style_bg_color(taPass, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(taPass, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(taPass, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_text_color(taPass, lv_color_hex(0x93A8C2), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_color(taPass, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(taPass, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(taPass, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(taPass, ta_event, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *bConn = lv_button_create(t);
  lv_obj_set_size(bConn, 430, 44);
  lv_obj_add_event_cb(bConn, connect_event, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(bConn), "Conectar");
  lv_obj_center(lv_obj_get_child(bConn, 0));

  setStatusLbl = makeLabel(t, "", &lv_font_weather_14, 0x8FB0D0);
  wifiInfoLbl = makeLabel(t, "Wi-Fi: verificando...", &lv_font_weather_14, 0x8FE0B6);
  lv_obj_set_width(wifiInfoLbl, 430);
  lv_label_set_long_mode(wifiInfoLbl, LV_LABEL_LONG_DOT);

  t = settingsBlock(content, LV_SYMBOL_HOME, "Localiza\xC3\xA7\xC3\xA3o");
  makeLabel(t, "Cidade (Enter para aplicar)", &lv_font_weather_14, 0xFFFFFF);
  taCity = lv_textarea_create(t);
  lv_obj_set_width(taCity, 430);
  lv_textarea_set_one_line(taCity, true);
  lv_textarea_set_text(taCity, cityFieldText().c_str());   // same text as the web page
  lv_obj_set_style_text_font(taCity, &lv_font_weather_20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(taCity, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(taCity, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(taCity, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(taCity, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(taCity, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(taCity, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(taCity, ta_event, LV_EVENT_FOCUSED, NULL);
  makeLabel(t, "Ex.: São Paulo, BR  ou  Nome @ lat,lon", &lv_font_weather_14, 0x8FB0D0);

  t = settingsBlock(content, LV_SYMBOL_GPS, "Radar e aeroporto");
  makeLabel(t, "Aeroporto do radar (IATA/ICAO)", &lv_font_weather_14, 0xFFFFFF);
  taAirport = lv_textarea_create(t);
  String airport = prefs.getString("radarAirport", "CGH");
  lv_textarea_set_text(taAirport, airport.c_str());
  lv_textarea_set_one_line(taAirport, true);
  lv_textarea_set_max_length(taAirport, 4);
  lv_textarea_set_placeholder_text(taAirport, "CGH");
  lv_obj_set_width(taAirport, 430);
  lv_obj_set_style_bg_color(taAirport, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(taAirport, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(taAirport, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(taAirport, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(taAirport, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(taAirport, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(taAirport, ta_event, LV_EVENT_FOCUSED, NULL);
  lv_obj_t *bAirport = lv_button_create(t);
  lv_obj_set_size(bAirport, 430, 40);
  lv_obj_add_event_cb(bAirport, apply_airport_event, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(bAirport), "Aplicar aeroporto");
  lv_obj_center(lv_obj_get_child(bAirport, 0));
  airportConfigStatus = makeLabel(t, "Padrão: Congonhas (CGH), raio 10 km", &lv_font_weather_14, 0x8FE0B6);
  makeLabel(t, "Raio do radar", &lv_font_weather_14, 0xFFFFFF);
  radarRadiusDropdown = lv_dropdown_create(t);
  lv_dropdown_set_options(radarRadiusDropdown, "5 km\n10 km (padrão)\n25 km\n50 km");
  {
    const uint8_t saved = prefs.getUChar("radarRadiusKm", 10);
    lv_dropdown_set_selected(radarRadiusDropdown, saved == 5 ? 0 : saved == 25 ? 2 : saved == 50 ? 3 : 1);
  }
  lv_obj_set_width(radarRadiusDropdown, 430);
  lv_obj_set_style_bg_color(radarRadiusDropdown, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(radarRadiusDropdown, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(radarRadiusDropdown, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(radarRadiusDropdown, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(radarRadiusDropdown, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(radarRadiusDropdown, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(radarRadiusDropdown, radar_radius_event, LV_EVENT_VALUE_CHANGED, NULL);


  t = settingsBlock(content, LV_SYMBOL_IMAGE, "Tela");
  lv_obj_t *r2 = lv_obj_create(t);
  lv_obj_set_size(r2, 430, 44);
  lv_obj_set_style_bg_opa(r2, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r2, 0, 0);
  lv_obj_set_style_pad_all(r2, 0, 0);
  lv_obj_remove_flag(r2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  makeLabel(r2, "Relógio 24 horas", &lv_font_weather_20, 0xFFFFFF);
  swH24 = lv_switch_create(r2);
  if (cfg.h24) lv_obj_add_state(swH24, LV_STATE_CHECKED);
  lv_obj_add_event_cb(swH24, h24_event, LV_EVENT_VALUE_CHANGED, NULL);

  makeLabel(t, "Brilho", &lv_font_weather_14, 0xFFFFFF);
  lv_obj_t *sl = lv_slider_create(t);
  lv_obj_set_width(sl, 410);
  lv_slider_set_range(sl, 0, 100);
  lv_slider_set_value(sl, cfg.bright, LV_ANIM_OFF);
  lv_obj_add_event_cb(sl, bright_event, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- web page login (services/web_auth): user + password asked at http://nextneovista.local
  t = settingsBlock(content, LV_SYMBOL_EYE_CLOSE, "Acesso \xC3\xA0 p\xC3\xA1gina web");
  makeLabel(t, "Usu\xC3\xA1rio", &lv_font_weather_14, 0xFFFFFF);
  taWebUser = styledField(t, webAuthUser().c_str(), false);
  makeLabel(t, "Nova senha (m\xC3\xADnimo 4 caracteres)", &lv_font_weather_14, 0xFFFFFF);
  taWebPass = styledField(t, "", true);
  lv_obj_t *bWeb = lv_button_create(t);
  lv_obj_set_size(bWeb, 430, 42);
  lv_obj_set_style_radius(bWeb, 12, 0);
  lv_obj_set_style_shadow_width(bWeb, 0, 0);
  lv_obj_add_event_cb(bWeb, web_auth_save_event, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(bWeb), "Salvar acesso");
  lv_obj_center(lv_obj_get_child(bWeb, 0));
  webAuthStatus = makeLabel(t, "Padr\xC3\xA3o: admin / admin  \xC2\xB7  a sess\xC3\xA3o expira ap\xC3\xB3s 10 min sem uso",
                            &lv_font_weather_14, 0x8FB0D0);
  lv_obj_set_width(webAuthStatus, 430);
}


void buildConfigScreen(lv_obj_t *screen) {
  lv_obj_set_size(screen, SCR_W, SCR_H);
  lv_obj_set_pos(screen, 0, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
  build_settings(screen);
}

// Compact keyboard: 4 rows, 156 px tall (under half of the 320 px screen). The close
// key lives in the bottom row instead of a separate strip, and the space bar is only
// 4 units wide. "ABC" / "abc" / "1#" are the mode keys LVGL's keyboard already knows.
#define KB_W(w) ((lv_buttonmatrix_ctrl_t)(w))
#define KB_MODE ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED))
static const char *const kKbLower[] = {
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_BACKSPACE, "\n",
  "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "\n",
  "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_CLOSE, LV_SYMBOL_OK, ""};
static const char *const kKbUpper[] = {
  "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
  "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_BACKSPACE, "\n",
  "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "\n",
  "1#", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_CLOSE, LV_SYMBOL_OK, ""};
static const char *const kKbSpecial[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "@", "#", "$", "%", "&", "*", "-", "+", "=", LV_SYMBOL_BACKSPACE, "\n",
  "abc", "_", "/", "?", "!", ":", ";", "'", "\"", "~", "\n",
  "(", ")", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_CLOSE, LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t kKbCtrlText[] = {
  KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1),
  KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1),
  (lv_buttonmatrix_ctrl_t)(KB_MODE | 2), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1),
  (lv_buttonmatrix_ctrl_t)(KB_MODE | 2), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1), KB_W(4), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1),
  (lv_buttonmatrix_ctrl_t)(KB_MODE | 2), (lv_buttonmatrix_ctrl_t)(KB_MODE | 2)};
static const lv_buttonmatrix_ctrl_t kKbCtrlSpecial[] = {
  KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1),
  KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1),
  (lv_buttonmatrix_ctrl_t)(KB_MODE | 2), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1), KB_W(1),
  KB_W(1), KB_W(1), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1), KB_W(4), (lv_buttonmatrix_ctrl_t)(KB_MODE | 1),
  (lv_buttonmatrix_ctrl_t)(KB_MODE | 2), (lv_buttonmatrix_ctrl_t)(KB_MODE | 2)};
void buildConfigKeyboard() {
  kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(kb, SCR_W, KB_H);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char **)kKbLower, kKbCtrlText);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char **)kKbUpper, kKbCtrlText);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, (const char **)kKbSpecial, kKbCtrlSpecial);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  // Light keyboard (iOS-like): white keys, dark text, grey action keys. Every style the
  // default theme sets is overridden, so no theme blue (border/outline/checked) shows up.
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);   // crisp letters + native LV_SYMBOL glyphs
  lv_obj_set_style_bg_color(kb, lv_color_hex(0xCDD2DA), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(kb, 0, (lv_style_selector_t)LV_PART_MAIN | LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(kb, 0, (lv_style_selector_t)LV_PART_MAIN | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_radius(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(0x111418), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0xA9B1BD), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);   // mode / action keys
  lv_obj_set_style_text_color(kb, lv_color_hex(0x111418), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x8E97A5), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x8E97A5), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
  lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);                     // thin bottom edge = key depth
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, lv_color_hex(0x8A929E), LV_PART_ITEMS);
  lv_obj_set_style_outline_width(kb, 0, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_width(kb, 0, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_FOCUSED);
  lv_obj_set_style_pad_all(kb, 4, 0);
  lv_obj_set_style_pad_gap(kb, 4, 0);
  lv_obj_remove_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  // LV_SYMBOL_CLOSE key -> LV_EVENT_CANCEL, LV_SYMBOL_OK -> LV_EVENT_READY (kb_event hides it)
  lv_obj_add_event_cb(kb, kb_event, LV_EVENT_ALL, NULL);
}

// Keeps the radar fields in Settings in sync when the airport/radius was
// changed elsewhere (radar range pill or the web page). UI thread.
void configSyncRadarUi() {
  if (!taAirport || !radarRadiusDropdown) return;
  char code[8];
  uint8_t radius = 10;
  getFlightRadarSettings(code, sizeof code, nullptr, 0, radius);
  const uint32_t index = radius == 5 ? 0 : radius == 25 ? 2 : radius == 50 ? 3 : 1;
  if (lv_dropdown_get_selected(radarRadiusDropdown) != index && !lv_dropdown_is_open(radarRadiusDropdown))
    lv_dropdown_set_selected(radarRadiusDropdown, index);
  if (!lv_obj_has_state(taAirport, LV_STATE_FOCUSED) && strcmp(lv_textarea_get_text(taAirport), code) != 0)
    lv_textarea_set_text(taAirport, code);
}
