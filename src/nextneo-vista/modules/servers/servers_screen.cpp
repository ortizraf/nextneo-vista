// Servers screen: status of the Beszel-monitored machines (homelab + proxy).
// Data: Beszel hub REST API (PocketBase) over plain HTTP on the LAN.
//   POST /api/collections/users/auth-with-password  -> token
//   GET  /api/collections/systems/records/<id>       -> status + info (cpu, mp, dp, u, dt, la...)
//   GET  /api/collections/system_stats/records       -> last 60 x "1m" samples for the charts
// Network work runs on the worker task (serversFetch); everything else on the LVGL thread.
#include "app_screens.h"
#include "core/screen_registry.h"
#include "modules/servers/servers.h"
#include "modules/servers/servers_internal.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
#include "services/app_log.h"

using namespace servers;

// ------------------------------------------------------------------ UI
static lv_obj_t *serversView;
static lv_obj_t *overview, *detailBox, *statusLbl;

static constexpr uint32_t C_BG = 0x07090C, C_CARD = 0x11161D, C_LINE = 0x2A3240, C_MUTED = 0x8A96A8,
                          C_TEXT = 0xE8EEF6, C_GREEN = 0x34C77B, C_BLUE = 0x4A9BEA, C_PURPLE = 0xC58AF9,
                          C_ORANGE = 0xF2B33D, C_RED = 0xF0605A;

struct CardUi {
  lv_obj_t *card, *dot, *name, *role, *state, *arc[3], *arcVal[3], *chart;
  lv_obj_t *footCap[3], *footVal[3];
  lv_chart_series_t *sCpu, *sRam;
};
static CardUi cards[SERVERS_MAX];

struct MiniChart { lv_obj_t *box, *title, *value, *chart, *yTop, *yMid, *yBot; lv_chart_series_t *ser; };
static MiniChart mc[4];          // CPU, RAM, Disco, Rede
static lv_obj_t *dTitle, *dSub;
static constexpr int INFO_N = 6;
static lv_obj_t *dInfoVal[INFO_N];
static int8_t detailIndex = -1;
static uint32_t renderedVersion = UINT32_MAX;
static uint32_t lastAgeRender = 0;

static uint32_t levelColor(float v) { return v >= 85 ? C_RED : v >= 60 ? C_ORANGE : C_GREEN; }
static uint32_t tempColor(float t) { return t >= 80 ? C_RED : t >= 65 ? C_ORANGE : C_TEXT; }

