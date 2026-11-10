#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <math.h>
#include "app_screens.h"
#include "modules/flight_radar/flight_radar.h"
#include "services/aircraft_data_provider.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"
#include "services/https_fetch.h"
#include <esp_heap_caps.h>

// Guition JC3248W535C: AXS15231B 320x480 QSPI LCD, shown landscape (480x320).
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define PANEL_W 320
#define PANEL_H 480

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, PANEL_W, PANEL_H);
// Keep the driver's native portrait geometry. Rotate the canvas instead of
// rotating the panel: AXS15231B full-frame QSPI writes can go blank when the
// panel is rotated independently from a landscape canvas.
Arduino_Canvas *gfx = new Arduino_Canvas(PANEL_W, PANEL_H, panel, 0, 0, 0);

// Shared application state and configuration used by the worker and screen modules.
Shared sh;
SemaphoreHandle_t mtx;
QueueHandle_t cmdQ;
Config cfg;
Preferences prefs;
WebServer configServer(80);
// LVGL renders on the Arduino loop task. Deep widget trees (Weather tabs over the
// sky photo) overflowed the default 8 KB stack ("Stack canary watchpoint (loopTask)").
SET_LOOP_TASK_STACK_SIZE(24 * 1024);
bool configServerStarted = false;
static uint32_t financeRetryAfterMs[FINANCE_MAX_ASSETS] = {};
volatile int8_t financeSingleIndex = -1;
static uint32_t financeBlockedUntilMs = 0;   // honours HTTP 429 Retry-After
static constexpr uint32_t FINANCE_MAX_BACKOFF_MS = 16UL * 60 * 1000;

// ---------------------------------------------------------------- screen context state
// Written only by the UI thread, read by the network task.
static volatile ScreenID gActiveScreen = SCREEN_NONE;
static volatile uint32_t gScreenEpoch = 0;
// Dedup: a command already queued/in flight is not queued again.
static volatile bool gCmdPending[CMD_COUNT] = {};
// Result of the last job per screen (network task -> UI thread).
enum JobOutcome : uint8_t { JOB_NONE = 0, JOB_OK, JOB_FAILED };
static volatile uint8_t gJobOutcome[SCREEN_COUNT] = {};
// Job currently executed by the network task.
static NetJob gCurrentJob = {CMD_COUNT, SCREEN_NONE, 0};

ScreenID activeScreenId() { return gActiveScreen; }

bool screenJobStillCurrent() {
  if (gCurrentJob.screen == SCREEN_NONE) return true;
  return gCurrentJob.screen == gActiveScreen && gCurrentJob.epoch == gScreenEpoch;
}

// ---------------------------------------------------------------- helpers
const char *weatherText(int c) {
  if (c == 0) return "Ensolarado";
  if (c <= 2) return "Parcialmente nublado";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if (c >= 51 && c <= 57) return "Garoa";
  if (c >= 61 && c <= 67) return "Chuva";
  if (c >= 71 && c <= 77) return "Neve";
  if (c >= 80 && c <= 82) return "Pancadas";
  if (c == 85 || c == 86) return "Neve fraca";
  if (c >= 95) return "Tempestade";
  return "Sem dados";
}

bool isRainCode(int c) {
  return (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
}

// One keep-alive TLS session to Yahoo for the whole Finance visit: the spark
// call and any per-symbol fallbacks reuse it instead of doing up to four full
// handshakes (each needs ~40 KB of contiguous internal RAM). Released when the
// user leaves the Finance screen.
static WiFiClientSecure financeClient;
static bool financeClientOpen = false;
static PsramBuffer financeBody(256 * 1024);

static void financeReleaseConnection() {
  if (!financeClientOpen) return;
  financeClient.stop();
  financeClientOpen = false;
  Log.println("Finance: conexão TLS liberada");
}

// GET on Yahoo into financeBody (PSRAM). Returns the HTTP status.
static int financeGet(const String &url, uint32_t &retryAfterSeconds, uint32_t timeoutMs) {
  static bool configured = false;
  if (!configured) { financeClient.setInsecure(); financeClient.setHandshakeTimeout(12); configured = true; }
  if (!financeClient.connected()) financeClient.stop();   // drop a server-closed session first
  HttpsFetchOptions options;
  options.userAgent = "Mozilla/5.0";
  options.noCache = true;
  options.retryAfterSeconds = &retryAfterSeconds;
  financeClientOpen = true;
  const int status = httpsFetch(financeClient, url.c_str(), financeBody, timeoutMs, true, nullptr,
                                "application/json", options);
  if (status <= 0) {
    Log.printf("Finance: erro de transporte %d (%s)\n", status, HTTPClient::errorToString(status).c_str());
    logInternalHeap("finance");
  }
  return status;
}

// Quote of one asset as returned by Yahoo (chart or spark "response" item).
struct Quote {
  bool ok = false;
  float price = 0, changePct = 0, previousClose = 0;
  uint32_t epoch = 0;
  float series[24] = {};
  uint8_t count = 0;
  char currency[4] = {};
};

static void appendPoint(Quote &q, float x) {
  if (q.count < 24) q.series[q.count++] = x;
  else { memmove(q.series, q.series + 1, sizeof(float) * 23); q.series[23] = x; }
}

// Parses {meta:{...}, indicators:{quote:[{close:[...]}]}} (same shape in chart and spark).
static bool parseYahooResult(JsonVariantConst result, Quote &q) {
  JsonObjectConst meta = result["meta"];
  float p = meta["regularMarketPrice"] | 0.0f;
  float prev = meta["previousClose"] | (meta["chartPreviousClose"] | 0.0f);
  float pct = meta["regularMarketChangePercent"] | NAN;
  if (p <= 0 || !isfinite(p)) return false;
  if (!isfinite(pct) && prev > 0) pct = (p - prev) * 100.0f / prev;
  if (!isfinite(pct)) pct = 0;
  q.count = 0;
  for (JsonVariantConst v : result["indicators"]["quote"][0]["close"].as<JsonArrayConst>()) {
    const float x = v | 0.0f;
    if (x > 0) appendPoint(q, x);
  }
  // Yahoo may stop returning intraday candles before the current quote time
  // (e.g. outside FX trading hours): append the current quote so the line
  // does not visually stop at the last historical candle.
  if (q.count == 0 || fabsf(q.series[q.count - 1] - p) > 0.0001f) appendPoint(q, p);
  q.price = p; q.changePct = pct; q.previousClose = prev;
  q.epoch = meta["regularMarketTime"] | 0U;
  snprintf(q.currency, sizeof q.currency, "%s", (const char *)(meta["currency"] | ""));
  q.ok = true;
  return true;
}

// Symbols like ^BVSP need URL encoding ('^' -> %5E).
static String yahooSymbolParam(const char *symbol) {
  String out;
  for (const char *p = symbol; *p; ++p) {
    if (*p == '^') out += "%5E";
    else out += *p;
  }
  return out;
}

// GET with one retry on transport/5xx errors. Returns true with financeBody filled.
static bool financeGetWithRetry(const String &url, const char *what, uint32_t &retryAfterMs, uint32_t timeoutMs) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!screenJobStillCurrent()) return false;  // user left: no retry, no new connection
    uint32_t retrySeconds = 0;
    const int status = financeGet(url, retrySeconds, timeoutMs);
    if (retrySeconds)
      retryAfterMs = max(retryAfterMs, min(retrySeconds, FINANCE_MAX_BACKOFF_MS / 1000UL) * 1000UL);
    if (status == HTTP_CODE_OK && financeBody.length()) return screenJobStillCurrent();
    Log.printf("Finance %s HTTP %d (tentativa %d/2)\n", what, status, attempt + 1);
    if (attempt == 0 && (status <= 0 || status >= HTTP_CODE_INTERNAL_SERVER_ERROR)) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    return false;
  }
  return false;
}

// Per-symbol chart API (also used as fallback when spark misses a symbol).
static bool fetchYahooChart(const char *symbol, Quote &q, uint32_t &retryAfterMs) {
  String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + yahooSymbolParam(symbol) +
               "?range=1d&interval=5m&nocache=" + String(millis());
  if (!financeGetWithRetry(url, symbol, retryAfterMs, 12000)) return false;
  JsonDocument doc(SpiRamJsonAllocator::instance());  // PSRAM: keep internal RAM for TLS
  if (deserializeJson(doc, financeBody.c_str(), financeBody.length())) {
    Log.printf("Finance %s: resposta JSON inválida\n", symbol);
    return false;
  }
  if (!parseYahooResult(doc["chart"]["result"][0], q)) {
    Log.printf("Finance %s: símbolo inexistente ou sem cotação\n", symbol);
    return false;
  }
  return true;
}

// One request for every asset (spark accepts a comma separated list).
static bool fetchYahooSpark(const FinanceAsset *assets, uint8_t count, Quote *quotes) {
  String url = "https://query1.finance.yahoo.com/v7/finance/spark?range=1d&interval=5m&symbols=";
  for (uint8_t i = 0; i < count; ++i) { if (i) url += ','; url += yahooSymbolParam(assets[i].symbol); }
  uint32_t retryAfterMs = 0;
  const bool got = financeGetWithRetry(url, "spark", retryAfterMs, 15000);
  if (retryAfterMs) for (uint8_t i = 0; i < count; ++i) financeRetryAfterMs[i] = max(financeRetryAfterMs[i], retryAfterMs);
  if (!got) return false;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (deserializeJson(doc, financeBody.c_str(), financeBody.length())) { Log.println("Finance spark: resposta JSON inválida"); return false; }
  for (JsonVariantConst item : doc["spark"]["result"].as<JsonArrayConst>()) {
    const char *symbol = item["symbol"] | "";
    for (uint8_t i = 0; i < count; ++i)
      if (!strcmp(symbol, assets[i].symbol)) { parseYahooResult(item["response"][0], quotes[i]); break; }
  }
  for (uint8_t i = 0; i < count; ++i)
    if (!quotes[i].ok) Log.printf("Finance spark: símbolo %s ausente/sem cotação\n", assets[i].symbol);
  return true;
}

