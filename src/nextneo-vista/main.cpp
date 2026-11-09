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
  Serial.println("Finance: conexão TLS liberada");
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
    Serial.printf("Finance: erro de transporte %d (%s)\n", status, HTTPClient::errorToString(status).c_str());
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
    Serial.printf("Finance %s HTTP %d (tentativa %d/2)\n", what, status, attempt + 1);
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
    Serial.printf("Finance %s: resposta JSON inválida\n", symbol);
    return false;
  }
  if (!parseYahooResult(doc["chart"]["result"][0], q)) {
    Serial.printf("Finance %s: símbolo inexistente ou sem cotação\n", symbol);
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
  if (deserializeJson(doc, financeBody.c_str(), financeBody.length())) { Serial.println("Finance spark: resposta JSON inválida"); return false; }
  for (JsonVariantConst item : doc["spark"]["result"].as<JsonArrayConst>()) {
    const char *symbol = item["symbol"] | "";
    for (uint8_t i = 0; i < count; ++i)
      if (!strcmp(symbol, assets[i].symbol)) { parseYahooResult(item["response"][0], quotes[i]); break; }
  }
  for (uint8_t i = 0; i < count; ++i)
    if (!quotes[i].ok) Serial.printf("Finance spark: símbolo %s ausente/sem cotação\n", assets[i].symbol);
  return true;
}

// Runs on the network task only, and only for the active Finance screen.
// onlyIndex = -1 refreshes every asset; otherwise just that card.
static bool doFinance(int onlyIndex) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if ((int32_t)(millis() - financeBlockedUntilMs) < 0) {
    Serial.println("Finance: aguardando Retry-After da API");
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
      if (!q.ok) { Serial.printf("Finance %s: atualização falhou; mantendo última cotação válida\n", assets[i].symbol); continue; }
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
      Serial.printf("Weather: %s\n", error.c_str());
      return false;
    }
    error = code <= 0 ? String("erro de conexão: ") + HTTPClient::errorToString(code)
                      : String("HTTP ") + code;
    Serial.printf("Weather: %s (tentativa %d/2)\n", error.c_str(), attempt + 1);
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
               "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m"
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
    Serial.println("Weather: resposta incompleta da API");
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
      default: ran = false; break;
    }
  }
  if (job.screen != SCREEN_NONE) {
    if (!ran) {
      Serial.printf("[net] %s descartado (tela inativa)\n", cmdName(job.cmd));
    } else if (screenJobStillCurrent()) {
      gJobOutcome[job.screen] = ok ? JOB_OK : JOB_FAILED;
      Serial.printf("[net] %s %s em %lums (interno livre %u, maior bloco %u)\n", cmdName(job.cmd),
                    ok ? "ok" : "falhou", (unsigned long)(millis() - started),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
      Serial.printf("[net] %s concluído fora de contexto; resultado ignorado\n", cmdName(job.cmd));
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
        Serial.printf("Wi-Fi offline; retrying saved network: %s\n", savedSsid.c_str());
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

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  if (lv_display_flush_is_last(disp)) gfx->flush();
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
    renderedWifiUp = up;
    renderedWifiKnown = true;
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
  setLabelIfChanged(lblStatus, status.c_str());
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
};
static_assert(sizeof(kScreens) / sizeof(kScreens[0]) == SCREEN_COUNT, "kScreens must list every ScreenID in order");

static lv_timer_t *contextTimer = nullptr;   // the single API timer, owned by the active screen

static const ScreenLifecycle &screenInfo(ScreenID id) {
  return kScreens[id < SCREEN_COUNT ? id : SCREEN_NONE];
}

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
  Serial.printf("[ctx] %s -> %s\n", old.name, next.name);
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
.chip{width:auto;margin:0;padding:6px 10px;border-radius:999px;background:#ffffff14;color:#cfe2f7;font-size:13px;font-weight:600}code{color:#9fe3ff}</style>
<main>
<div class="card"><h1>NextNeo Vista</h1><div class="muted">Ajustes do aparelho</div>
<div class="status %NOTICECLASS%">%NOTICE%Wi-Fi: %STATUS%<br>Endereço: http://%IP%</div></div>

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
  return page;
}

static void sendConfigPage(const String &notice) {
  configServer.send(200, "text/html; charset=utf-8", configPage(notice));
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

static lv_obj_t *menuTile(lv_obj_t *parent, const char *symbol, const char *caption,
                          uint32_t tileColor, lv_event_cb_t callback) {
  lv_obj_t *tile = callback ? lv_button_create(parent) : lv_obj_create(parent);
  lv_obj_set_size(tile, 96, 104);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_radius(tile, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
  if (callback) lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, NULL);
  else lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *iconBox = lv_obj_create(tile);
  lv_obj_set_size(iconBox, 92, 78);
  lv_obj_align(iconBox, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(iconBox, lv_color_hex(tileColor), 0);
  lv_obj_set_style_bg_grad_color(iconBox, lv_color_mix(lv_color_white(),lv_color_hex(tileColor),LV_OPA_30), 0);
  lv_obj_set_style_bg_grad_dir(iconBox, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(iconBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(iconBox, 0, 0);
  lv_obj_set_style_radius(iconBox, 18, 0);
  lv_obj_set_style_shadow_width(iconBox, 9, 0);
  lv_obj_set_style_shadow_opa(iconBox, LV_OPA_20, 0);
  lv_obj_set_style_shadow_color(iconBox, lv_color_hex(0x020817), 0);
  lv_obj_set_style_pad_all(iconBox, 0, 0);
  lv_obj_remove_flag(iconBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(iconBox, LV_OBJ_FLAG_CLICKABLE);
  if (strcmp(caption, "Calendário") == 0) {
    lv_obj_t *page = lv_obj_create(iconBox); lv_obj_set_size(page, 62, 68); lv_obj_align(page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF8F8FA), 0); lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(page, 14, 0); lv_obj_set_style_border_color(page, lv_color_hex(0xD9DDE5), 0); lv_obj_set_style_border_width(page, 1, 0); lv_obj_set_style_pad_all(page, 0, 0); lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE); lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *cap = lv_obj_create(page); lv_obj_set_size(cap, 62, 18); lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0xF23B38), 0); lv_obj_set_style_bg_grad_color(cap, lv_color_hex(0xD90018), 0); lv_obj_set_style_bg_grad_dir(cap, LV_GRAD_DIR_VER, 0); lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0); lv_obj_set_style_radius(cap, 14, 0); lv_obj_set_style_border_width(cap, 0, 0); lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    for(int r=0;r<3;r++) for(int c=0;c<5;c++){ lv_obj_t *dot=lv_obj_create(page); lv_obj_set_size(dot,5,5); lv_obj_set_pos(dot,10+c*10,28+r*10); lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(dot,lv_color_hex(0x111318),0); lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0); lv_obj_set_style_border_width(dot,0,0); lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE); }
    lv_obj_t *todayDot=lv_obj_create(page); lv_obj_set_size(todayDot,9,9); lv_obj_set_pos(todayDot,35,46); lv_obj_set_style_radius(todayDot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(todayDot,lv_color_hex(0xF23B38),0); lv_obj_set_style_bg_opa(todayDot,LV_OPA_COVER,0); lv_obj_set_style_border_width(todayDot,0,0); lv_obj_remove_flag(todayDot,LV_OBJ_FLAG_CLICKABLE);
  } else if (strcmp(caption, "Radar") == 0) {
    for (int i = 0; i < 3; ++i) {
      lv_obj_t *ring = lv_obj_create(iconBox);
      int d = 46 - i * 14;
      lv_obj_set_size(ring, d, d);
      lv_obj_center(ring);
      lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_color(ring, lv_color_hex(i == 0 ? 0xD8FFF0 : 0x87E8BF), 0);
      lv_obj_set_style_border_width(ring, 2, 0);
      lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
      lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *dot = lv_obj_create(iconBox);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_center(dot);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_t *icon = lv_label_create(iconBox); lv_label_set_text(icon, symbol); lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0); lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0); lv_obj_center(icon);
  }
  lv_obj_t *label = lv_label_create(tile);
  lv_label_set_text(label, caption);
  lv_obj_set_style_text_font(label, &lv_font_weather_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF4F7FF), 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -1);
  return tile;
}

