// Servers screen: status of the Beszel-monitored machines (homelab + proxy).
// Data: Beszel hub REST API (PocketBase) over plain HTTP on the LAN.
//   POST /api/collections/users/auth-with-password  -> token
//   GET  /api/collections/systems/records/<id>       -> status + info (cpu, mp, dp, u, dt, la...)
//   GET  /api/collections/system_stats/records       -> last 60 x "1m" samples for the charts
// Network work runs on the worker task (serversFetch); everything else on the LVGL thread.
#include "app_screens.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
#include "services/app_log.h"

static constexpr int HIST = 60;

struct ServerCfg { char label[24]; char url[64]; char id[24]; };
struct ServerData {
  bool known = false, reachable = false, up = false;
  char name[32] = "", host[32] = "", kernel[32] = "", cpuModel[48] = "", agent[12] = "", error[48] = "";
  uint8_t cores = 0;
  float cpu = 0, ram = 0, disk = 0, temp = -1, load[3] = {-1, -1, -1};
  float netIn = 0, netOut = 0;     // bytes/s
  uint32_t uptime = 0, updatedEpoch = 0;
  float hCpu[HIST], hRam[HIST], hDisk[HIST], hNet[HIST];
  uint8_t hCount = 0;
  uint32_t histMs = 0;             // millis() of the last history download
};

static ServerCfg gCfg[SERVERS_MAX];
static ServerData gData[SERVERS_MAX];
static char gUser[64] = "", gPass[64] = "";
static String gToken[SERVERS_MAX];
static uint32_t gDataVersion = 0, gCfgVersion = 0;

// ------------------------------------------------------------------ config
static const char *DEF_LABEL[SERVERS_MAX] = {"NextNeo Server", "Network Server"};
static const char *DEF_ROLE[SERVERS_MAX]  = {"homelab", "proxy"};
static const char *DEF_URL[SERVERS_MAX]   = {"http://nextneo.local:8090", "http://networkserver.local:8090"};
static const char *DEF_ID[SERVERS_MAX]    = {"naap07gfucfyv19", "utrz2p5k7xtymho"};

void loadServersConfig() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < SERVERS_MAX; ++i) {
    char k[12];
    snprintf(k, sizeof k, "bzLbl%d", i); snprintf(gCfg[i].label, sizeof gCfg[i].label, "%s", prefs.getString(k, DEF_LABEL[i]).c_str());
    snprintf(k, sizeof k, "bzUrl%d", i); snprintf(gCfg[i].url, sizeof gCfg[i].url, "%s", prefs.getString(k, DEF_URL[i]).c_str());
    snprintf(k, sizeof k, "bzId%d", i);  snprintf(gCfg[i].id, sizeof gCfg[i].id, "%s", prefs.getString(k, DEF_ID[i]).c_str());
  }
  snprintf(gUser, sizeof gUser, "%s", prefs.getString("bzUser", "").c_str());
  snprintf(gPass, sizeof gPass, "%s", prefs.getString("bzPass", "").c_str());
  gCfgVersion++;
  xSemaphoreGive(mtx);
}

// Accepts "http://host:8090/system/<id>" (link copied from Beszel) or just a base URL.
bool serversParseLink(const char *link, char *url, size_t urlSize, char *id, size_t idSize) {
  String s = link; s.trim();
  if (s.isEmpty()) return false;
  if (!s.startsWith("http://") && !s.startsWith("https://")) s = "http://" + s;
  int p = s.indexOf("/system/");
  if (p > 0) {
    String sid = s.substring(p + 8);
    int q = sid.indexOf('/'); if (q >= 0) sid = sid.substring(0, q);
    q = sid.indexOf('?'); if (q >= 0) sid = sid.substring(0, q);
    if (id && sid.length()) snprintf(id, idSize, "%s", sid.c_str());
    s = s.substring(0, p);
  }
  while (s.endsWith("/")) s.remove(s.length() - 1);
  snprintf(url, urlSize, "%s", s.c_str());
  return true;
}

void serversSetServer(uint8_t i, const char *label, const char *url, const char *id) {
  if (i >= SERVERS_MAX) return;
  char k[12];
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (label && *label) { snprintf(gCfg[i].label, sizeof gCfg[i].label, "%s", label); snprintf(k, sizeof k, "bzLbl%d", i); prefs.putString(k, gCfg[i].label); }
  if (url && *url)     { snprintf(gCfg[i].url, sizeof gCfg[i].url, "%s", url);       snprintf(k, sizeof k, "bzUrl%d", i); prefs.putString(k, gCfg[i].url); }
  if (id && *id)       { snprintf(gCfg[i].id, sizeof gCfg[i].id, "%s", id);          snprintf(k, sizeof k, "bzId%d", i);  prefs.putString(k, gCfg[i].id); }
  gToken[i] = ""; gData[i] = ServerData(); gCfgVersion++; gDataVersion++;
  xSemaphoreGive(mtx);
}