// Runs on the network task only, and only for the active Finance screen.
// onlyIndex = -1 refreshes every asset; otherwise just that card.
static bool doFinance(int onlyIndex) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if ((int32_t)(millis() - financeBlockedUntilMs) < 0) {
    Log.println("Finance: aguardando Retry-After da API");
    return false;
  }
  static FinanceAsset assets[FINANCE_MAX_ASSETS];
  static Quote quotes[FINANCE_MAX_ASSETS];
  xSemaphoreTake(mtx, portMAX_DELAY);
  const uint8_t count = sh.financeAssetCount;
  const uint32_t configVersion = sh.financeConfigVersion;
  memcpy(assets, sh.financeAssets, sizeof assets);
  sh.financeRefreshing = true;
  xSemaphoreGive(mtx);
  for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) quotes[i] = Quote();
  if (onlyIndex >= count) onlyIndex = -1;

  if (onlyIndex < 0 && count) fetchYahooSpark(assets, count, quotes);
  for (uint8_t i = 0; i < count; ++i) {
    if (onlyIndex >= 0 && i != onlyIndex) continue;
    if (!screenJobStillCurrent()) break;   // user left the screen: stop here
    if (!quotes[i].ok && financeRetryAfterMs[i] == 0) fetchYahooChart(assets[i].symbol, quotes[i], financeRetryAfterMs[i]);
  }

  bool anySuccess = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  // Discard results if the asset list was edited while downloading.
  if (sh.financeConfigVersion == configVersion) {
    for (uint8_t i = 0; i < count; ++i) {
      if (onlyIndex >= 0 && i != onlyIndex) continue;
      Quote &q = quotes[i];
      sh.financeApiKnown[i] = true;
      sh.financeApiOk[i] = q.ok;
      if (!q.ok) { Log.printf("Finance %s: atualização falhou; mantendo última cotação válida\n", assets[i].symbol); continue; }
      anySuccess = true;
      financeRetryAfterMs[i] = 0;
      sh.previousFinance[i] = q.previousClose; sh.financeChange[i] = q.changePct;
      sh.haveChange[i] = fabsf(q.changePct) > 0.0001f;
      sh.finance[i] = q.price; sh.haveFinance[i] = true;
      sh.financeQuoteEpoch[i] = q.epoch;
      if (q.currency[0]) memcpy(sh.financeCurrency[i], q.currency, sizeof sh.financeCurrency[i]);
      // Without intraday candles, keep the graph useful with previous close + quote.
      if (q.count < 2 && q.previousClose > 0) { q.series[0] = q.previousClose; q.series[1] = q.price; q.count = 2; }
      if (q.count >= 2) { sh.historyCount[i] = q.count; memcpy(sh.history[i], q.series, sizeof q.series); }
    }
  }
  sh.financeRefreshing = false;
  uint8_t totalSuccesses = 0;
  for (uint8_t i = 0; i < sh.financeAssetCount; ++i) if (sh.financeApiKnown[i] && sh.financeApiOk[i]) totalSuccesses++;
  sh.financeSuccessCount = totalSuccesses;
  if (anySuccess) sh.financeLastSuccessMs = millis();
  sh.financeVersion++;
  xSemaphoreGive(mtx);

  uint32_t retryAfter = 0;
  for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) { retryAfter = max(retryAfter, financeRetryAfterMs[i]); financeRetryAfterMs[i] = 0; }
  if (retryAfter) financeBlockedUntilMs = millis() + retryAfter;
  return anySuccess;
}

static String urlEncode(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = (uint8_t)s[i];
    // Encode UTF-8 bytes individually; only ASCII URL-unreserved bytes pass through.
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static void setStatus(const String &s) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.status = s;
  xSemaphoreGive(mtx);
}

static bool httpGetJson(const String &url, JsonDocument &doc, String &error) {
  static PsramBuffer body(256 * 1024);   // response kept in PSRAM, not in an internal String
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!screenJobStillCurrent()) { error = "cancelado (tela inativa)"; return false; }  // no retry off-screen
    WiFiClientSecure client;
    client.setInsecure();  // public read-only weather data; no secrets are sent
    client.setHandshakeTimeout(12);
    const int code = httpsFetch(client, url.c_str(), body, 10000, false);
    client.stop();
    if (code == HTTP_CODE_OK) {
      // The user may have left the screen while the download was running:
      // skip the JSON parsing entirely in that case.
      if (!screenJobStillCurrent()) { error = "cancelado (tela inativa)"; return false; }
      DeserializationError jsonError = deserializeJson(doc, body.c_str(), body.length());
      if (!jsonError) return true;
      error = String("JSON inválido: ") + jsonError.c_str();
      Log.printf("Weather: %s\n", error.c_str());
      return false;
    }
    error = code <= 0 ? String("erro de conexão: ") + HTTPClient::errorToString(code)
                      : String("HTTP ") + code;
    Log.printf("Weather: %s (tentativa %d/2)\n", error.c_str(), attempt + 1);
    if (code <= 0) logInternalHeap("weather");
    // Retry only transient transport/server errors; client errors such as 429
    // are handled by the worker's slower retry schedule.
    if (attempt == 0 && (code <= 0 || code >= HTTP_CODE_INTERNAL_SERVER_ERROR)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    return false;
  }
  return false;
}

// ---------------------------------------------------------------- worker task
static void applyTime() { configTime(cfg.utcOffset, 0, "pool.ntp.org", "time.google.com"); }

static bool doWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m,apparent_temperature,uv_index"
               "&hourly=temperature_2m,weather_code,precipitation_probability"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&timezone=auto&forecast_days=7&forecast_hours=24";
  JsonDocument doc(SpiRamJsonAllocator::instance());  // PSRAM: keep internal RAM for TLS
  String error;
  if (!httpGetJson(url, doc, error)) {
    if (screenJobStillCurrent()) setStatus(String("Falha clima: ") + error);
    return false;
  }
  JsonArray hourlyTimes = doc["hourly"]["time"].as<JsonArray>();
  JsonArray hourlyTemps = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray hourlyCodes = doc["hourly"]["weather_code"].as<JsonArray>();
  JsonArray hourlyRain = doc["hourly"]["precipitation_probability"].as<JsonArray>();
  JsonArray dailyTimes = doc["daily"]["time"].as<JsonArray>();
  JsonArray dailyMax = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray dailyMin = doc["daily"]["temperature_2m_min"].as<JsonArray>();
  JsonArray dailyCodes = doc["daily"]["weather_code"].as<JsonArray>();
  if (doc["current"]["temperature_2m"].isNull() || hourlyTimes.isNull() ||
      hourlyTimes.size() == 0 || hourlyTemps.size() != hourlyTimes.size() ||
      hourlyCodes.size() != hourlyTimes.size() || hourlyRain.size() != hourlyTimes.size() ||
      dailyTimes.isNull() || dailyTimes.size() < 7 || dailyMax.size() < 7 ||
      dailyMin.size() < 7 || dailyCodes.size() < 7) {
    setStatus("Falha clima: resposta incompleta da API");
    Log.println("Weather: resposta incompleta da API");
    return false;
  }

  int off = doc["utc_offset_seconds"] | cfg.utcOffset;
  if (off != cfg.utcOffset) {
    cfg.utcOffset = off;
    prefs.putInt("tz", off);
    applyTime();
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.temp = doc["current"]["temperature_2m"] | 0.0f;
  sh.code = doc["current"]["weather_code"] | 0;
  sh.humidity = doc["current"]["relative_humidity_2m"] | 0;
  sh.wind = doc["current"]["wind_speed_10m"] | 0.0f;
  sh.feels = doc["current"]["apparent_temperature"] | sh.temp;
  sh.uv = doc["current"]["uv_index"] | -1.0f;
  sh.hourCount = 0;
  JsonArray times = hourlyTimes;
  JsonArray hourlyTemp = hourlyTemps;
  JsonArray hourlyCode = hourlyCodes;
  String currentHour = String(doc["current"]["time"] | "");
  currentHour = currentHour.substring(0, 13);
  int first = 0;
  for (size_t i = 0; i < times.size(); i++) {
    String candidate = String(times[i] | "");
    if (candidate.startsWith(currentHour)) { first = i; break; }
  }
  for (int j = 0; j < 8; j++) {
    int i = first + j * 3;
    if (i >= (int)times.size()) break;
    String hour = String(times[i] | "");
    if (hour.length() >= 13) {
      sh.hourLabel[j][0] = hour[11]; sh.hourLabel[j][1] = hour[12];
      sh.hourLabel[j][2] = 0;
    }
    sh.hourTemp[j] = hourlyTemp[i] | 0.0f;
    sh.hourCode[j] = hourlyCode[i] | 0;
    sh.hourRain[j] = hourlyRain[i] | 0;
    sh.hourCount++;
  }
  for (int i = 0; i < 7; i++) {
    sh.tmax[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    sh.tmin[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    sh.dcode[i] = doc["daily"]["weather_code"][i] | 0;
    sh.dayRain[i] = doc["daily"]["precipitation_probability_max"][i] | 0;
    const char *d = doc["daily"]["time"][i] | "";
    int y, m, dd;
    struct tm t = {};
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3) {
      t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = dd; t.tm_hour = 12;
      mktime(&t);
      static const char *weekdays[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
      snprintf(sh.dayName[i], sizeof sh.dayName[i], "%s", weekdays[t.tm_wday]);
    }
  }
  sh.haveWeather = true;
  sh.weatherVersion++;
  sh.status = "Previsão atualizada";
  xSemaphoreGive(mtx);
  return true;
}

static bool doGeocode() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = sh.pendingCity;
  xSemaphoreGive(mtx);
  if (city.isEmpty() || WiFi.status() != WL_CONNECTED) { setStatus("Conecte ao Wi-Fi primeiro"); return false; }
  String countryCode;
  int comma = city.lastIndexOf(',');
  if (comma >= 0) {
    countryCode = city.substring(comma + 1);
    countryCode.trim();
    city = city.substring(0, comma);
    city.trim();
    countryCode.toUpperCase();
  }
  String lookup = "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + urlEncode(city);
  if (countryCode.length() == 2) lookup += "&countryCode=" + countryCode;
  JsonDocument doc(SpiRamJsonAllocator::instance());  // PSRAM: keep internal RAM for TLS
  String error;
  if (!httpGetJson(lookup, doc, error)) {
    setStatus(String("Falha geocodificação: ") + error);
    return false;
  }
  if (doc["results"].isNull()) {
    setStatus("Cidade não encontrada");
    return false;
  }
  cfg.lat = doc["results"][0]["latitude"] | cfg.lat;
  cfg.lon = doc["results"][0]["longitude"] | cfg.lon;
  String name = doc["results"][0]["name"] | city.c_str();
  String resolvedCountry = doc["results"][0]["country_code"] | countryCode.c_str();
  if (resolvedCountry.length()) name += ", " + resolvedCountry;
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", name);
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.cityName = name;
  sh.weatherVersion++;
  xSemaphoreGive(mtx);
  return true;
}

static void doScan() {
  setStatus("Buscando redes...");
  int n = WiFi.scanNetworks();
  String list;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty() || list.indexOf(s + "\n") >= 0) continue;
    list += s + "\n";
  }
  if (list.endsWith("\n")) list.remove(list.length() - 1);
  WiFi.scanDelete();
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.ssidList = list;
  sh.scanVersion++;
  sh.status = n > 0 ? String(n) + (n == 1 ? " rede encontrada" : " redes encontradas") : "Nenhuma rede encontrada";
  xSemaphoreGive(mtx);
}