static void build_main_menu(lv_obj_t *menu) {
  lv_obj_set_style_bg_color(menu, lv_color_hex(0x06163C), 0);
  lv_obj_set_style_bg_grad_color(menu, lv_color_hex(0x073B86), 0);
  lv_obj_set_style_bg_grad_dir(menu, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
  lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = makeLabel(menu, "", &lv_font_weather_20, 0xFFFFFF);
  lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *grid = lv_obj_create(menu);
  lv_obj_set_size(grid, 436, 220);
  lv_obj_align(grid, LV_ALIGN_CENTER, 0, 4);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_style_pad_row(grid, 15, 0);
  lv_obj_set_style_pad_column(grid, 17, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  menuTile(grid, "$", "Finance", 0x151820, showFinanceEvent);
  menuTile(grid, LV_SYMBOL_IMAGE, "Photo", 0x39B982, NULL);
  menuTile(grid, LV_SYMBOL_PLAY, "Video", 0x7353E8, NULL);
  menuTile(grid, LV_SYMBOL_SETTINGS, "Setting", 0x8993A3, showSettingsEvent);
  menuTile(grid, "", "Calendário", 0xF8F8FA, showCalendarEvent);
  menuTile(grid, "", "Radar", 0x126E58, showFlightRadarEvent);
  menuTile(grid, LV_SYMBOL_TINT, "Weather", 0x078BDD, showWeatherEvent);
  menuTile(grid, LV_SYMBOL_HOME, "Clock", 0xF49B35, NULL);
  menuWifiIcon = makeLabel(menu, LV_SYMBOL_WIFI, &lv_font_weather_14, 0xEAF2FF);
  lv_obj_align(menuWifiIcon, LV_ALIGN_TOP_RIGHT, -15, 7);
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
  delay(1000);
  Serial.println("NextNeo Vista boot");

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

  gfx->setRotation(1); // Landscape UI in the canvas; keep AXS15231B native portrait mode.
  if (!gfx->begin(40000000)) {
    Serial.println("gfx->begin FAILED (display or framebuffer allocation)");
  }
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  const size_t buf_bytes = SCR_W * 40 * sizeof(uint16_t);
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
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
  lv_timer_handler();
  delay(5);
}
