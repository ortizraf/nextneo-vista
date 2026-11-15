// Servers data side: status of the Beszel-monitored machines (homelab + proxy).
// Data: Beszel hub REST API (PocketBase) over plain HTTP on the LAN.
//   POST /api/collections/users/auth-with-password  -> token
//   GET  /api/collections/systems/records/<id>       -> status + info (cpu, mp, dp, u, dt, la...)
//   GET  /api/collections/system_stats/records       -> last 60 x "1m" samples for the charts
// Network work runs on the worker task (serversFetch); everything else on the LVGL thread.
#include "app_screens.h"
#include "modules/servers/servers_internal.h"
#include "modules/servers/servers_retry.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
#include "services/app_log.h"

namespace servers {

SemaphoreHandle_t srvMtx = xSemaphoreCreateMutex();

ServerCfg gCfg[SERVERS_MAX];
ServerData gData[SERVERS_MAX];
static char gUser[64] = "", gPass[64] = "";
static String gToken[SERVERS_MAX];
uint32_t gDataVersion = 0;
static uint32_t gCfgVersion = 0;
static constexpr uint32_t SERVER_RETRY_MS = 60000;   // a failed server is retried this often
static ServerRetry gRetry(SERVERS_MAX, SERVER_RETRY_MS);

const char *const DEF_ROLE[SERVERS_MAX]  = {"homelab", "proxy"};

}  // namespace servers
using namespace servers;

// ------------------------------------------------------------------ config
static const char *DEF_LABEL[SERVERS_MAX] = {"NextNeo Server", "Network Server"};
static const char *DEF_URL[SERVERS_MAX]   = {"http://nextneo.local:8090", "http://networkserver.local:8090"};
static const char *DEF_ID[SERVERS_MAX]    = {"naap07gfucfyv19", "utrz2p5k7xtymho"};

void loadServersConfig() {
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  for (int i = 0; i < SERVERS_MAX; ++i) {
    char k[12];
    snprintf(k, sizeof k, "bzLbl%d", i); snprintf(gCfg[i].label, sizeof gCfg[i].label, "%s", prefs.getString(k, DEF_LABEL[i]).c_str());
    snprintf(k, sizeof k, "bzUrl%d", i); snprintf(gCfg[i].url, sizeof gCfg[i].url, "%s", prefs.getString(k, DEF_URL[i]).c_str());
    snprintf(k, sizeof k, "bzId%d", i);  snprintf(gCfg[i].id, sizeof gCfg[i].id, "%s", prefs.getString(k, DEF_ID[i]).c_str());
  }
  snprintf(gUser, sizeof gUser, "%s", prefs.getString("bzUser", "").c_str());
  snprintf(gPass, sizeof gPass, "%s", prefs.getString("bzPass", "").c_str());
  gCfgVersion++;
  xSemaphoreGive(srvMtx);
}

// Accepts "http://host:8090/system/<id>" (link copied from Beszel) or just a base URL.

void serversSetServer(uint8_t i, const char *label, const char *url, const char *id) {
  if (i >= SERVERS_MAX) return;
  char k[12];
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  if (label && *label) { snprintf(gCfg[i].label, sizeof gCfg[i].label, "%s", label); snprintf(k, sizeof k, "bzLbl%d", i); prefs.putString(k, gCfg[i].label); }
  if (url && *url)     { snprintf(gCfg[i].url, sizeof gCfg[i].url, "%s", url);       snprintf(k, sizeof k, "bzUrl%d", i); prefs.putString(k, gCfg[i].url); }
  if (id && *id)       { snprintf(gCfg[i].id, sizeof gCfg[i].id, "%s", id);          snprintf(k, sizeof k, "bzId%d", i);  prefs.putString(k, gCfg[i].id); }
  gToken[i] = ""; gData[i] = ServerData(); gRetry.reset(i); gCfgVersion++; gDataVersion++;
  xSemaphoreGive(srvMtx);
}

void serversSetCredentials(const char *user, const char *pass) {
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  if (user) { snprintf(gUser, sizeof gUser, "%s", user); prefs.putString("bzUser", gUser); }
  if (pass && *pass) { snprintf(gPass, sizeof gPass, "%s", pass); prefs.putString("bzPass", gPass); }
  for (int i = 0; i < SERVERS_MAX; ++i) gToken[i] = "";
  gCfgVersion++;
  xSemaphoreGive(srvMtx);
}

void serversGetConfig(uint8_t i, char *label, char *url, char *id, size_t n) {
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  snprintf(label, n, "%s", gCfg[i].label); snprintf(url, n, "%s", gCfg[i].url); snprintf(id, n, "%s", gCfg[i].id);
  xSemaphoreGive(srvMtx);
}
String serversUser() { xSemaphoreTake(srvMtx, portMAX_DELAY); String u = gUser; xSemaphoreGive(srvMtx); return u; }
bool serversHasPassword() { return gPass[0] != 0; }

String serversStatusText(uint8_t i) {
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  const ServerData d = gData[i];
  xSemaphoreGive(srvMtx);
  if (!d.known) return T("aguardando consulta (abra a tela Servers)", "waiting for a check (open the Servers screen)");
  if (!d.reachable) return String(T("erro: ", "error: ")) + d.error;
  return String(d.name) + (d.up ? " · online" : " · offline");
}

// true when any server looked offline/unreachable on the last check (menu badge)
bool serversAlert() {
  bool a = false;
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  for (int i = 0; i < SERVERS_MAX; ++i) if (gData[i].known && (!gData[i].reachable || !gData[i].up)) a = true;
  xSemaphoreGive(srvMtx);
  return a;
}