void serversSetCredentials(const char *user, const char *pass) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (user) { snprintf(gUser, sizeof gUser, "%s", user); prefs.putString("bzUser", gUser); }
  if (pass && *pass) { snprintf(gPass, sizeof gPass, "%s", pass); prefs.putString("bzPass", gPass); }
  for (int i = 0; i < SERVERS_MAX; ++i) gToken[i] = "";
  gCfgVersion++;
  xSemaphoreGive(mtx);
}

void serversGetConfig(uint8_t i, char *label, char *url, char *id, size_t n) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(label, n, "%s", gCfg[i].label); snprintf(url, n, "%s", gCfg[i].url); snprintf(id, n, "%s", gCfg[i].id);
  xSemaphoreGive(mtx);
}
String serversUser() { xSemaphoreTake(mtx, portMAX_DELAY); String u = gUser; xSemaphoreGive(mtx); return u; }
bool serversHasPassword() { return gPass[0] != 0; }

String serversStatusText(uint8_t i) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const ServerData d = gData[i];
  xSemaphoreGive(mtx);
  if (!d.known) return "aguardando consulta (abra a tela Servers)";
  if (!d.reachable) return String("erro: ") + d.error;
  return String(d.name) + (d.up ? " · online" : " · offline");
}

// true when any server looked offline/unreachable on the last check (menu badge)
bool serversAlert() {
  bool a = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < SERVERS_MAX; ++i) if (gData[i].known && (!gData[i].reachable || !gData[i].up)) a = true;
  xSemaphoreGive(mtx);
  return a;
}

// ------------------------------------------------------------------ network (worker task)
// "http://nextneo.local:8090" -> "http://192.168.x.y:8090" (lwIP does not always resolve .local)
struct MdnsCache { char host[48]; IPAddress ip; uint32_t atMs; };
static MdnsCache gMdns[SERVERS_MAX + 1];
static void forgetMdns() { for (auto &m : gMdns) m.host[0] = 0; }

static String resolveBase(const char *base) {
  static bool mdnsUp = false;
  String s = base;
  int hs = s.indexOf("://"); hs = hs < 0 ? 0 : hs + 3;
  int he = s.indexOf(':', hs); if (he < 0) he = s.indexOf('/', hs); if (he < 0) he = s.length();
  String host = s.substring(hs, he);
  if (!host.endsWith(".local")) return s;
  for (auto &m : gMdns)                         // cached for 5 min (queryHost can take ~2 s)
    if (!strcmp(m.host, host.c_str()) && millis() - m.atMs < 300000UL) return s.substring(0, hs) + m.ip.toString() + s.substring(he);
  if (!mdnsUp) mdnsUp = MDNS.begin("nextneo-vista");
  IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
  if (ip == IPAddress(0, 0, 0, 0)) return s;   // let HTTPClient try anyway
  MdnsCache *slot = &gMdns[0];
  for (auto &m : gMdns) { if (!strcmp(m.host, host.c_str()) || !m.host[0]) { slot = &m; break; } }
  snprintf(slot->host, sizeof slot->host, "%s", host.c_str()); slot->ip = ip; slot->atMs = millis();
  return s.substring(0, hs) + ip.toString() + s.substring(he);
}

static bool login(uint8_t i, const String &base, const char *user, const char *pass, char *err) {
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(base + "/api/collections/users/auth-with-password")) { strcpy(err, "URL inválida"); return false; }
  http.addHeader("Content-Type", "application/json");
  JsonDocument body; body["identity"] = user; body["password"] = pass;
  String payload; serializeJson(body, payload);
  const int code = http.POST(payload);
  if (code != 200) {
    String resp = code > 0 ? http.getString() : String();
    http.end();
    JsonDocument e; String msg;
    if (!deserializeJson(e, resp)) msg = e["message"] | "";
    Log.printf("Beszel login HTTP %d: %s\n", code, msg.c_str());
    if (code == 403) snprintf(err, 48, "login 403: usuário não verificado?");
    else snprintf(err, 48, code == 400 ? "e-mail ou senha incorretos" : "login HTTP %d", code);
    return false;
  }
  JsonDocument filter; filter["token"] = true;
  JsonDocument doc;
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  gToken[i] = doc["token"] | "";
  if (gToken[i].isEmpty()) { strcpy(err, "token vazio"); return false; }
  return true;
}

static int getJson(const String &url, const String &token, JsonDocument &doc, JsonDocument &filter) {
  HTTPClient http;
  http.useHTTP10(true);          // no chunked encoding -> safe to parse straight from the stream
  http.setTimeout(8000);
  if (!http.begin(url)) return -100;
  http.addHeader("Authorization", token);
  const int code = http.GET();
  if (code == 200) {
    if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) { http.end(); return -101; }
  }
  http.end();
  return code;
}