static void fmtUptime(uint32_t s, char *b, size_t n) {
  if (s >= 86400) snprintf(b, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
  else snprintf(b, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
}
static void fmtAge(uint32_t epoch, char *b, size_t n) {
  time_t now = time(nullptr);
  if (!epoch || now < 1700000000) { snprintf(b, n, "--"); return; }
  long a = (long)now - (long)epoch; if (a < 0) a = 0;
  if (a < 90) snprintf(b, n, "há %lds", a);
  else if (a < 5400) snprintf(b, n, "há %ldmin", a / 60);
  else snprintf(b, n, "há %ldh", a / 3600);
}
// bytes/s -> "0", "850B", "12K", "1.2M"  (compact, for the card footer)
static void fmtRate(float v, char *b, size_t n, bool unit = false) {
  const char *sfx = unit ? "/s" : "";
  if (v < 1) snprintf(b, n, "0%s", unit ? " B/s" : "");
  else if (v < 1024) snprintf(b, n, "%.0fB%s", v, sfx);
  else if (v < 1048576) snprintf(b, n, "%.0fK%s", v / 1024, sfx);
  else if (v < 10485760) snprintf(b, n, "%.1fM%s", v / 1048576, sfx);
  else snprintf(b, n, "%.0fM%s", v / 1048576, sfx);
}
// rounds a network peak up to 1/2/5 x 10^n bytes/s so the scale reads nicely
static float niceMax(float v) {
  if (v < 1024) return 1024;
  float p = 1; while (p * 10 <= v) p *= 10;
  for (float m : {1.0f, 2.0f, 5.0f, 10.0f}) if (m * p >= v) return m * p;
  return v;
}

static lv_obj_t *plainBox(lv_obj_t *p) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  return o;
}
static lv_obj_t *cardBox(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_set_size(c, w, h); lv_obj_set_pos(c, x, y);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0); lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 16, 0); lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(c, 0, 0); lv_obj_set_scrollable(c, false);
  return c;
}
static lv_obj_t *makeChart(lv_obj_t *p, int w, int h, int hDiv) {
  lv_obj_t *ch = lv_chart_create(p);
  lv_obj_set_size(ch, w, h);
  lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(ch, HIST);
  lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(ch, hDiv, 0);
  lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(ch, 0, 0);
  lv_obj_set_style_radius(ch, 0, 0); lv_obj_set_style_pad_all(ch, 0, 0);
  lv_obj_set_style_line_color(ch, lv_color_hex(0x232B36), LV_PART_MAIN);
  lv_obj_set_style_line_dash_width(ch, 2, LV_PART_MAIN); lv_obj_set_style_line_dash_gap(ch, 3, LV_PART_MAIN);
  lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR); lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
  lv_obj_set_clickable(ch, false);
  return ch;
}
static void fillSeries(lv_obj_t *ch, lv_chart_series_t *s, const float *v, int n, float scale = 1) {
  lv_chart_set_all_values(ch, s, LV_CHART_POINT_NONE);
  const int off = HIST - n;
  for (int k = 0; k < n; ++k) lv_chart_set_value_by_id(ch, s, off + k, (int32_t)lroundf(v[k] * scale));
  lv_chart_refresh(ch);
}

static void openDetail(lv_event_t *e) {
  detailIndex = (int8_t)(intptr_t)lv_event_get_user_data(e);
  lv_obj_set_hidden(overview, true);
  lv_obj_set_hidden(detailBox, false);
  renderedVersion = UINT32_MAX;
}
static void closeDetail(lv_event_t *) {
  detailIndex = -1;
  lv_obj_set_hidden(detailBox, true);
  lv_obj_set_hidden(overview, false);
  renderedVersion = UINT32_MAX;
}