static void doConnect() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String ssid = sh.pendingSsid, pass = sh.pendingPass;
  xSemaphoreGive(mtx);
  if (ssid.isEmpty()) { setStatus("Selecione uma rede"); return; }
  setStatus("Conectando a " + ssid + "...");
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(500));
  if (WiFi.status() == WL_CONNECTED) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    setStatus("Conectado: " + WiFi.localIP().toString());
    applyTime();
  } else {
    setStatus("Falha ao conectar (verifique a senha)");
  }
}

static const char *cmdName(uint8_t cmd) {
  switch (cmd) {
    case CMD_SCAN: return "scan";
    case CMD_CONNECT: return "connect";
    case CMD_GEOCODE: return "geocode";
    case CMD_WEATHER: return "weather";
    case CMD_FINANCE: return "finance";
    case CMD_FINANCE_ONE: return "finance-one";
    case CMD_FLIGHT_RADAR: return "flight-radar";
    case CMD_FLIGHT_DETAILS: return "flight-details";
    case CMD_SERVERS: return "servers";
  }
  return "?";
}

// Executes one API job. Blocking HTTP is fine here: this is the dedicated
// network task, never the LVGL thread.
static void runJob(const NetJob &job) {
  gCurrentJob = job;
  const uint32_t started = millis();
  bool ok = false;
  bool ran = true;
  if (job.screen != SCREEN_NONE && !screenJobStillCurrent()) {
    ran = false;  // stale request from a screen the user already left
  } else {
    switch (job.cmd) {
      case CMD_SCAN: doScan(); break;
      case CMD_CONNECT: doConnect(); break;
      case CMD_GEOCODE:
        // Started from Settings or the web page. Refresh the forecast right
        // away only if the Weather screen is the one being shown.
        if (doGeocode() && gActiveScreen == SCREEN_WEATHER) {
          gCurrentJob = {CMD_WEATHER, SCREEN_WEATHER, gScreenEpoch};
          ok = doWeather();
          gJobOutcome[SCREEN_WEATHER] = ok ? JOB_OK : JOB_FAILED;
        }
        break;
      case CMD_WEATHER: ok = doWeather(); break;
      case CMD_FINANCE: ok = doFinance(-1); break;
      case CMD_FINANCE_ONE: ok = doFinance(financeSingleIndex); break;
      case CMD_FLIGHT_RADAR: ok = updateFlightRadarData(); break;
      case CMD_FLIGHT_DETAILS: ok = updateFlightRadarDetails(); break;
      case CMD_SERVERS: ok = serversFetch(); break;
      default: ran = false; break;
    }
  }
  if (job.screen != SCREEN_NONE) {
    if (!ran) {
      Log.printf("[net] %s descartado (tela inativa)\n", cmdName(job.cmd));
    } else if (screenJobStillCurrent()) {
      gJobOutcome[job.screen] = ok ? JOB_OK : JOB_FAILED;
      Log.printf("[net] %s %s em %lums (interno livre %u, maior bloco %u)\n", cmdName(job.cmd),
                    ok ? "ok" : "falhou", (unsigned long)(millis() - started),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
      Log.printf("[net] %s concluído fora de contexto; resultado ignorado\n", cmdName(job.cmd));
    }
  }
  gCurrentJob = {CMD_COUNT, SCREEN_NONE, 0};
  if (job.cmd < CMD_COUNT) gCmdPending[job.cmd] = false;
}

// Network task (core 0). It owns every HTTP/TLS client and only runs jobs that
// the active screen requested. It never touches LVGL objects.
static void workerTask(void *) {
  uint32_t lastReconnectCheck = millis();
  uint32_t reconnectStarted = 0;
  bool wasConnected = false;
  for (;;) {
    NetJob job;
    if (xQueueReceive(cmdQ, &job, pdMS_TO_TICKS(1000)) == pdTRUE) runJob(job);

    // Free the radar's keep-alive TLS session as soon as the radar is not the
    // active screen, so Weather/Finance have internal heap for their handshakes.
    if (gActiveScreen != SCREEN_FLIGHT_RADAR) adsbLolReleaseConnection();
    if (gActiveScreen != SCREEN_FINANCE) financeReleaseConnection();

    const bool up = WiFi.status() == WL_CONNECTED;
    const uint32_t now = millis();
    if (up && !wasConnected) {
      // Housekeeping only (no screen API): sync the clock once per connection.
      applyTime();
      setStatus("Conectado: " + WiFi.localIP().toString());
    }
    wasConnected = up;
    if (up) {
      reconnectStarted = 0;
    } else if (now - lastReconnectCheck >= 60000UL) {
      lastReconnectCheck = now;
      xSemaphoreTake(mtx, portMAX_DELAY);
      String savedSsid = prefs.getString("ssid", "");
      String savedPass = prefs.getString("pass", "");
      xSemaphoreGive(mtx);
      if (!savedSsid.isEmpty() && (!reconnectStarted || now - reconnectStarted >= 30000UL)) {
        reconnectStarted = now;
        Log.printf("Wi-Fi offline; retrying saved network: %s\n", savedSsid.c_str());
        WiFi.disconnect(false, false);
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
    }
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.wifiUp = up;
    xSemaphoreGive(mtx);
  }
}

// ---------------------------------------------------------------- display + touch glue
static bool readTouch(int &x, int &y) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 8};
  uint8_t b[8];
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 8);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 8) != 8) return false;
  for (int i = 0; i < 8; i++) b[i] = Wire.read();
  if (b[1] == 0 || b[1] > 4) return false;
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  return true;
}

// ---------------------------------------------------------------- fast flush
// The canvas is rotated in software. Arduino_Canvas::draw16bitRGBBitmap only has a
// fast path for rotation 0, otherwise it writes pixel by pixel through virtual
// calls. We copy LVGL's landscape area straight into the portrait framebuffer
// instead, column by column, so PSRAM writes are contiguous.
// The mapping is detected once at boot (fbMapping); unknown -> old slow path.
static int fbMapping = -1;          // -1 unknown, 1: idx = x*W + (W-1-y), 3: idx = (H-1-x)*W + y
static uint32_t perfFrames = 0, perfFlushUs = 0, perfCopyUs = 0, perfFrameUs = 0, perfLastLog = 0, perfRefrStart = 0;
static uint32_t qspiHz = 40000000;
static const char *perfScreenName();

// whole refresh (render + copy + send), measured from LVGL display events
static void perfRefrEvent(lv_event_t *e) {
  const lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_REFR_START) perfRefrStart = 0;              // never span two refresh cycles
  else if (c == LV_EVENT_RENDER_START) perfRefrStart = micros();
  else if (perfRefrStart) { perfFrameUs += micros() - perfRefrStart; perfRefrStart = 0; }
}

// ---------------------------------------------------------------- async panel send
// Sending the 300 KB frame to the AXS15231B takes ~40 ms of CPU (byte swap + QSPI
// chunks) no matter the bus clock. A task on core 0 does it from a second buffer,
// so the LVGL thread only pays a PSRAM memcpy (~3-5 ms) and keeps rendering.
static uint16_t *sendBuf = nullptr;
static TaskHandle_t sendTaskHandle = nullptr;
static SemaphoreHandle_t sendIdle = nullptr;
static volatile uint32_t perfPanelUs = 0, perfPanelN = 0;

static void panelSendTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t t = micros();
    panel->draw16bitRGBBitmap(0, 0, sendBuf, PANEL_W, PANEL_H);
    perfPanelUs += micros() - t; perfPanelN++;
    xSemaphoreGive(sendIdle);
  }
}

static void startAsyncSend() {
  sendBuf = (uint16_t *)heap_caps_malloc(PANEL_W * PANEL_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  sendIdle = xSemaphoreCreateBinary();
  if (!sendBuf || !sendIdle) { Log.println("Display: envio assíncrono indisponível (memória)"); sendBuf = nullptr; return; }
  xSemaphoreGive(sendIdle);
  xTaskCreatePinnedToCore(panelSendTask, "panel", 4096, NULL, 2, &sendTaskHandle, 0);
  Log.println("Display: envio assíncrono ativo (core 0)");
}

static void sendFrame() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!sendBuf || !fb) { gfx->flush(); return; }
  xSemaphoreTake(sendIdle, portMAX_DELAY);       // previous frame still going out? wait for it
  memcpy(sendBuf, fb, PANEL_W * PANEL_H * 2);
  xTaskNotifyGive(sendTaskHandle);
}

