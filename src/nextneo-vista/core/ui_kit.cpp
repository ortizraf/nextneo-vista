// Small UI kit shared by every screen: labels, cards, the back button and safe text.
#include <Arduino.h>
#include <string.h>
#include "app_screens.h"

// Our fonts (lv_font_weather_14/20) have ASCII, Latin-1, Latin Extended-A, "•" and "€";
// LVGL's symbols (U+F000..F8FF) come from the Montserrat fallback. Anything else (an em
// dash, curly quotes from an API, ...) would show as an empty box, so it is replaced.
static bool glyphOk(uint32_t c) {
  return c < 0x80 || (c >= 0xA0 && c <= 0x17F) || c == 0x2022 || c == 0x20AC || (c >= 0xF000 && c <= 0xF8FF);
}
const char *uiSafeText(const char *in, char *out, size_t n) {
  const uint8_t *p = (const uint8_t *)in;
  bool clean = true;
  for (const uint8_t *q = p; *q; ++q) if (*q >= 0xE0) { clean = false; break; }   // 3/4-byte chars only
  if (clean || n < 4) return in;
  size_t o = 0;
  while (*p && o + 4 < n) {
    uint32_t c = *p; int len = 1;
    if (c >= 0xF0 && p[1] && p[2] && p[3]) { c = ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); len = 4; }
    else if (c >= 0xE0 && p[1] && p[2]) { c = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); len = 3; }
    else if (c >= 0xC0 && p[1]) len = 2;
    if (len < 3 || glyphOk(c)) { memcpy(out + o, p, len); o += len; }
    else if (c >= 0x2010 && c <= 0x2015) out[o++] = '-';
    else if (c >= 0x2018 && c <= 0x201B) out[o++] = '\'';
    else if (c >= 0x201C && c <= 0x201F) out[o++] = '"';
    else if (c == 0x2026) { memcpy(out + o, "...", 3); o += 3; }
    else if (c == 0x2039) out[o++] = '<';
    else if (c == 0x203A) out[o++] = '>';
    else out[o++] = '?';
    p += len;
  }
  out[o] = 0;
  return out;
}

void setLabelIfChanged(lv_obj_t *label, const char *text) {
  if (!label || !text) return;
  char safe[256];
  text = uiSafeText(text, safe, sizeof safe);
  const char *current = lv_label_get_text(label);
  if (current && strcmp(current, text) == 0) return;  // avoid needless invalidation/redraw
  lv_label_set_text(label, text);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, txt);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

lv_obj_t *makeCard(lv_obj_t *parent, int w, int h) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x243247), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

// The one back button of the app: same size (40x34), position and "glass" look on
// every screen. Screens must not restyle or move it.
static void backPressFx(lv_event_t *e) {
  lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
  const bool down = lv_event_get_code(e) == LV_EVENT_PRESSED;
  lv_obj_set_style_bg_opa(b, down ? LV_OPA_70 : LV_OPA_40, LV_PART_MAIN);
}
lv_obj_t *addBackButton(lv_obj_t *parent) {
  lv_obj_t *back = lv_button_create(parent);
  lv_obj_set_size(back, 40, 34);
  lv_obj_set_pos(back, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x0A1220), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(back, LV_GRAD_DIR_NONE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_40, LV_PART_MAIN);           // translucent, readable on any background
  lv_obj_set_style_border_color(back, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_border_opa(back, LV_OPA_30, LV_PART_MAIN);       // thin light rim, like iOS glass buttons
  lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x0A1220), LV_STATE_PRESSED);
  lv_obj_add_event_cb(back, backPressFx, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(back, backPressFx, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(back, backPressFx, LV_EVENT_PRESS_LOST, NULL);
  lv_obj_add_event_cb(back, [](lv_event_t *) { switchScreen(SCREEN_MENU); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *arrow = lv_label_create(back);
  lv_label_set_text(arrow, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(arrow, &lv_font_weather_14, 0);
  lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(arrow);
  lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
  return back;
}

void renderWifiIndicator(lv_obj_t *icon, bool up) {
  lv_obj_set_style_text_color(icon, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
}