// ---- overview card (228 x 268)
static void buildCard(int i, int x) {
  CardUi &u = cards[i];
  lv_obj_t *c = u.card = cardBox(overview, x, 0, 228, 268);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_set_clickable(c, true);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x18202A), LV_STATE_PRESSED);
  lv_obj_add_event_cb(c, openDetail, LV_EVENT_CLICKED, (void *)(intptr_t)i);

  u.dot = lv_obj_create(c);
  lv_obj_set_size(u.dot, 9, 9); lv_obj_set_pos(u.dot, 14, 19);
  lv_obj_set_style_radius(u.dot, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(u.dot, 0, 0);
  lv_obj_set_style_bg_color(u.dot, lv_color_hex(0x6B7685), 0); lv_obj_set_clickable(u.dot, false);
  u.name = makeLabel(c, gCfg[i].label, &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(u.name, 30, 10); lv_obj_set_width(u.name, 186); lv_label_set_long_mode(u.name, LV_LABEL_LONG_DOT);
  u.role = makeLabel(c, DEF_ROLE[i], &lv_font_weather_14, C_MUTED); lv_obj_set_pos(u.role, 30, 36);
  u.state = makeLabel(c, "", &lv_font_weather_14, C_MUTED);
  lv_obj_set_width(u.state, 110); lv_obj_set_pos(u.state, 104, 36);
  lv_obj_set_style_text_align(u.state, LV_TEXT_ALIGN_RIGHT, 0);

  static const char *caps[3] = {"CPU", "RAM", "Disco"};
  for (int k = 0; k < 3; ++k) {
    lv_obj_t *a = u.arc[k] = lv_arc_create(c);
    lv_obj_set_size(a, 64, 64); lv_obj_set_pos(a, 12 + k * 70, 62);
    lv_arc_set_rotation(a, 135); lv_arc_set_bg_angles(a, 0, 270); lv_arc_set_range(a, 0, 100); lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB); lv_obj_set_clickable(a, false);
    lv_obj_set_style_arc_width(a, 7, LV_PART_MAIN); lv_obj_set_style_arc_width(a, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x232B36), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    u.arcVal[k] = makeLabel(a, "--", &lv_font_weather_20, 0xFFFFFF); lv_obj_align(u.arcVal[k], LV_ALIGN_CENTER, 0, 2);
    lv_obj_t *cap = makeLabel(c, caps[k], &lv_font_weather_14, C_MUTED);
    lv_obj_set_width(cap, 64); lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(cap, 12 + k * 70, 122);
  }

  // sparkline: legend row + chart
  lv_obj_set_pos(makeLabel(c, "CPU", &lv_font_weather_14, C_GREEN), 14, 146);
  lv_obj_set_pos(makeLabel(c, "RAM", &lv_font_weather_14, C_BLUE), 50, 146);
  lv_obj_t *h1 = makeLabel(c, "1h", &lv_font_weather_14, 0x5C6676); lv_obj_align(h1, LV_ALIGN_TOP_RIGHT, -14, 146);
  u.chart = makeChart(c, 200, 40, 2); lv_obj_set_pos(u.chart, 14, 166);
  u.sCpu = lv_chart_add_series(u.chart, lv_color_hex(C_GREEN), LV_CHART_AXIS_PRIMARY_Y);
  u.sRam = lv_chart_add_series(u.chart, lv_color_hex(C_BLUE), LV_CHART_AXIS_PRIMARY_Y);

  // footer: 3 stat columns (caption over value) separated from the chart by a hairline
  lv_obj_t *sep = lv_obj_create(c);
  lv_obj_set_size(sep, 200, 1); lv_obj_set_pos(sep, 14, 214);
  lv_obj_set_style_bg_color(sep, lv_color_hex(C_LINE), 0); lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_clickable(sep, false);
  static const char *fcap[3] = {"TEMP", "REDE", "UPTIME"};
  static const int fx[3] = {14, 70, 150}, fw[3] = {54, 78, 66};
  for (int k = 0; k < 3; ++k) {
    u.footCap[k] = makeLabel(c, fcap[k], &lv_font_weather_14, 0x5C6676);
    lv_obj_set_pos(u.footCap[k], fx[k], 220);
    u.footVal[k] = makeLabel(c, "--", &lv_font_weather_14, C_TEXT);
    lv_obj_set_pos(u.footVal[k], fx[k], 240); lv_obj_set_width(u.footVal[k], fw[k]);
    lv_label_set_long_mode(u.footVal[k], LV_LABEL_LONG_CLIP);
  }
  lv_obj_set_style_text_font(u.footVal[0], &lv_font_weather_20, 0);
  lv_obj_set_pos(u.footVal[0], fx[0], 236);
}