// ------------------------------------------------------------------ network (worker task)
// "http://nextneo.local:8090" -> "http://192.168.x.y:8090" (lwIP does not always resolve .local)
struct MdnsCache { char host[48]; IPAddress ip; uint32_t atMs; };
static MdnsCache gMdns[SERVERS_MAX + 1];

// One server must never hold the others up (one offline machine used to block the
// whole cycle for ~15 s, so the other server showed nothing):
//  - short timeouts, and an unresolvable .local name fails at once;
//  - a server that failed is retried only every SERVER_RETRY_MS, after the healthy ones
//    (ServerRetry, servers_retry.cpp).

static constexpr uint16_t CONNECT_TIMEOUT_MS = 2500;

static void forgetMdns(const char *base) {          // only this server's host
  for (auto &m : gMdns) if (m.host[0] && strstr(base, m.host)) m.host[0] = 0;
}

// "" when a .local host cannot be resolved (do not hand it to the unicast DNS: slow).
static String resolveBase(const char *base) {
  String s = base;
  int hs = s.indexOf("://"); hs = hs < 0 ? 0 : hs + 3;
  int he = s.indexOf(':', hs); if (he < 0) he = s.indexOf('/', hs); if (he < 0) he = s.length();
  String host = s.substring(hs, he);
  if (!host.endsWith(".local")) return s;
  for (auto &m : gMdns)                         // cached for 5 min (queryHost can take ~2 s)
    if (!strcmp(m.host, host.c_str()) && millis() - m.atMs < 300000UL) return s.substring(0, hs) + m.ip.toString() + s.substring(he);
  // mDNS itself is started once at boot (core/web_config.cpp, hostname DEVICE_HOSTNAME).
  IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 1500);
  if (ip == IPAddress(0, 0, 0, 0)) return String();
  MdnsCache *slot = &gMdns[0];
  for (auto &m : gMdns) { if (!strcmp(m.host, host.c_str()) || !m.host[0]) { slot = &m; break; } }
  snprintf(slot->host, sizeof slot->host, "%s", host.c_str()); slot->ip = ip; slot->atMs = millis();
  return s.substring(0, hs) + ip.toString() + s.substring(he);
}

static bool login(uint8_t i, const String &base, const char *user, const char *pass, char *err) {
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(6000);
  if (!http.begin(base + "/api/collections/users/auth-with-password")) { strcpy(err, T("URL inválida", "invalid URL")); return false; }
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
    if (code == 403) snprintf(err, 48, T("login 403: usuário não verificado?", "login 403: user not verified?"));
    else if (code < 0) snprintf(err, 48, T("sem conexão com o hub", "no connection to the hub"));
    else snprintf(err, 48, code == 400 ? T("e-mail ou senha incorretos", "wrong e-mail or password") : "login HTTP %d", code);
    return false;
  }
  JsonDocument filter; filter["token"] = true;
  JsonDocument doc;
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  gToken[i] = doc["token"] | "";
  if (gToken[i].isEmpty()) { strcpy(err, T("token vazio", "empty token")); return false; }
  return true;
}

static int getJson(const String &url, const String &token, JsonDocument &doc, JsonDocument &filter) {
  HTTPClient http;
  http.useHTTP10(true);          // no chunked encoding -> safe to parse straight from the stream
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
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
  if (!user[0] || !pass[0]) { strcpy(out.error, T("configure usuário/senha", "set user/password")); return false; }
  const String base = resolveBase(c.url);
  if (base.isEmpty()) { strcpy(out.error, T("nome .local não encontrado", ".local name not found")); return false; }
  if (gToken[i].isEmpty() && !login(i, base, user, pass, out.error)) { forgetMdns(c.url); return false; }
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
    snprintf(out.error, sizeof out.error, code == 404 ? T("sistema não encontrado (ID/permissão)", "system not found (ID/permission)") :
             code < 0 ? T("sem conexão com o hub", "no connection to the hub") : "HTTP %d", code);
    if (code < 0) forgetMdns(c.url);                // IP may have changed: resolve again next time
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
  xSemaphoreTake(srvMtx, portMAX_DELAY);
  memcpy(cfgCopy, gCfg, sizeof cfgCopy); memcpy(user, gUser, sizeof user); memcpy(pass, gPass, sizeof pass);
  const uint32_t ver = gCfgVersion;
  xSemaphoreGive(srvMtx);
  bool any = false;
  static ServerData tmp, prev;
  uint8_t order[SERVERS_MAX];
  const uint8_t n = gRetry.plan(millis(), order);     // healthy first; failed ones every 60 s
  for (uint8_t k = 0; k < n; ++k) {
    const uint8_t i = order[k];
    if (!screenJobStillCurrent()) return any;
    xSemaphoreTake(srvMtx, portMAX_DELAY); prev = gData[i]; xSemaphoreGive(srvMtx);
    const uint32_t t0 = millis();
    const bool ok = fetchOne(i, cfgCopy[i], user, pass, tmp, prev);
    if (!ok) Log.printf("Servers %s: %s (%lums)\n", cfgCopy[i].label, tmp.error, (unsigned long)(millis() - t0));
    gRetry.record(i, ok, millis());
    any |= ok;
    xSemaphoreTake(srvMtx, portMAX_DELAY);
    if (ver == gCfgVersion) { gData[i] = tmp; gDataVersion++; }
    xSemaphoreGive(srvMtx);
  }
  return any;
}