static void detectFramebufferMapping() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  const int W = PANEL_W, H = PANEL_H;
  const int tx[2] = {5, 401}, ty[2] = {7, 250};
  const uint16_t tc[2] = {0x1234, 0x4321};
  for (int i = 0; i < 2; ++i) gfx->drawPixel(tx[i], ty[i], tc[i]);
  bool m1 = true, m3 = true;
  for (int i = 0; i < 2; ++i) {
    m1 &= fb[tx[i] * W + (W - 1 - ty[i])] == tc[i];
    m3 &= fb[(H - 1 - tx[i]) * W + ty[i]] == tc[i];
  }
  fbMapping = m1 ? 1 : m3 ? 3 : -1;
  for (int i = 0; i < 2; ++i) gfx->drawPixel(tx[i], ty[i], 0);
  Log.printf("Display: framebuffer mapping %d (%s)\n", fbMapping, fbMapping > 0 ? "fast flush" : "fallback");
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  const uint32_t t0 = micros();
  const int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  uint16_t *fb = gfx->getFramebuffer();
  const uint16_t *src = (const uint16_t *)px;
  if (fbMapping > 0 && fb) {
    const int W = PANEL_W, H = PANEL_H;
    for (int c = 0; c < w; ++c) {
      const int lx = a->x1 + c;
      if (fbMapping == 1) {                       // column lx -> fb row lx, y reversed
        uint16_t *dst = fb + lx * W + (W - 1 - a->y1);
        for (int r = 0; r < h; ++r) *(dst - r) = src[r * w + c];
      } else {                                    // column lx -> fb row (H-1-lx), y forward
        uint16_t *dst = fb + (H - 1 - lx) * W + a->y1;
        for (int r = 0; r < h; ++r) dst[r] = src[r * w + c];
      }
    }
  } else {
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
  }
  const uint32_t t1 = micros();
  perfCopyUs += t1 - t0;
  if (lv_display_flush_is_last(disp)) {
    sendFrame();
    perfFlushUs += micros() - t1;
    ++perfFrames;
    if (millis() - perfLastLog >= 10000) {        // one perf line every 10 s (only when drawing)
      const uint32_t el = millis() - perfLastLog;
      const uint32_t frame = perfFrameUs / perfFrames, copy = perfCopyUs / perfFrames, send = perfFlushUs / perfFrames;
      const uint32_t render = frame > copy + send ? frame - copy - send : 0;
      const uint32_t panelMs = perfPanelN ? perfPanelUs / perfPanelN / 1000 : 0;
      Log.printf("[perf] %s: %lu fps | quadro %lu ms = desenho %lu + copia %lu + envio %lu ms | tela (core 0) %lu ms | QSPI %lu MHz | livre %u\n",
                 perfScreenName(), (unsigned long)(perfFrames * 1000 / (el ? el : 1)), (unsigned long)(frame / 1000),
                 (unsigned long)(render / 1000), (unsigned long)(copy / 1000), (unsigned long)(send / 1000),
                 (unsigned long)panelMs, (unsigned long)(qspiHz / 1000000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      perfFrames = perfFlushUs = perfCopyUs = perfFrameUs = 0; perfPanelUs = perfPanelN = 0; perfLastLog = millis();
    }
  }
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  int rawX, rawY;
  if (readTouch(rawX, rawY)) {
    // The touch controller reports native portrait coordinates. Rotate them
    // into the landscape coordinates used by the LCD and LVGL.
    data->point.x = constrain(rawY, 0, SCR_W - 1);
    data->point.y = constrain(PANEL_W - 1 - rawX, 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------------------------------------------------------------- UI
lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
lv_obj_t *lblRange, *lblHumidity, *lblWind, *weatherArt, *homeTab;
lv_obj_t *fcDay[7], *fcCond[7], *fcTemp[7];
lv_obj_t *hourTime[8], *hourCond[8], *hourTemp[8], *hourRain[8];
lv_obj_t *hourArt[8], *dayArt[7];
lv_obj_t *ddSsid, *taPass, *taCity, *kb, *setStatusLbl, *wifiInfoLbl, *swH24;
lv_obj_t *mainMenu, *weatherView, *settingsView, *weatherTabs;
lv_obj_t *financeView, *financeStrip, *financeStatusLabel, *financeAgeLabel[FINANCE_MAX_ASSETS], *financeValue[FINANCE_MAX_ASSETS], *financeDelta[FINANCE_MAX_ASSETS], *financePlot[FINANCE_MAX_ASSETS], *financeLine[FINANCE_MAX_ASSETS], *financeHeader[FINANCE_MAX_ASSETS], *financeCard[FINANCE_MAX_ASSETS];
lv_obj_t *flightRadarView;
lv_point_precise_t financeLinePoints[FINANCE_MAX_ASSETS][24] = {};
static lv_obj_t *menuWifiIcon;
static lv_obj_t *menuServersBadge;
static lv_obj_t *menuClock;
static lv_obj_t *menuCalDay, *menuCalWeek;
lv_obj_t *calendarView;
int calendarOffset = 0;
lv_obj_t *calendarTitle;
lv_obj_t *calendarGrid;
lv_point_t calTouchStart;
static uint32_t seenScan = 0;

void post(int cmd) {
  NetJob job = {(uint8_t)cmd, SCREEN_NONE, 0};
  xQueueSend(cmdQ, &job, 0);
}

// Queues an API job tagged with the active screen and its visit epoch.
// Never blocks the UI thread; duplicates of an in-flight command are dropped.
void postScreenJob(int cmd) {
  if (cmd < 0 || cmd >= CMD_COUNT || gActiveScreen == SCREEN_NONE) return;
  if (gCmdPending[cmd]) return;
  NetJob job = {(uint8_t)cmd, gActiveScreen, gScreenEpoch};
  gCmdPending[cmd] = true;
  if (xQueueSend(cmdQ, &job, 0) != pdTRUE) gCmdPending[cmd] = false;
}

void setLabelIfChanged(lv_obj_t *label, const char *text) {
  if (!label || !text) return;
  const char *current = lv_label_get_text(label);
  if (current && strcmp(current, text) == 0) return;  // avoid needless invalidation/redraw
  lv_label_set_text(label, text);
}

// ---------------------------------------------------------------- per-screen render (UI thread)
static bool renderedWifiUp = false;
static bool renderedWifiKnown = false;

static void renderWifiIndicator(lv_obj_t *icon, bool up) {
  lv_obj_set_style_text_color(icon, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
}

static void renderMenu() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  if (!renderedWifiKnown || renderedWifiUp != up) {
    renderWifiIndicator(menuWifiIcon, up);
    if (up) lv_obj_add_flag(menuWifiIcon, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_remove_flag(menuWifiIcon, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(menuWifiIcon); }
    renderedWifiUp = up;
    renderedWifiKnown = true;
  }
  if (menuClock) {
    struct tm ti; char b[12];
    if (getLocalTime(&ti, 0)) {
      strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M", &ti); setLabelIfChanged(menuClock, b);
      static const char *wd[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
      if (menuCalWeek) setLabelIfChanged(menuCalWeek, wd[ti.tm_wday]);
      if (menuCalDay) { snprintf(b, sizeof b, "%d", ti.tm_mday); setLabelIfChanged(menuCalDay, b); }
    }
  }
  if (menuServersBadge) {
    const bool alert = serversAlert();
    if (alert != !lv_obj_has_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN)) {
      if (alert) lv_obj_remove_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void renderWeather() {
  struct tm ti;
  char b[24];
  if (getLocalTime(&ti, 0)) {
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

static void renderSettings() {
  configSyncRadarUi();
  xSemaphoreTake(mtx, portMAX_DELAY);
  String status = sh.status;
  String list = sh.ssidList;
  bool up = sh.wifiUp;
  uint32_t scanVersion = sh.scanVersion;
  xSemaphoreGive(mtx);
  setLabelIfChanged(setStatusLbl, status.c_str());
  String wifiInfo = up ? "Conectado: " + WiFi.SSID() + "  |  IP " + WiFi.localIP().toString()
                       : "Desconectado  |  aguardando reconexão";
  setLabelIfChanged(wifiInfoLbl, wifiInfo.c_str());
  if (scanVersion != seenScan) {
    seenScan = scanVersion;
    lv_dropdown_set_options(ddSsid, list.isEmpty() ? "(none)" : list.c_str());
  }
}

static void settingsOnExit() {
  if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void requestWeather() { postScreenJob(CMD_WEATHER); }
static void requestFinance() { postScreenJob(CMD_FINANCE); }
static void requestFlightRadar() { postScreenJob(CMD_FLIGHT_RADAR); }
static void requestFlightDetails() { postScreenJob(CMD_FLIGHT_DETAILS); }
static void requestServers() { postScreenJob(CMD_SERVERS); }
static void requestClockWeather() { postScreenJob(CMD_WEATHER); }   // sky follows the weather

// ---------------------------------------------------------------- screen manager
static const ScreenLifecycle kScreens[] = {
  {SCREEN_NONE, "none", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0},
  {SCREEN_MENU, "menu", &mainMenu, nullptr, nullptr, nullptr, renderMenu, 0, 0},
  {SCREEN_WEATHER, "weather", &weatherView, weatherOnEnter, nullptr, requestWeather, renderWeather,
   WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS},
  {SCREEN_FINANCE, "finance", &financeView, financeOnEnter, nullptr, requestFinance, updateFinanceScreen,
   FINANCE_UPDATE_INTERVAL_MS, FINANCE_RETRY_INTERVAL_MS},
  {SCREEN_CALENDARIO, "calendario", &calendarView, calendarOnEnter, nullptr, calendarRequestUpdate, nullptr,
   CALENDARIO_UPDATE_INTERVAL_MS, CALENDARIO_UPDATE_INTERVAL_MS},
  {SCREEN_FLIGHT_RADAR, "flight-radar", &flightRadarView, flightRadarOnEnter, flightRadarOnExit, requestFlightRadar,
   updateFlightRadarScreen, FLIGHT_RADAR_UPDATE_INTERVAL_MS, FLIGHT_RADAR_RETRY_INTERVAL_MS},
  {SCREEN_SETTINGS, "settings", &settingsView, nullptr, settingsOnExit, nullptr, renderSettings, 0, 0},
  {SCREEN_SERVERS, "servers", &serversView, serversOnEnter, nullptr, requestServers, serversRender,
   SERVERS_UPDATE_INTERVAL_MS, SERVERS_RETRY_INTERVAL_MS},
  {SCREEN_CLOCK, "clock", &clockView, clockOnEnter, clockOnExit, requestClockWeather, clockRender,
   WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS},
};
static_assert(sizeof(kScreens) / sizeof(kScreens[0]) == SCREEN_COUNT, "kScreens must list every ScreenID in order");

static lv_timer_t *contextTimer = nullptr;   // the single API timer, owned by the active screen

static const ScreenLifecycle &screenInfo(ScreenID id) {
  return kScreens[id < SCREEN_COUNT ? id : SCREEN_NONE];
}
static const char *perfScreenName() { return screenInfo(gActiveScreen).name; }

// Fires once SCREEN_ENTER_DEBOUNCE_MS after entering a screen, then every
// updateIntervalMs while the user stays there.
static void contextTimerCb(lv_timer_t *timer) {
  const ScreenLifecycle &screen = screenInfo(gActiveScreen);
  if (!screen.requestUpdate) { lv_timer_pause(timer); return; }
  screen.requestUpdate();
  if (screen.updateIntervalMs) lv_timer_set_period(timer, screen.updateIntervalMs);
  else lv_timer_pause(timer);
}

// After a job finishes, restart the countdown from its completion: normal
// interval on success, a shorter retry on failure.
static void applyJobOutcome() {
  const ScreenID id = gActiveScreen;
  const uint8_t outcome = gJobOutcome[id];
  if (outcome == JOB_NONE) return;
  gJobOutcome[id] = JOB_NONE;
  const ScreenLifecycle &screen = screenInfo(id);
  if (!screen.updateIntervalMs || !contextTimer) return;
  uint32_t period = screen.updateIntervalMs;
  if (outcome == JOB_FAILED && screen.retryIntervalMs) period = min(period, screen.retryIntervalMs);
  lv_timer_set_period(contextTimer, period);
  lv_timer_reset(contextTimer);
  lv_timer_resume(contextTimer);
}

void switchScreen(ScreenID id) {
  if (id == SCREEN_NONE || id >= SCREEN_COUNT) return;
  const ScreenID previous = gActiveScreen;
  if (previous == id) return;

  // ---- On-Exit: stop the previous screen's timer and invalidate its jobs.
  if (contextTimer) lv_timer_pause(contextTimer);
  const ScreenLifecycle &old = screenInfo(previous);
  if (old.onExit) old.onExit();
  gScreenEpoch = gScreenEpoch + 1;  // queued/in-flight jobs of the old visit become stale
  gActiveScreen = id;
  for (int i = 0; i < SCREEN_COUNT; ++i) gJobOutcome[i] = JOB_NONE;

  // ---- Swap the visible container.
  for (const ScreenLifecycle &screen : kScreens) {
    if (!screen.view || !*screen.view) continue;
    if (screen.id == id) lv_obj_remove_flag(*screen.view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(*screen.view, LV_OBJ_FLAG_HIDDEN);
  }

  // ---- On-Enter: paint cached data now, request fresh data after the debounce.
  const ScreenLifecycle &next = screenInfo(id);
  Log.printf("[ctx] %s -> %s\n", old.name, next.name);
  if (next.onEnter) next.onEnter();
  if (next.render) next.render();
  if (next.requestUpdate && contextTimer) {
    lv_timer_set_period(contextTimer, SCREEN_ENTER_DEBOUNCE_MS);
    lv_timer_reset(contextTimer);
    lv_timer_resume(contextTimer);
  }
}

void showView(lv_obj_t *view) {
  for (const ScreenLifecycle &screen : kScreens) {
    if (screen.view && *screen.view == view) { switchScreen(screen.id); return; }
  }
}

// 500 ms UI tick: renders ONLY the active screen.
static bool lastWifiUp = false;
static void ui_update(lv_timer_t *) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  const ScreenLifecycle &screen = screenInfo(gActiveScreen);
  // Wi-Fi just came back: refresh the active screen right away (after debounce).
  if (up && !lastWifiUp && screen.requestUpdate && contextTimer) {
    lv_timer_set_period(contextTimer, SCREEN_ENTER_DEBOUNCE_MS);
    lv_timer_reset(contextTimer);
    lv_timer_resume(contextTimer);
  }
  lastWifiUp = up;
  applyJobOutcome();
  if (screen.render) screen.render();
}

static String htmlEscape(const String &value) {
  String out;
  out.reserve(value.length() + 12);
  for (size_t i = 0; i < value.length(); i++) {
    switch (value[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += value[i]; break;
    }
  }
  return out;
}

static String configPage(const String &notice = "") {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = sh.cityName;
  String status = sh.status;
  const uint8_t assetCount = sh.financeAssetCount;
  static FinanceAsset assets[FINANCE_MAX_ASSETS];
  static float price[FINANCE_MAX_ASSETS];
  static bool havePrice[FINANCE_MAX_ASSETS], apiKnown[FINANCE_MAX_ASSETS], apiOk[FINANCE_MAX_ASSETS];
  static char currency[FINANCE_MAX_ASSETS][4];
  memcpy(assets, sh.financeAssets, sizeof assets);
  memcpy(price, sh.finance, sizeof price);
  memcpy(havePrice, sh.haveFinance, sizeof havePrice);
  memcpy(apiKnown, sh.financeApiKnown, sizeof apiKnown);
  memcpy(apiOk, sh.financeApiOk, sizeof apiOk);
  memcpy(currency, sh.financeCurrency, sizeof currency);
  xSemaphoreGive(mtx);
  char airportCode[8], airportName[32];
  uint8_t radius = 10;
  getFlightRadarSettings(airportCode, sizeof airportCode, airportName, sizeof airportName, radius);

  String rows;
  for (uint8_t i = 0; i < assetCount; ++i) {
    String info;
    if (havePrice[i]) {
      char b[40];
      snprintf(b, sizeof b, "%s %.4g", currency[i], price[i]);
      info = b;
    } else if (apiKnown[i] && !apiOk[i]) info = "sem cotação (símbolo válido?)";
    else info = "aguardando consulta";
    rows += "<div class=\"asset\"><input name=\"sym\" value=\"" + htmlEscape(assets[i].symbol) +
            "\" placeholder=\"AAPL\" maxlength=\"15\" autocapitalize=\"characters\"><input name=\"lbl\" value=\"" +
            htmlEscape(assets[i].label) + "\" placeholder=\"Legenda (opcional)\" maxlength=\"31\">"
            "<button type=\"button\" class=\"del\" onclick=\"delRow(this)\" title=\"Remover\">&times;</button>"
            "<span class=\"info\">" + htmlEscape(info) + "</span></div>";
  }

  String page = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Ajustes</title>
<style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(145deg,#091629,#1b3b62);color:#edf4ff;font:16px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:18px}
main{max-width:560px;margin:0 auto}.card{padding:22px;margin-bottom:16px;border:1px solid #ffffff20;border-radius:20px;background:#ffffff0d;box-shadow:0 20px 60px #0005}
h1{font-size:24px;margin:0 0 4px}h2{font-size:18px;margin:0 0 4px}.muted{color:#b4c7df}.status{margin:14px 0 0;padding:14px;border-radius:14px;background:#ffffff10;color:#d9e8fa;line-height:1.7}
.notice{background:#18a4ef33;border:1px solid #18a4ef88}.label{display:block;margin:16px 0 8px;color:#d3e1f1}
input,select{width:100%;padding:12px;border:1px solid #ffffff30;border-radius:12px;background:#0c1728;color:#fff;font-size:16px}
button{margin-top:14px;width:100%;padding:13px;border:0;border-radius:12px;background:#18a4ef;color:#071525;font-size:16px;font-weight:700;cursor:pointer}
button.sec{background:#ffffff1a;color:#edf4ff}small{display:block;margin-top:12px;line-height:1.5;color:#adbed2}.row{display:flex;gap:12px;align-items:center}.row input{width:auto}
.asset{display:grid;grid-template-columns:1fr 1.4fr 44px;gap:8px;margin-top:10px}.asset .info{grid-column:1/4;font-size:13px;color:#9fb6d0;margin:-2px 0 4px 4px}
.del{margin:0;padding:0;background:#ff5a5a33;color:#ffb3b3;font-size:22px}.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{width:auto;margin:0;padding:6px 10px;border-radius:999px;background:#ffffff14;color:#cfe2f7;font-size:13px;font-weight:600}code{color:#9fe3ff}.btnlink{display:block;margin-top:14px;padding:12px;border-radius:12px;background:#ffffff1a;color:#edf4ff;text-align:center;text-decoration:none;font-weight:700}</style>
<main>
<div class="card"><h1>NextNeo Vista</h1><div class="muted">Ajustes do aparelho</div>
<div class="status %NOTICECLASS%">%NOTICE%Wi-Fi: %STATUS%<br>Endereço: http://%IP%</div>
<a class="btnlink" href="/log">Ver log da aplicação</a></div>

<form class="card" method="post" action="/save-display"><h2>Tela (desempenho)</h2>
<div class="muted">Velocidade do barramento QSPI da tela. Valores maiores deixam as animações mais fluidas; se aparecerem listras ou chiado, volte para 40 MHz. A placa reinicia ao salvar.</div>
<select name="qspi"><option value="40" %Q40%>40 MHz (padrão, seguro)</option><option value="60" %Q60%>60 MHz</option><option value="80" %Q80%>80 MHz (mais rápido)</option></select>
<button type="submit">Salvar e reiniciar</button></form>

<form class="card" method="post" action="/save-city" accept-charset="UTF-8"><h2>Clima e relógio</h2>
<label class="label" for="city">Cidade (ex.: São Paulo, BR)</label><input id="city" name="city" value="%CITY%" required>
<label class="label row"><input type="checkbox" name="h24" value="1" %H24%> Relógio 24 horas</label>
<button type="submit">Salvar clima</button></form>

<form class="card" method="post" action="/save-radar" accept-charset="UTF-8"><h2>Radar aéreo</h2>
<div class="muted">Atual: %AIRPORT% (%AIRPORTNAME%), raio %RADIUS% km</div>
<label class="label" for="airport">Aeroporto (IATA ou ICAO)</label>
<input id="airport" name="airport" value="%AIRPORT%" maxlength="4" list="airports" autocapitalize="characters" required>
<datalist id="airports"><option value="CGH">Congonhas</option><option value="GRU">Guarulhos</option><option value="VCP">Viracopos</option><option value="SDU">Santos Dumont</option><option value="GIG">Galeão</option><option value="BSB">Brasília</option><option value="CNF">Confins</option><option value="CWB">Curitiba</option><option value="POA">Porto Alegre</option><option value="FLN">Florianópolis</option><option value="REC">Recife</option><option value="SSA">Salvador</option><option value="FOR">Fortaleza</option><option value="BEL">Belém</option><option value="MAO">Manaus</option><option value="NAT">Natal</option><option value="MCZ">Maceió</option><option value="JPA">João Pessoa</option><option value="IGU">Foz do Iguaçu</option><option value="GYN">Goiânia</option></datalist>
<label class="label" for="radius">Raio</label>
<select id="radius" name="radius"><option value="5" %R5%>5 km</option><option value="10" %R10%>10 km</option><option value="25" %R25%>25 km</option><option value="50" %R50%>50 km</option></select>
<button type="submit">Salvar radar</button></form>

<form class="card" method="post" action="/save-finance" accept-charset="UTF-8" onsubmit="return prep()"><h2>Finance</h2>
<div class="muted">Até 8 ativos. Use o símbolo do <b>Yahoo Finance</b>.</div>
<div id="assets">%ROWS%</div>
<div class="chips" id="chips"></div>
<button type="button" class="sec" onclick="addRow('','')">+ Adicionar ativo</button>
<button type="submit">Salvar Finance</button>
<small>Exemplos: moedas <code>USDBRL=X</code>, <code>GBPBRL=X</code>; cripto <code>BTC-USD</code>, <code>ETH-USD</code>; ações dos EUA <code>AAPL</code>, <code>MSFT</code>; B3 com <code>.SA</code>: <code>PETR4.SA</code>, <code>VALE3.SA</code>; índices <code>^BVSP</code> (Ibovespa), <code>^GSPC</code> (S&amp;P 500). Confira o símbolo em finance.yahoo.com.</small></form>
<form class="card" method="post" action="/save-servers" accept-charset="UTF-8"><h2>Servidores (Beszel)</h2>
<div class="muted">Use um usuário <b>somente leitura</b> do Beszel, com acesso aos dois sistemas.</div>
<label class="label" for="bzuser">E-mail do usuário</label><input id="bzuser" name="bzuser" type="email" value="%BZUSER%" autocomplete="username">
<label class="label" for="bzpass">Senha</label><input id="bzpass" name="bzpass" type="password" placeholder="%BZPASSPH%" autocomplete="current-password">
%BZROWS%
<button type="submit">Salvar servidores</button>
<small>Cole o link da página do sistema no Beszel (ex.: <code>http://nextneo.local:8090/system/abc123</code>); a URL do hub e o ID são extraídos automaticamente.</small></form>
</main>
<script>
const MAX=8;
function rows(){return document.querySelectorAll('#assets .asset')}
function addRow(s,l){if(rows().length>=MAX){alert('Máximo de '+MAX+' ativos');return}
const d=document.createElement('div');d.className='asset';
d.innerHTML='<input name="sym" placeholder="AAPL" maxlength="15" autocapitalize="characters"><input name="lbl" placeholder="Legenda (opcional)" maxlength="31"><button type="button" class="del" onclick="delRow(this)" title="Remover">&times;</button>';
d.children[0].value=s;d.children[1].value=l;document.getElementById('assets').appendChild(d);if(!s)d.children[0].focus()}
function delRow(b){b.parentNode.remove()}
function prep(){let n=0;rows().forEach(r=>{const s=r.children[0],l=r.children[1];s.value=s.value.trim().toUpperCase();
if(!s.value){r.remove();return}s.name='sym'+n;l.name='lbl'+n;n++});if(!n){alert('Adicione pelo menos um ativo');return false}return true}
[['AAPL','Apple'],['PETR4.SA','Petrobras'],['^BVSP','Ibovespa'],['ETH-USD','Ethereum'],['GBPBRL=X','Libra para real']].forEach(([s,l])=>{
const c=document.createElement('button');c.type='button';c.className='chip';c.textContent='+ '+s;c.onclick=()=>addRow(s,l);document.getElementById('chips').appendChild(c)});
</script></html>)HTML";
  page.replace("%NOTICECLASS%", notice.isEmpty() ? "" : "notice");
  page.replace("%NOTICE%", notice.isEmpty() ? String() : htmlEscape(notice) + "<br>");
  page.replace("%STATUS%", htmlEscape(status));
  page.replace("%IP%", WiFi.localIP().toString());
  page.replace("%CITY%", htmlEscape(city));
  page.replace("%H24%", cfg.h24 ? "checked" : "");
  page.replace("%AIRPORTNAME%", htmlEscape(airportName));
  page.replace("%AIRPORT%", htmlEscape(airportCode));
  page.replace("%RADIUS%", String(radius));
  page.replace("%R5%", radius == 5 ? "selected" : "");
  page.replace("%R10%", radius == 10 ? "selected" : "");
  page.replace("%R25%", radius == 25 ? "selected" : "");
  page.replace("%R50%", radius == 50 ? "selected" : "");
  page.replace("%ROWS%", rows);
  page.replace("%Q40%", qspiHz == 40000000 ? "selected" : "");
  page.replace("%Q60%", qspiHz == 60000000 ? "selected" : "");
  page.replace("%Q80%", qspiHz == 80000000 ? "selected" : "");
  String bzRows;
  for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
    char lbl[64], url[64], id[64];
    serversGetConfig(i, lbl, url, id, sizeof lbl);
    const String n = String(i);
    bzRows += "<label class=\"label\">Servidor " + String(i + 1) + "</label><div class=\"asset\" style=\"grid-template-columns:1fr 1.6fr\">"
              "<input name=\"bzlbl" + n + "\" value=\"" + htmlEscape(lbl) + "\" placeholder=\"Apelido\" maxlength=\"23\">"
              "<input name=\"bzlink" + n + "\" value=\"" + htmlEscape(String(url) + "/system/" + id) + "\" placeholder=\"http://host:8090/system/ID\">"
              "<span class=\"info\" style=\"grid-column:1/3\">" + htmlEscape(serversStatusText(i)) + "</span></div>";
  }
  page.replace("%BZROWS%", bzRows);
  page.replace("%BZUSER%", htmlEscape(serversUser()));
  page.replace("%BZPASSPH%", serversHasPassword() ? "•••••• (salva; deixe vazio para manter)" : "senha do usuário");
  return page;
}

static void sendConfigPage(const String &notice) {
  configServer.send(200, "text/html; charset=utf-8", configPage(notice));
}

// ---------------------------------------------------------------- log page
// Last 30 minutes of log, newest first. Errors in red, warnings in yellow.
// Streamed in chunks so no big String is built in RAM.
static void sendLogPage() {
  static LogLine *snapshot = nullptr;
  if (!snapshot) snapshot = (LogLine *)psramCalloc(APP_LOG_CAPACITY, sizeof(LogLine));
  const String filter = configServer.arg("f");          // "", "warn" or "err"
  const uint32_t window = 30UL * 60UL * 1000UL;
  const uint32_t nowMs = millis();
  const uint32_t since = nowMs > window ? nowMs - window : 0;
  const size_t n = snapshot ? appLogSnapshot(snapshot, APP_LOG_CAPACITY, since) : 0;
  size_t errors = 0, warnings = 0;
  for (size_t i = 0; i < n; ++i) {
    if (snapshot[i].level == LogLevel::ERROR) ++errors;
    else if (snapshot[i].level == LogLevel::WARN) ++warnings;
  }
  const time_t nowEpoch = time(nullptr);
  const bool haveClock = nowEpoch > 1700000000;

  configServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  configServer.send(200, "text/html; charset=utf-8", "");
  String chunk = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Log</title>
<style>*{box-sizing:border-box}body{margin:0;background:#0b1424;color:#d9e4f2;font:14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px}
main{max-width:900px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.muted{color:#8fa6c2}.bar{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}
.bar a,.bar label{padding:8px 12px;border-radius:10px;background:#ffffff14;color:#d9e4f2;text-decoration:none;font-weight:600}.bar a.on{background:#18a4ef;color:#071525}
table{width:100%;border-collapse:collapse;font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace}td{padding:5px 8px;border-bottom:1px solid #ffffff10;vertical-align:top}
td.t{color:#7f93ad;white-space:nowrap}td.m{word-break:break-word}tr.e td.m{color:#ff6b6b}tr.w td.m{color:#ffd477}
.b{display:inline-block;min-width:44px;text-align:center;border-radius:6px;padding:1px 6px;font-size:11px;font-weight:700}.b.e{background:#ff6b6b33;color:#ff8f8f}.b.w{background:#ffd47733;color:#ffd477}.b.i{background:#ffffff14;color:#9fb6d0}
.empty{padding:24px;text-align:center;color:#8fa6c2}</style>
<main><h1>Log da aplicação</h1><div class="muted">Últimos 30 minutos · mais recentes primeiro · %COUNT% linhas (%ERR% erros, %WARN% avisos)</div>
<div class="bar"><a href="/">&larr; Ajustes</a><a href="/log" class="%ALL%">Tudo</a><a href="/log?f=warn" class="%FW%">Avisos e erros</a><a href="/log?f=err" class="%FE%">Só erros</a>
<label><input type="checkbox" id="auto" onchange="tick()"> Atualizar a cada 5 s</label></div><table>)HTML";
  chunk.replace("%COUNT%", String((unsigned)n));
  chunk.replace("%ERR%", String((unsigned)errors));
  chunk.replace("%WARN%", String((unsigned)warnings));
  chunk.replace("%ALL%", filter.isEmpty() ? "on" : "");
  chunk.replace("%FW%", filter == "warn" ? "on" : "");
  chunk.replace("%FE%", filter == "err" ? "on" : "");
  configServer.sendContent(chunk);

  size_t shown = 0;
  chunk = "";
  for (size_t k = n; k-- > 0;) {
    const LogLine &l = snapshot[k];
    if (filter == "err" && l.level != LogLevel::ERROR) continue;
    if (filter == "warn" && l.level == LogLevel::INFO) continue;
    char when[16];
    const uint32_t agoS = (nowMs - l.ms) / 1000;
    if (haveClock) {
      const time_t at = nowEpoch - agoS;
      struct tm tmv;
      localtime_r(&at, &tmv);
      strftime(when, sizeof when, "%H:%M:%S", &tmv);
    } else {
      snprintf(when, sizeof when, "-%lus", (unsigned long)agoS);
    }
    const char *cls = l.level == LogLevel::ERROR ? "e" : l.level == LogLevel::WARN ? "w" : "i";
    const char *tag = l.level == LogLevel::ERROR ? "ERRO" : l.level == LogLevel::WARN ? "AVISO" : "info";
    chunk += "<tr class=\"";
    chunk += cls;
    chunk += "\"><td class=\"t\">";
    chunk += when;
    chunk += "</td><td><span class=\"b ";
    chunk += cls;
    chunk += "\">";
    chunk += tag;
    chunk += "</span></td><td class=\"m\">";
    chunk += htmlEscape(l.text);
    chunk += "</td></tr>";
    ++shown;
    if (chunk.length() > 3000) { configServer.sendContent(chunk); chunk = ""; }
  }
  if (!shown) chunk += "<tr><td class=\"empty\" colspan=\"3\">Nada registrado neste período.</td></tr>";
  chunk += R"HTML(</table></main><script>
const a=document.getElementById('auto');a.checked=location.hash=='#auto';let t;
function tick(){clearTimeout(t);if(a.checked){history.replaceState(null,'','#auto');t=setTimeout(()=>location.reload(),5000)}else history.replaceState(null,'',location.pathname+location.search)}
tick();</script></html>)HTML";
  configServer.sendContent(chunk);
  configServer.sendContent("");   // end of chunked response
}

static void startConfigServer() {
  configServer.on("/", HTTP_GET, []() { sendConfigPage(""); });
  configServer.on("/save-city", HTTP_POST, []() {
    String city = configServer.arg("city");
    city.trim();
    cfg.h24 = configServer.hasArg("h24");
    prefs.putBool("h24", cfg.h24);
    if (!city.isEmpty()) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;
      sh.status = "Buscando cidade...";
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
    }
    sendConfigPage(city.isEmpty() ? "Formato de hora salvo." : "Ajustes salvos; atualizando a previsão.");
  });
  configServer.on("/save-radar", HTTP_POST, []() {
    String airport = configServer.arg("airport");
    airport.trim();
    const int radius = configServer.arg("radius").toInt();
    char currentCode[8];
    uint8_t currentRadius = 0;
    getFlightRadarSettings(currentCode, sizeof currentCode, nullptr, 0, currentRadius);
    // Only apply what changed (each setter clears the radar and re-queries).
    const bool airportOk = airport.equalsIgnoreCase(currentCode) || setFlightRadarAirportCode(airport.c_str());
    const bool radiusOk = radius == currentRadius || setFlightRadarRadiusKm((uint8_t)radius);
    String notice;
    if (!airportOk) notice = String("Aeroporto \"") + airport + "\" não reconhecido. Use um da lista (ex.: CGH, GRU, SBGR).";
    else if (!radiusOk) notice = "Raio inválido.";
    else notice = String("Radar salvo: ") + airport + ", " + String(radius) + " km.";
    sendConfigPage(notice);
  });
  configServer.on("/save-finance", HTTP_POST, []() {
    FinanceAsset input[FINANCE_MAX_ASSETS];
    uint8_t count = 0;
    String invalid;
    for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) {
      String sym = configServer.arg(String("sym") + String(i));
      String lbl = configServer.arg(String("lbl") + String(i));
      sym.trim();
      lbl.trim();
      if (sym.isEmpty()) continue;
      FinanceAsset a;
      if (!normalizeFinanceSymbol(sym.c_str(), a.symbol, sizeof a.symbol)) {
        invalid += String(invalid.isEmpty() ? "" : ", ") + sym;
        continue;
      }
      snprintf(a.label, sizeof a.label, "%s", lbl.c_str());
      input[count++] = a;
    }
    String notice;
    if (!count) notice = "Nenhum símbolo válido; lista mantida.";
    else if (!setFinanceAssets(input, count)) notice = "Não foi possível salvar a lista.";
    else {
      notice = String("Finance salvo com ") + String(count) + (count == 1 ? " ativo." : " ativos.");
      if (activeScreenId() == SCREEN_FINANCE) postScreenJob(CMD_FINANCE);
      else notice += " Abra a tela Finance para consultar.";
    }
    if (!invalid.isEmpty()) notice += " Ignorados (caracteres inválidos): " + invalid + ".";
    sendConfigPage(notice);
  });
  configServer.on("/save-servers", HTTP_POST, []() {
    String user = configServer.arg("bzuser"); user.trim();
    String pass = configServer.arg("bzpass");
    serversSetCredentials(user.c_str(), pass.c_str());
    String notice = "Servidores salvos.";
    for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
      String lbl = configServer.arg("bzlbl" + String(i)); lbl.trim();
      char url[64] = "", id[24] = "";
      if (!serversParseLink(configServer.arg("bzlink" + String(i)).c_str(), url, sizeof url, id, sizeof id)) continue;
      if (!id[0]) notice += " Servidor " + String(i + 1) + ": link sem /system/ID (ID mantido).";
      serversSetServer(i, lbl.c_str(), url, id);
    }
    if (activeScreenId() == SCREEN_SERVERS) postScreenJob(CMD_SERVERS);
    else notice += " Abra a tela Servers para consultar.";
    sendConfigPage(notice);
  });
  configServer.on("/save-display", HTTP_POST, []() {
    const int mhz = configServer.arg("qspi").toInt();
    if (mhz == 40 || mhz == 60 || mhz == 80) prefs.putUInt("qspiHz", (uint32_t)mhz * 1000000UL);
    configServer.send(200, "text/html; charset=utf-8",
      "<meta http-equiv=refresh content='8;url=/'><body style='font-family:sans-serif;background:#0c1728;color:#fff;padding:30px'>"
      "Salvo. Reiniciando a placa com QSPI " + String(mhz) + " MHz...</body>");
    delay(400);
    ESP.restart();
  });
  configServer.on("/log", HTTP_GET, []() { sendLogPage(); });
  configServer.onNotFound([]() { configServer.send(404, "text/plain; charset=utf-8", "Nao encontrado"); });
  configServer.begin();
  configServerStarted = true;
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

static void showMenuEvent(lv_event_t *) { showView(mainMenu); }
static void showWeatherEvent(lv_event_t *) { showView(weatherView); }
static void showSettingsEvent(lv_event_t *) { showView(settingsView); }

static void showFinanceEvent(lv_event_t *) { showView(financeView); }
static void showCalendarEvent(lv_event_t *) { showView(calendarView); }
static void showFlightRadarEvent(lv_event_t *) { showView(flightRadarView); }
static void showServersEvent(lv_event_t *) { showView(serversView); }
static void showClockEvent(lv_event_t *) { showView(clockView); }

// iOS-style app icon: 64 px squircle-ish tile, vertical gradient (lighter on top),
// 1 px top highlight, soft drop shadow, press = shrink to 92 % + darken.
static constexpr int TILE_W = 112, TILE_H = 94, ICON = 72;

static void noClickBox(lv_obj_t *o) {
  lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- hand-drawn app glyphs (shapes only, no bitmaps) --------------------------

static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius = LV_RADIUS_CIRCLE,
                      lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *o = lv_obj_create(p); noClickBox(o);
  lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0); lv_obj_set_style_bg_opa(o, opa, 0);
  return o;
}
static lv_obj_t *polyline(lv_obj_t *p, const lv_point_precise_t *pts, int n, int width, uint32_t color) {
  lv_obj_t *l = lv_line_create(p);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_pos(l, 0, 0);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}
static void iconBg(lv_obj_t *box, uint32_t top, uint32_t bottom) {
  lv_obj_set_style_bg_color(box, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(box, lv_color_hex(bottom), 0);
}

static void drawAppGlyph(lv_obj_t *box, const char *app, const char *symbol) {
  if (!strcmp(app, "Clima")) {                       // sun behind a cloud
    iconBg(box, 0x4FB3FF, 0x1A6FE0);
    blob(box, 34, 12, 24, 24, 0xFFD43B);
    blob(box, 14, 34, 22, 22, 0xFFFFFF); blob(box, 26, 26, 26, 26, 0xFFFFFF);
    blob(box, 42, 34, 18, 18, 0xFFFFFF); blob(box, 14, 42, 46, 14, 0xFFFFFF, 7);
  } else if (!strcmp(app, "Finanças")) {             // rising chart
    iconBg(box, 0x2FD27A, 0x0E9F54);
    static const lv_point_precise_t up[] = {{14, 52}, {28, 38}, {38, 44}, {56, 22}};
    static const lv_point_precise_t ah[] = {{46, 22}, {56, 22}, {56, 32}};
    polyline(box, up, 4, 5, 0xFFFFFF); polyline(box, ah, 3, 5, 0xFFFFFF);
    blob(box, 12, 56, 48, 3, 0xFFFFFF, 2, LV_OPA_50);
  } else if (!strcmp(app, "Servidores")) {           // 3-unit rack with LEDs
    iconBg(box, 0x3A4558, 0x161B24);
    for (int r = 0; r < 3; ++r) {
      lv_obj_t *u = blob(box, 14, 14 + r * 16, 44, 12, 0xDDE3EC, 4);
      lv_obj_set_style_bg_grad_color(u, lv_color_hex(0xA9B3C2), 0); lv_obj_set_style_bg_grad_dir(u, LV_GRAD_DIR_VER, 0);
      blob(u, 6, 4, 4, 4, r == 2 ? 0xFFB020 : 0x2EE07A);
      blob(u, 14, 5, 22, 2, 0x6B7585, 1);
    }
  } else if (!strcmp(app, "Ajustes")) {              // iOS-like grey gear
    iconBg(box, 0xB7BDC6, 0x7D848F);
    lv_obj_t *g = lv_label_create(box); lv_label_set_text(g, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g, lv_color_hex(0x3B4049), 0); lv_obj_center(g);
  } else if (!strcmp(app, "Calendário")) {           // live: weekday + day number
    iconBg(box, 0xFFFFFF, 0xF1F2F5);
    menuCalWeek = lv_label_create(box); lv_label_set_text(menuCalWeek, "SEG");
    lv_obj_set_style_text_font(menuCalWeek, &lv_font_weather_14, 0);
    lv_obj_set_style_text_color(menuCalWeek, lv_color_hex(0xF23B38), 0);
    lv_obj_align(menuCalWeek, LV_ALIGN_TOP_MID, 0, 6);
    menuCalDay = lv_label_create(box); lv_label_set_text(menuCalDay, "--");
    lv_obj_set_style_text_font(menuCalDay, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(menuCalDay, lv_color_hex(0x15171C), 0);
    lv_obj_align(menuCalDay, LV_ALIGN_CENTER, 0, 8);
  } else if (!strcmp(app, "Radar")) {                // rings + sweep + blips
    iconBg(box, 0x1FA27A, 0x0A4F3C);
    for (int i = 0; i < 3; ++i) {
      const int d = 56 - i * 18;
      lv_obj_t *ring = blob(box, (ICON - d) / 2, (ICON - d) / 2, d, d, 0, LV_RADIUS_CIRCLE, LV_OPA_TRANSP);
      lv_obj_set_style_border_width(ring, 2, 0);
      lv_obj_set_style_border_color(ring, lv_color_hex(0xB9FBE0), 0);
      lv_obj_set_style_border_opa(ring, i == 0 ? LV_OPA_70 : LV_OPA_40, 0);
    }
    static const lv_point_precise_t sw[] = {{36, 36}, {56, 18}};
    polyline(box, sw, 2, 3, 0xE9FFF6);
    blob(box, 33, 33, 6, 6, 0xFFFFFF);
    blob(box, 46, 42, 5, 5, 0xFFE066); blob(box, 20, 22, 4, 4, 0xFFE066);
  } else if (!strcmp(app, "Relógio")) {              // white face, 10:10 hands
    iconBg(box, 0x22252B, 0x0B0C0F);
    lv_obj_t *face = blob(box, 8, 8, 56, 56, 0xFFFFFF);
    for (int k = 0; k < 12; ++k) {
      const float a = k * 0.5235988f;
      blob(face, 27 + (int)lroundf(sinf(a) * 23), 27 - (int)lroundf(cosf(a) * 23), 2, 2, 0x2A2D34, 1);
    }
    static const lv_point_precise_t hh[] = {{28, 28}, {17, 21}}, mh[] = {{28, 28}, {44, 17}};
    polyline(face, hh, 2, 4, 0x15171C); polyline(face, mh, 2, 3, 0x15171C);
    blob(face, 25, 25, 6, 6, 0xF49B35);
  } else if (!strcmp(app, "Fotos")) {                // 8-petal colour flower
    iconBg(box, 0xFFFFFF, 0xF3F4F7);
    static const uint32_t pc[8] = {0xFF9F0A, 0xFFD60A, 0x8FD14F, 0x34C759, 0x32ADE6, 0x5E5CE6, 0xBF5AF2, 0xFF375F};
    for (int k = 0; k < 8; ++k) {
      const float a = k * 0.7853982f;
      blob(box, 26 + (int)lroundf(sinf(a) * 14), 26 - (int)lroundf(cosf(a) * 14), 20, 20, pc[k], LV_RADIUS_CIRCLE, LV_OPA_80);
    }
  } else {
    lv_obj_t *icon = lv_label_create(box); lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0); lv_obj_center(icon);
  }
}

static lv_obj_t *menuTile(lv_obj_t *parent, const char *symbol, const char *caption,
                          uint32_t tileColor, lv_event_cb_t callback) {
  lv_obj_t *tile = callback ? lv_button_create(parent) : lv_obj_create(parent);
  lv_obj_set_size(tile, TILE_W, TILE_H);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_shadow_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  // press feedback: scale the whole tile around its centre
  lv_obj_set_style_transform_pivot_x(tile, TILE_W / 2, 0);
  lv_obj_set_style_transform_pivot_y(tile, ICON / 2, 0);
  lv_obj_set_style_transform_scale(tile, 236, LV_STATE_PRESSED);
  static lv_style_transition_dsc_t tr;
  static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_PROP_INV};
  static bool trInit = false;
  if (!trInit) { lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 90, 0, NULL); trInit = true; }
  lv_obj_set_style_transition(tile, &tr, 0);
  lv_obj_set_style_transition(tile, &tr, LV_STATE_PRESSED);
  if (callback) lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, NULL);
  else lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);

  const lv_color_t base = lv_color_hex(tileColor);
  lv_obj_t *iconBox = lv_obj_create(tile);
  lv_obj_set_size(iconBox, ICON, ICON);
  lv_obj_align(iconBox, LV_ALIGN_TOP_MID, 0, 0);
  noClickBox(iconBox);
  lv_obj_set_style_radius(iconBox, 17, 0);                       // ~23 % like iOS
  lv_obj_set_style_bg_color(iconBox, lv_color_mix(lv_color_white(), base, LV_OPA_20), 0);
  lv_obj_set_style_bg_grad_color(iconBox, lv_color_mix(lv_color_black(), base, LV_OPA_10), 0);
  lv_obj_set_style_bg_grad_dir(iconBox, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(iconBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(iconBox, LV_BORDER_SIDE_TOP, 0);    // glassy top highlight
  lv_obj_set_style_border_width(iconBox, 1, 0);
  lv_obj_set_style_border_color(iconBox, lv_color_white(), 0);
  lv_obj_set_style_border_opa(iconBox, LV_OPA_30, 0);
  lv_obj_set_style_shadow_width(iconBox, 14, 0);
  lv_obj_set_style_shadow_offset_y(iconBox, 4, 0);
  lv_obj_set_style_shadow_opa(iconBox, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color(iconBox, lv_color_hex(0x01040C), 0);

  drawAppGlyph(iconBox, caption, symbol);
  lv_obj_t *label = lv_label_create(tile);
  lv_label_set_text(label, caption);
  lv_obj_set_width(label, TILE_W);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, &lv_font_weather_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF4F7FF), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, ICON + 4);
  return tile;
}

static void build_main_menu(lv_obj_t *menu) {
  lv_obj_set_style_bg_color(menu, lv_color_hex(0x06163C), 0);
  lv_obj_set_style_bg_grad_color(menu, lv_color_hex(0x073B86), 0);
  lv_obj_set_style_bg_grad_dir(menu, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
  lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

  // status bar (clock left, Wi-Fi right) like the iPhone
  menuClock = makeLabel(menu, "--:--", &lv_font_weather_14, 0xEAF2FF);
  lv_obj_add_flag(menuClock, LV_OBJ_FLAG_HIDDEN);   // no status bar: more room between rows

  // 4 x 3 grid = 12 apps
  lv_obj_t *grid = lv_obj_create(menu);
  lv_obj_set_size(grid, 4 * TILE_W + 3 * 6, 3 * TILE_H + 2 * 11);
  lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_style_pad_row(grid, 11, 0);
  lv_obj_set_style_pad_column(grid, 6, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  menuTile(grid, "$", "Finanças", 0x0E9F54, showFinanceEvent);
  menuTile(grid, LV_SYMBOL_IMAGE, "Fotos", 0x39B982, NULL);
  {
    lv_obj_t *srv = menuTile(grid, LV_SYMBOL_DRIVE, "Servidores", 0x2E3A4C, showServersEvent);
    menuServersBadge = lv_obj_create(srv);   // red dot when a server is offline
    lv_obj_set_size(menuServersBadge, 16, 16); lv_obj_align(menuServersBadge, LV_ALIGN_TOP_RIGHT, -12, -4);
    lv_obj_set_style_radius(menuServersBadge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(menuServersBadge, lv_color_hex(0xF0453A), 0);
    lv_obj_set_style_border_color(menuServersBadge, lv_color_hex(0x06163C), 0);
    lv_obj_set_style_border_width(menuServersBadge, 2, 0);
    lv_obj_remove_flag(menuServersBadge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(menuServersBadge, LV_OBJ_FLAG_HIDDEN);
  }
  menuTile(grid, LV_SYMBOL_SETTINGS, "Ajustes", 0x8993A3, showSettingsEvent);
  menuTile(grid, "", "Calendário", 0xF8F8FA, showCalendarEvent);
  menuTile(grid, "", "Radar", 0x126E58, showFlightRadarEvent);
  menuTile(grid, LV_SYMBOL_TINT, "Clima", 0x078BDD, showWeatherEvent);
  menuTile(grid, LV_SYMBOL_HOME, "Relógio", 0x22252B, showClockEvent);
  menuWifiIcon = makeLabel(menu, LV_SYMBOL_WIFI, &lv_font_weather_14, 0xEAF2FF);
  lv_obj_align(menuWifiIcon, LV_ALIGN_TOP_RIGHT, -3, 3);
}

lv_obj_t *addBackButton(lv_obj_t *parent) {
  lv_obj_t *back = lv_button_create(parent);
  lv_obj_set_size(back, 40, 34);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x0878B8), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(back, lv_color_hex(0x07528A), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(back, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(back, 10, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back, 3, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(back, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(back, showMenuEvent, LV_EVENT_CLICKED, NULL);
  lv_obj_t *arrow = lv_label_create(back);
  lv_label_set_text(arrow, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(arrow, &lv_font_weather_14, 0);
  lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(arrow);
  lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
  return back;
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

  mainMenu = lv_obj_create(scr);
  lv_obj_set_size(mainMenu, SCR_W, SCR_H);
  lv_obj_set_pos(mainMenu, 0, 0);
  lv_obj_set_style_pad_all(mainMenu, 0, 0);
  lv_obj_set_style_border_width(mainMenu, 0, 0);
  build_main_menu(mainMenu);

  buildFinanceScreen(scr);
  buildCalendarScreen(scr);
  buildWeatherScreen(scr);
  buildServersScreen(scr);
  buildClockScreen(scr);
  flightRadarView = lv_obj_create(scr);
  buildFlightRadarScreen(flightRadarView);
  settingsView = lv_obj_create(scr);
  buildConfigScreen(settingsView);
  lv_obj_add_flag(weatherView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(flightRadarView, LV_OBJ_FLAG_HIDDEN);
  setFlightRadarRefreshRequester(requestFlightRadar);
  setFlightRadarDetailsRequester(requestFlightDetails);
  buildConfigKeyboard();

  contextTimer = lv_timer_create(contextTimerCb, SCREEN_ENTER_DEBOUNCE_MS, NULL);
  lv_timer_pause(contextTimer);
  lv_timer_create(ui_update, 500, NULL);
  switchScreen(SCREEN_MENU);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  appLogBegin();   // keep the last minutes of log in PSRAM for http://IP/log
  delay(1000);
  Log.println("NextNeo Vista boot");

  mtx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(NetJob));

  prefs.begin("clock", false);
  cfg.lat = prefs.getFloat("lat", cfg.lat);
  cfg.lon = prefs.getFloat("lon", cfg.lon);
  cfg.utcOffset = prefs.getInt("tz", cfg.utcOffset);
  cfg.h24 = prefs.getBool("h24", true);
  cfg.bright = prefs.getInt("bl", 100);
  sh.cityName = prefs.getString("city", "São Paulo");
  if (sh.cityName == "Sao Paulo") {
    sh.cityName = "São Paulo";
    prefs.putString("city", sh.cityName);
  }

  loadFinanceAssets();   // Finance symbols (editable on the web page)
  loadServersConfig();   // Beszel hubs + login (Settings / web page)

  gfx->setRotation(1); // Landscape UI in the canvas; keep AXS15231B native portrait mode.
  // QSPI clock is selectable on the web page (40/60/80 MHz); default 40 MHz.
  qspiHz = prefs.getUInt("qspiHz", 40000000);
  if (qspiHz != 40000000 && qspiHz != 60000000 && qspiHz != 80000000) qspiHz = 40000000;
  if (!gfx->begin(qspiHz)) {
    Log.println("gfx->begin FAILED (display or framebuffer allocation)");
  }
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  detectFramebufferMapping();
  startAsyncSend();
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  const size_t buf_bytes = SCR_W * 40 * sizeof(uint16_t);
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_RENDER_READY, NULL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);

  build_ui();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
    setStatus("Connecting to " + ssid + "...");
  }
  xTaskCreatePinnedToCore(workerTask, "worker", 12288, NULL, 1, NULL, 0);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && !configServerStarted) startConfigServer();
  if (configServerStarted) configServer.handleClient();
  const uint32_t idle = lv_timer_handler();      // ms until the next LVGL timer
  delay(idle > 5 ? 5 : idle ? idle : 1);
}