// ---- detail: 2x2 mini charts with Y scale + info column
static void buildMini(int k, int x, int y, const char *title, uint32_t color) {
  MiniChart &m = mc[k];
  m.box = cardBox(detailBox, x, y, 166, 112);
  m.title = makeLabel(m.box, title, &lv_font_weather_14, color); lv_obj_set_pos(m.title, 10, 6);
  m.value = makeLabel(m.box, "--", &lv_font_weather_14, 0xFFFFFF);
  lv_obj_set_width(m.value, 90); lv_obj_set_pos(m.value, 66, 6);
  lv_obj_set_style_text_align(m.value, LV_TEXT_ALIGN_RIGHT, 0);
  m.chart = makeChart(m.box, 120, 60, 3); lv_obj_set_pos(m.chart, 38, 30);
  m.ser = lv_chart_add_series(m.chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_set_style_bg_opa(m.chart, LV_OPA_20, LV_PART_ITEMS);   // soft fill under the line
  lv_obj_t **ax[3] = {&m.yTop, &m.yMid, &m.yBot};
  static const int ay[3] = {24, 52, 80};
  for (int j = 0; j < 3; ++j) {
    *ax[j] = makeLabel(m.box, j == 0 ? "100" : j == 1 ? "50" : "0", &lv_font_weather_14, 0x5C6676);
    lv_obj_set_width(*ax[j], 32); lv_obj_set_pos(*ax[j], 2, ay[j]);
    lv_obj_set_style_text_align(*ax[j], LV_TEXT_ALIGN_RIGHT, 0);
  }
  lv_obj_set_pos(makeLabel(m.box, "-60m", &lv_font_weather_14, 0x5C6676), 38, 92);
  lv_obj_t *now = makeLabel(m.box, "agora", &lv_font_weather_14, 0x5C6676);
  lv_obj_align(now, LV_ALIGN_TOP_RIGHT, -8, 92);
}

static void buildServersScreen(lv_obj_t *scr) {
  serversView = lv_obj_create(scr);
  lv_obj_set_size(serversView, SCR_W, SCR_H); lv_obj_set_pos(serversView, 0, 0);
  lv_obj_set_hidden(serversView, true);
  lv_obj_set_style_bg_color(serversView, lv_color_hex(C_BG), 0); lv_obj_set_style_bg_opa(serversView, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(serversView, 0, 0); lv_obj_set_style_pad_all(serversView, 0, 0);
  lv_obj_set_scrollable(serversView, false);
  addBackButton(serversView);
  lv_obj_t *t = makeLabel(serversView, "Servidores", &lv_font_weather_20, 0xFFFFFF); lv_obj_align(t, LV_ALIGN_TOP_LEFT, 52, 6);
  statusLbl = makeLabel(serversView, "conectando...", &lv_font_weather_14, C_MUTED);
  lv_obj_set_width(statusLbl, 260); lv_obj_set_pos(statusLbl, 212, 10);
  lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_RIGHT, 0);

  overview = plainBox(serversView); lv_obj_set_size(overview, SCR_W, 270); lv_obj_set_pos(overview, 0, 46);
  buildCard(0, 8); buildCard(1, 244);

  detailBox = plainBox(serversView); lv_obj_set_size(detailBox, SCR_W, 280); lv_obj_set_pos(detailBox, 0, 38);
  lv_obj_set_hidden(detailBox, true);
  dTitle = makeLabel(detailBox, "", &lv_font_weather_20, 0xFFFFFF); lv_obj_set_pos(dTitle, 10, 0);
  lv_obj_set_width(dTitle, 220); lv_label_set_long_mode(dTitle, LV_LABEL_LONG_DOT);
  dSub = makeLabel(detailBox, "", &lv_font_weather_14, C_MUTED); lv_obj_set_pos(dSub, 10, 26);
  lv_obj_set_width(dSub, 330); lv_label_set_long_mode(dSub, LV_LABEL_LONG_DOT);
  lv_obj_t *back = lv_button_create(detailBox);
  lv_obj_set_size(back, 120, 30); lv_obj_set_pos(back, 352, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x1C2430), 0); lv_obj_set_style_radius(back, 10, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_t *bl = makeLabel(back, LV_SYMBOL_LEFT " Visão geral", &lv_font_weather_14, 0xFFFFFF); lv_obj_center(bl);
  lv_obj_add_event_cb(back, closeDetail, LV_EVENT_CLICKED, NULL);

  buildMini(0, 8, 48, "CPU", C_GREEN);
  buildMini(1, 180, 48, "RAM", C_BLUE);
  buildMini(2, 8, 166, "Disco", C_ORANGE);
  buildMini(3, 180, 166, "Rede", C_PURPLE);

  // info column: 6 caption/value pairs, 37 px each -> fits the 230 px card
  lv_obj_t *info = cardBox(detailBox, 352, 48, 120, 230);
  static const char *caps[INFO_N] = {"STATUS", "TEMPERATURA", "UPTIME", "LOAD AVG", "REDE", "AGENTE"};
  for (int k = 0; k < INFO_N; ++k) {
    lv_obj_t *c = makeLabel(info, caps[k], &lv_font_weather_14, 0x5C6676);
    lv_obj_set_pos(c, 10, 6 + k * 37);
    dInfoVal[k] = makeLabel(info, "--", &lv_font_weather_14, C_TEXT);
    lv_obj_set_pos(dInfoVal[k], 10, 23 + k * 37); lv_obj_set_width(dInfoVal[k], 104);
    lv_label_set_long_mode(dInfoVal[k], LV_LABEL_LONG_DOT);
  }
}

// Setting a style always invalidates the object, even with the same value: keep
// what each card shows and only touch what changed (cards redrew 70-350 ms).
static int cardState[SERVERS_MAX] = {-1, -1};
static uint32_t cardHistMs[SERVERS_MAX] = {UINT32_MAX, UINT32_MAX}, arcColor[SERVERS_MAX][3], footColor[SERVERS_MAX];
static uint32_t detailHistMs = UINT32_MAX; static int detailShown = -1;
static void setArcColor(int i, int k, uint32_t c) {
  if (arcColor[i][k] == c) return;
  arcColor[i][k] = c;
  lv_obj_set_style_arc_color(cards[i].arc[k], lv_color_hex(c), LV_PART_INDICATOR);
}

static void renderCard(int i, const ServerData &d, const ServerCfg &c) {
  CardUi &u = cards[i];
  char b[48];
  setLabelIfChanged(u.name, d.name[0] ? d.name : c.label);
  const bool live = d.reachable && d.up;
  const int state = !d.known ? 0 : live ? 1 : 2;
  const bool stateChanged = state != cardState[i];
  if (stateChanged) {
    cardState[i] = state;
    lv_obj_set_style_bg_color(u.dot, lv_color_hex(!d.known ? 0x6B7685 : live ? C_GREEN : C_RED), 0);
    lv_obj_set_style_border_color(u.card, lv_color_hex(d.known && !live ? 0x7A2E2E : C_LINE), 0);
  }
  if (!d.known) setLabelIfChanged(u.state, "...");
  else if (!d.reachable) setLabelIfChanged(u.state, "sem dados");
  else if (!d.up) setLabelIfChanged(u.state, "OFFLINE");
  else { fmtAge(d.updatedEpoch, b, sizeof b); setLabelIfChanged(u.state, b); }
  if (stateChanged) lv_obj_set_style_text_color(u.state, lv_color_hex(d.known && !live ? C_RED : C_MUTED), 0);

  const float vals[3] = {d.cpu, d.ram, d.disk};
  for (int k = 0; k < 3; ++k) {
    lv_arc_set_value(u.arc[k], live ? (int)lroundf(vals[k]) : 0);
    setArcColor(i, k, live ? levelColor(vals[k]) : 0x3A4250);
    if (d.reachable) snprintf(b, sizeof b, "%.0f%%", vals[k]); else strcpy(b, "--");
    setLabelIfChanged(u.arcVal[k], b);
    if (stateChanged) lv_obj_set_style_text_color(u.arcVal[k], lv_color_hex(live ? 0xFFFFFF : 0x6B7685), 0);
  }
  if (d.histMs != cardHistMs[i] || stateChanged) {          // history changes once a minute, not every poll
    cardHistMs[i] = d.histMs;
    fillSeries(u.chart, u.sCpu, d.hCpu, d.hCount);
    fillSeries(u.chart, u.sRam, d.hRam, d.hCount);
  }

  if (!d.reachable) {
    setLabelIfChanged(u.footVal[0], "--"); setLabelIfChanged(u.footVal[2], "--");
    setLabelIfChanged(u.footVal[1], d.known ? "erro" : "...");
    return;
  }
  if (d.temp >= 0) snprintf(b, sizeof b, "%.0f\xC2\xB0", d.temp); else strcpy(b, "--");
  setLabelIfChanged(u.footVal[0], b);
  const uint32_t fc = d.temp >= 0 ? tempColor(d.temp) : C_MUTED;
  if (fc != footColor[i]) { footColor[i] = fc; lv_obj_set_style_text_color(u.footVal[0], lv_color_hex(fc), 0); }
  char rx[12], tx[12];
  fmtRate(d.netIn, rx, sizeof rx); fmtRate(d.netOut, tx, sizeof tx);
  snprintf(b, sizeof b, LV_SYMBOL_DOWN "%s " LV_SYMBOL_UP "%s", rx, tx);
  setLabelIfChanged(u.footVal[1], b);
  fmtUptime(d.uptime, b, sizeof b);
  setLabelIfChanged(u.footVal[2], b);
}

static void renderDetail(const ServerData &d, const ServerCfg &c) {
  char b[200];
  setLabelIfChanged(dTitle, d.name[0] ? d.name : c.label);
  if (d.cpuModel[0]) snprintf(b, sizeof b, "%s%s%u cores", d.cpuModel, d.cores ? " · " : "", d.cores);
  else snprintf(b, sizeof b, "%s", d.reachable ? "" : d.error);
  if (!d.cores && d.cpuModel[0]) snprintf(b, sizeof b, "%s", d.cpuModel);
  setLabelIfChanged(dSub, b);

  const float cur[3] = {d.cpu, d.ram, d.disk};
  const float *hist[3] = {d.hCpu, d.hRam, d.hDisk};
  const bool histChanged = d.histMs != detailHistMs || detailShown != detailIndex;
  detailHistMs = d.histMs; detailShown = detailIndex;
  for (int k = 0; k < 3; ++k) {
    if (histChanged) fillSeries(mc[k].chart, mc[k].ser, hist[k], d.hCount);
    if (d.reachable) snprintf(b, sizeof b, "%.0f%%", cur[k]); else strcpy(b, "--");
    setLabelIfChanged(mc[k].value, b);
    lv_obj_set_style_text_color(mc[k].value, lv_color_hex(d.reachable ? levelColor(cur[k]) : C_MUTED), 0);
  }
  // network: auto scale (nice max) with the scale written on the Y axis
  float peak = 0;
  for (int k = 0; k < d.hCount; ++k) peak = max(peak, d.hNet[k]);
  const float top = niceMax(peak);
  if (histChanged) fillSeries(mc[3].chart, mc[3].ser, d.hNet, d.hCount, 100.0f / top);
  char t1[12], t2[12];
  fmtRate(top, t1, sizeof t1); fmtRate(top / 2, t2, sizeof t2);
  setLabelIfChanged(mc[3].yTop, t1); setLabelIfChanged(mc[3].yMid, t2);
  fmtRate(d.netIn + d.netOut, t1, sizeof t1, true);
  setLabelIfChanged(mc[3].value, d.reachable ? t1 : "--");

  char up[16], age[16], load[24] = "--", temp[12] = "--", rx[12], tx[12], net[28], agent[32];
  fmtUptime(d.uptime, up, sizeof up); fmtAge(d.updatedEpoch, age, sizeof age);
  if (d.load[0] >= 0) snprintf(load, sizeof load, "%.2f %.2f %.2f", d.load[0], d.load[1], d.load[2]);
  if (d.temp >= 0) snprintf(temp, sizeof temp, "%.0f\xC2\xB0" "C", d.temp);
  fmtRate(d.netIn, rx, sizeof rx); fmtRate(d.netOut, tx, sizeof tx);
  snprintf(net, sizeof net, LV_SYMBOL_DOWN "%s  " LV_SYMBOL_UP "%s", rx, tx);
  snprintf(agent, sizeof agent, "v%s · %s", d.agent[0] ? d.agent : "?", age);
  const char *vals[INFO_N] = {!d.reachable ? "sem dados" : d.up ? "online" : "OFFLINE", temp, up, load, net, agent};
  for (int k = 0; k < INFO_N; ++k) setLabelIfChanged(dInfoVal[k], d.reachable || k == 0 ? vals[k] : "--");
  lv_obj_set_style_text_color(dInfoVal[0], lv_color_hex(d.reachable && d.up ? C_GREEN : C_RED), 0);
  lv_obj_set_style_text_color(dInfoVal[1], lv_color_hex(d.temp >= 0 ? tempColor(d.temp) : C_MUTED), 0);
}

static void serversOnEnter() {
  if (detailIndex >= 0) closeDetail(nullptr);
  renderedVersion = UINT32_MAX;
  for (int i = 0; i < SERVERS_MAX; ++i) { cardState[i] = -1; cardHistMs[i] = UINT32_MAX; footColor[i] = UINT32_MAX; for (auto &a : arcColor[i]) a = UINT32_MAX; }
  detailHistMs = UINT32_MAX; detailShown = -1;
}

static void serversRender() {
  const uint32_t nowMs = millis();
  xSemaphoreTake(mtx, portMAX_DELAY);
  const uint32_t ver = gDataVersion;
  xSemaphoreGive(mtx);
  if (ver == renderedVersion) {                 // no new data: only refresh the "há Xs" labels
    if (nowMs - lastAgeRender < 5000 || detailIndex >= 0) return;
    lastAgeRender = nowMs;
    xSemaphoreTake(mtx, portMAX_DELAY);
    uint32_t ep[SERVERS_MAX]; bool live[SERVERS_MAX];
    for (int i = 0; i < SERVERS_MAX; ++i) { ep[i] = gData[i].updatedEpoch; live[i] = gData[i].reachable && gData[i].up; }
    xSemaphoreGive(mtx);
    char b[16];
    for (int i = 0; i < SERVERS_MAX; ++i) if (live[i]) { fmtAge(ep[i], b, sizeof b); setLabelIfChanged(cards[i].state, b); }
    return;
  }
  renderedVersion = ver; lastAgeRender = nowMs;
  static ServerData d[SERVERS_MAX]; static ServerCfg c[SERVERS_MAX];
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < SERVERS_MAX; ++i) { d[i] = gData[i]; c[i] = gCfg[i]; }
  xSemaphoreGive(mtx);
  int online = 0, known = 0;
  for (int i = 0; i < SERVERS_MAX; ++i) { if (d[i].known) known++; if (d[i].reachable && d[i].up) online++; }
  char b[48];
  if (!known) snprintf(b, sizeof b, "conectando...");
  else snprintf(b, sizeof b, "%d/%d online", online, SERVERS_MAX);
  setLabelIfChanged(statusLbl, b);
  lv_obj_set_style_text_color(statusLbl, lv_color_hex(!known ? C_MUTED : online == SERVERS_MAX ? C_GREEN : C_RED), 0);
  if (detailIndex >= 0) renderDetail(d[detailIndex], c[detailIndex]);
  else for (int i = 0; i < SERVERS_MAX; ++i) renderCard(i, d[i], c[i]);
}

static const NetJobDef kServersJobs[] = {{"servers", serversFetch, true}};
const ScreenLifecycle kServersScreen = {
  SCREEN_SERVERS, "servers", &serversView, buildServersScreen, serversOnEnter, nullptr, serversRender,
  nullptr, SERVERS_UPDATE_INTERVAL_MS, SERVERS_RETRY_INTERVAL_MS, kServersJobs, 1, nullptr,
};