// "2026-11-10 23:58:01.123Z" -> epoch (UTC)
static uint32_t parsePbDate(const char *s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  y -= mo <= 2;
  const int era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + doe - 719468L;
  return (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

// prev = last good data of this server: its history is reused when it is still fresh
// (Beszel only writes a new 1-minute sample every 60 s, status polls run every 10 s).
static bool fetchOne(uint8_t i, const ServerCfg &c, const char *user, const char *pass, ServerData &out, const ServerData &prev) {
  out = ServerData(); out.known = true;
  if (!user[0] || !pass[0]) { strcpy(out.error, "configure usuário/senha"); return false; }
  const String base = resolveBase(c.url);
  if (gToken[i].isEmpty() && !login(i, base, user, pass, out.error)) return false;
  if (!screenJobStillCurrent()) return false;

  // ---- current status
  JsonDocument f1;
  f1["name"] = true; f1["status"] = true; f1["updated"] = true; f1["info"] = true;
  JsonDocument doc;
  String url = base + "/api/collections/systems/records/" + c.id;
  int code = getJson(url, gToken[i], doc, f1);
  if (code == 401 || code == 403) {                 // token expired: log in again once
    gToken[i] = "";
    if (!login(i, base, user, pass, out.error)) return false;
    code = getJson(url, gToken[i], doc, f1);
  }
  if (code != 200) {
    snprintf(out.error, sizeof out.error, code == 404 ? "sistema não encontrado (ID/permissão)" :
             code < 0 ? "sem conexão com o hub" : "HTTP %d", code);
    if (code < 0) forgetMdns();                     // IP may have changed: resolve again next time
    return false;
  }
  out.reachable = true;
  snprintf(out.name, sizeof out.name, "%s", doc["name"] | c.label);
  out.up = strcmp(doc["status"] | "", "up") == 0;
  out.updatedEpoch = parsePbDate(doc["updated"] | "");
  JsonObject in = doc["info"];
  out.cpu = in["cpu"] | 0.0f; out.ram = in["mp"] | 0.0f; out.disk = in["dp"] | 0.0f;
  out.uptime = in["u"] | 0; out.temp = in["dt"] | -1.0f; out.cores = in["c"] | 0;
  snprintf(out.host, sizeof out.host, "%s", in["h"] | "");
  snprintf(out.kernel, sizeof out.kernel, "%s", in["k"] | "");
  snprintf(out.cpuModel, sizeof out.cpuModel, "%s", in["m"] | "");
  snprintf(out.agent, sizeof out.agent, "%s", in["v"] | "");
  if (in["la"].is<JsonArray>()) for (int k = 0; k < 3; ++k) out.load[k] = in["la"][k] | -1.0f;
  else { out.load[0] = in["l1"] | -1.0f; out.load[1] = in["l5"] | -1.0f; out.load[2] = in["l15"] | -1.0f; }
  if (!screenJobStillCurrent()) return false;

  // ---- last hour (1-minute samples, newest first) -- at most once per 55 s
  if (prev.hCount && millis() - prev.histMs < 55000UL) {
    memcpy(out.hCpu, prev.hCpu, sizeof out.hCpu); memcpy(out.hRam, prev.hRam, sizeof out.hRam);
    memcpy(out.hDisk, prev.hDisk, sizeof out.hDisk); memcpy(out.hNet, prev.hNet, sizeof out.hNet);
    out.hCount = prev.hCount; out.histMs = prev.histMs; out.netIn = prev.netIn; out.netOut = prev.netOut;
    return true;
  }
  JsonDocument f2;
  JsonObject fs = f2["items"][0]["stats"].to<JsonObject>();
  fs["cpu"] = true; fs["mp"] = true; fs["dp"] = true; fs["nr"] = true; fs["ns"] = true; fs["b"] = true;
  JsonDocument hist;
  url = base + "/api/collections/system_stats/records?perPage=" + HIST + "&sort=-created&skipTotal=1"
        "&fields=stats&filter=system%3D%27" + c.id + "%27%26%26type%3D%271m%27";
  if (getJson(url, gToken[i], hist, f2) == 200) {
    JsonArray items = hist["items"];
    const int n = min((int)items.size(), HIST);
    for (int k = 0; k < n; ++k) {
      JsonObject st = items[n - 1 - k]["stats"];       // oldest -> newest
      float rx, tx;                                    // bytes/s
      if (st["b"].is<JsonArray>()) { tx = st["b"][0] | 0.0f; rx = st["b"][1] | 0.0f; }
      else { rx = (st["nr"] | 0.0f) * 1048576.0f; tx = (st["ns"] | 0.0f) * 1048576.0f; }
      out.hCpu[k] = st["cpu"] | 0.0f; out.hRam[k] = st["mp"] | 0.0f;
      out.hDisk[k] = st["dp"] | out.disk; out.hNet[k] = rx + tx;
      if (k == n - 1) { out.netIn = rx; out.netOut = tx; }
    }
    out.hCount = n; out.histMs = millis();
  }
  return true;
}

bool serversFetch() {
  if (WiFi.status() != WL_CONNECTED) return false;
  ServerCfg cfgCopy[SERVERS_MAX]; char user[64], pass[64];
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy(cfgCopy, gCfg, sizeof cfgCopy); memcpy(user, gUser, sizeof user); memcpy(pass, gPass, sizeof pass);
  const uint32_t ver = gCfgVersion;
  xSemaphoreGive(mtx);
  bool any = false;
  static ServerData tmp, prev;
  for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
    if (!screenJobStillCurrent()) return false;
    xSemaphoreTake(mtx, portMAX_DELAY); prev = gData[i]; xSemaphoreGive(mtx);
    const bool ok = fetchOne(i, cfgCopy[i], user, pass, tmp, prev);
    if (!ok) Log.printf("Servers %s: %s\n", cfgCopy[i].label, tmp.error);
    any |= ok;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (ver == gCfgVersion) { gData[i] = tmp; gDataVersion++; }
    xSemaphoreGive(mtx);
  }
  return any;
}

// ------------------------------------------------------------------ UI
lv_obj_t *serversView;
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
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
static lv_obj_t *cardBox(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_set_size(c, w, h); lv_obj_set_pos(c, x, y);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0); lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 16, 0); lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(c, 0, 0); lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
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
  lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
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
  lv_obj_add_flag(overview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(detailBox, LV_OBJ_FLAG_HIDDEN);
  renderedVersion = UINT32_MAX;
}
static void closeDetail(lv_event_t *) {
  detailIndex = -1;
  lv_obj_add_flag(detailBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(overview, LV_OBJ_FLAG_HIDDEN);
  renderedVersion = UINT32_MAX;
}

// ---- overview card (228 x 268)
static void buildCard(int i, int x) {
  CardUi &u = cards[i];
  lv_obj_t *c = u.card = cardBox(overview, x, 0, 228, 268);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x18202A), LV_STATE_PRESSED);
  lv_obj_add_event_cb(c, openDetail, LV_EVENT_CLICKED, (void *)(intptr_t)i);

  u.dot = lv_obj_create(c);
  lv_obj_set_size(u.dot, 9, 9); lv_obj_set_pos(u.dot, 14, 19);
  lv_obj_set_style_radius(u.dot, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(u.dot, 0, 0);
  lv_obj_set_style_bg_color(u.dot, lv_color_hex(0x6B7685), 0); lv_obj_remove_flag(u.dot, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_remove_style(a, NULL, LV_PART_KNOB); lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
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
  lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
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

void buildServersScreen(lv_obj_t *scr) {
  serversView = lv_obj_create(scr);
  lv_obj_set_size(serversView, SCR_W, SCR_H); lv_obj_set_pos(serversView, 0, 0);
  lv_obj_add_flag(serversView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(serversView, lv_color_hex(C_BG), 0); lv_obj_set_style_bg_opa(serversView, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(serversView, 0, 0); lv_obj_set_style_pad_all(serversView, 0, 0);
  lv_obj_remove_flag(serversView, LV_OBJ_FLAG_SCROLLABLE);
  addBackButton(serversView);
  lv_obj_t *t = makeLabel(serversView, "Servidores", &lv_font_weather_20, 0xFFFFFF); lv_obj_align(t, LV_ALIGN_TOP_LEFT, 52, 6);
  statusLbl = makeLabel(serversView, "conectando...", &lv_font_weather_14, C_MUTED);
  lv_obj_set_width(statusLbl, 260); lv_obj_set_pos(statusLbl, 212, 10);
  lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_RIGHT, 0);

  overview = plainBox(serversView); lv_obj_set_size(overview, SCR_W, 270); lv_obj_set_pos(overview, 0, 46);
  buildCard(0, 8); buildCard(1, 244);

  detailBox = plainBox(serversView); lv_obj_set_size(detailBox, SCR_W, 280); lv_obj_set_pos(detailBox, 0, 38);
  lv_obj_add_flag(detailBox, LV_OBJ_FLAG_HIDDEN);
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

void serversOnEnter() {
  if (detailIndex >= 0) closeDetail(nullptr);
  renderedVersion = UINT32_MAX;
  for (int i = 0; i < SERVERS_MAX; ++i) { cardState[i] = -1; cardHistMs[i] = UINT32_MAX; footColor[i] = UINT32_MAX; for (auto &a : arcColor[i]) a = UINT32_MAX; }
  detailHistMs = UINT32_MAX; detailShown = -1;
}

void serversRender() {
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
