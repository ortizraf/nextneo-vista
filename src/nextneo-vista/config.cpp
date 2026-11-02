#include "app_screens.h"
#include <WiFi.h>

static void ta_event(lv_event_t *e) {
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  }
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

static void h24_event(lv_event_t *e) {
  cfg.h24 = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
  prefs.putBool("h24", cfg.h24);
}

static void bright_event(lv_event_t *e) {
  cfg.bright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
  analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255));
  prefs.putInt("bl", cfg.bright);
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
  lv_obj_set_style_pad_bottom(t, 205, 0);  // room to scroll above the landscape keyboard

  makeLabel(t, "Wi-Fi", &lv_font_weather_20, 0xFFFFFF);
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

  makeLabel(t, "Cidade (Enter para aplicar)", &lv_font_weather_14, 0xFFFFFF);
  taCity = lv_textarea_create(t);
  lv_obj_set_width(taCity, 430);
  lv_textarea_set_one_line(taCity, true);
  lv_textarea_set_text(taCity, sh.cityName.c_str());
  lv_obj_set_style_text_font(taCity, &lv_font_weather_20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(taCity, lv_color_hex(0x17243A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(taCity, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(taCity, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
  lv_obj_set_style_border_color(taCity, lv_color_hex(0x3A506D), LV_PART_MAIN);
  lv_obj_set_style_border_width(taCity, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(taCity, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(taCity, ta_event, LV_EVENT_FOCUSED, NULL);

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
}


void buildConfigScreen(lv_obj_t *screen) {
  lv_obj_set_size(screen, SCR_W, SCR_H);
  lv_obj_set_pos(screen, 0, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
  build_settings(screen);
}

void buildConfigKeyboard() {
  kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(kb, SCR_W, 190);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, &lv_font_weather_20, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x0B1424), LV_PART_MAIN);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x1B2A42), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(0xEAF2FF), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x1767A8), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_pad_all(kb, 4, 0);
  lv_obj_set_style_pad_gap(kb, 5, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kb, kb_event, LV_EVENT_ALL, NULL);
}
