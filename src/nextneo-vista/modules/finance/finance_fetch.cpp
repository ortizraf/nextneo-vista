// Finance network jobs (network task only): Yahoo Finance spark/chart quotes and the
// on-demand detail chart. One keep-alive TLS session per Finance visit.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>
#include "app_screens.h"
#include "modules/finance/finance.h"
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

FinanceData fin;  // owned by this module; read by the Finance screen / web page (under mtx)

static uint32_t financeRetryAfterMs[FINANCE_MAX_ASSETS] = {};
volatile int8_t financeSingleIndex = -1;
static uint32_t financeBlockedUntilMs = 0;   // honours HTTP 429 Retry-After
static constexpr uint32_t FINANCE_MAX_BACKOFF_MS = 16UL * 60 * 1000;

// One keep-alive TLS session to Yahoo for the whole Finance visit: the spark
// call and any per-symbol fallbacks reuse it instead of doing up to four full
// handshakes (each needs ~40 KB of contiguous internal RAM). Released when the
// user leaves the Finance screen.
static WiFiClientSecure financeClient;
static bool financeClientOpen = false;
static PsramBuffer financeBody(256 * 1024);

void financeReleaseConnection() {
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
  float series[FINANCE_HISTORY] = {};
  uint8_t count = 0;
  char currency[4] = {};
};

static void appendPoint(Quote &q, float x) {
  if (q.count < FINANCE_HISTORY) q.series[q.count++] = x;
  else { memmove(q.series, q.series + 1, sizeof(float) * (FINANCE_HISTORY - 1)); q.series[FINANCE_HISTORY - 1] = x; }
}

// Evenly picks up to `maxOut` of the valid (> 0) values of a Yahoo close[] array, always
// keeping the first and the last one, so a sparkline covers the whole period instead of
// only its last candles. Returns how many were written. min/max (optional) see every value.
static int downsampleCloses(JsonArrayConst arr, float *out, int maxOut, float *lo = nullptr, float *hi = nullptr) {
  int n = 0;
  for (JsonVariantConst v : arr) if ((v | 0.0f) > 0) ++n;
  if (!n) return 0;
  const int m = n < maxOut ? n : maxOut;
  int k = 0, j = 0, next = 0, written = 0;
  for (JsonVariantConst v : arr) {
    const float x = v | 0.0f;
    if (x <= 0) continue;
    if (lo && (k == 0 || x < *lo)) *lo = x;
    if (hi && (k == 0 || x > *hi)) *hi = x;
    if (j < m && k == next) {
      out[written++] = x; ++j;
      next = m > 1 ? (int)((int64_t)j * (n - 1) / (m - 1)) : n;
    }
    ++k;
  }
  return written;
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
  q.count = (uint8_t)downsampleCloses(result["indicators"]["quote"][0]["close"].as<JsonArrayConst>(), q.series, FINANCE_HISTORY);
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
  const uint8_t count = fin.assetCount;
  const uint32_t configVersion = fin.configVersion;
  memcpy(assets, fin.assets, sizeof assets);
  fin.refreshing = true;
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
  if (fin.configVersion == configVersion) {
    for (uint8_t i = 0; i < count; ++i) {
      if (onlyIndex >= 0 && i != onlyIndex) continue;
      Quote &q = quotes[i];
      fin.apiKnown[i] = true;
      fin.apiOk[i] = q.ok;
      if (!q.ok) { Log.printf("Finance %s: atualização falhou; mantendo última cotação válida\n", assets[i].symbol); continue; }
      anySuccess = true;
      financeRetryAfterMs[i] = 0;
      fin.previousClose[i] = q.previousClose; fin.changePct[i] = q.changePct;
      fin.haveChange[i] = fabsf(q.changePct) > 0.0001f;
      fin.price[i] = q.price; fin.havePrice[i] = true;
      fin.quoteEpoch[i] = q.epoch;
      if (q.currency[0]) memcpy(fin.currency[i], q.currency, sizeof fin.currency[i]);
      // Without intraday candles, keep the graph useful with previous close + quote.
      if (q.count < 2 && q.previousClose > 0) { q.series[0] = q.previousClose; q.series[1] = q.price; q.count = 2; }
      if (q.count >= 2) { fin.historyCount[i] = q.count; memcpy(fin.history[i], q.series, sizeof q.series); }
    }
  }
  fin.refreshing = false;
  uint8_t totalSuccesses = 0;
  for (uint8_t i = 0; i < fin.assetCount; ++i) if (fin.apiKnown[i] && fin.apiOk[i]) totalSuccesses++;
  fin.successCount = totalSuccesses;
  if (anySuccess) fin.lastSuccessMs = millis();
  fin.version++;
  xSemaphoreGive(mtx);

  uint32_t retryAfter = 0;
  for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) { retryAfter = max(retryAfter, financeRetryAfterMs[i]); financeRetryAfterMs[i] = 0; }
  if (retryAfter) financeBlockedUntilMs = millis() + retryAfter;
  return anySuccess;
}

// ---------------------------------------------------------------- Finance detail (on demand)
FinanceDetail financeDetail;                  // network task -> UI (mtx)
FinanceDetailRequest financeDetailWant;       // UI -> network task (mtx)
static const char *const kFinRange[FIN_RANGES][2] = {{"1d", "5m"}, {"5d", "15m"}, {"1mo", "1d"}, {"6mo", "1d"}, {"1y", "1d"}};
static const uint32_t kFinRangeTtlMs[FIN_RANGES] = {60000UL, 300000UL, 1800000UL, 3600000UL, 3600000UL};

static bool parseFinanceDetail(JsonVariantConst r, uint8_t range, FinanceDetail &d) {
  JsonObjectConst meta = r["meta"];
  const float p = meta["regularMarketPrice"] | 0.0f;
  if (p <= 0 || !isfinite(p)) return false;
  d.price = p;
  d.dayHigh = meta["regularMarketDayHigh"] | NAN;
  d.dayLow = meta["regularMarketDayLow"] | NAN;
  d.wk52High = meta["fiftyTwoWeekHigh"] | NAN;
  d.wk52Low = meta["fiftyTwoWeekLow"] | NAN;
  d.rangeRef = meta["chartPreviousClose"] | NAN;
  snprintf(d.currency, sizeof d.currency, "%s", (const char *)(meta["currency"] | ""));
  JsonVariantConst quote = r["indicators"]["quote"][0];
  float lo = 0, hi = 0;
  d.count = (uint8_t)downsampleCloses(quote["close"].as<JsonArrayConst>(), d.series, FIN_DETAIL_POINTS, &lo, &hi);
  if (d.count && fabsf(d.series[d.count - 1] - p) > 0.0001f) {        // line ends at the live quote
    if (d.count < FIN_DETAIL_POINTS) d.series[d.count++] = p; else d.series[d.count - 1] = p;
    if (p < lo) lo = p;
    if (p > hi) hi = p;
  }
  if (!d.count) { d.series[0] = p; d.count = 1; lo = hi = p; }
  d.periodLow = lo; d.periodHigh = hi;
  d.open = NAN;
  if (range == 0) for (JsonVariantConst v : quote["open"].as<JsonArrayConst>()) { const float x = v | 0.0f; if (x > 0) { d.open = x; break; } }
  JsonArrayConst ts = r["timestamp"].as<JsonArrayConst>();
  d.t0 = ts.size() ? (uint32_t)(ts[0] | 0UL) : 0;
  d.t1 = ts.size() ? (uint32_t)(ts[ts.size() - 1] | 0UL) : 0;
  const char *type = meta["instrumentType"] | "";
  const time_t now = time(nullptr);
  d.market = FIN_MKT_UNKNOWN;
  if (!strcmp(type, "CRYPTOCURRENCY")) d.market = FIN_MKT_24H;
  else if (now > 1700000000) {
    JsonObjectConst tp = meta["currentTradingPeriod"];
    auto in = [&](const char *k) {
      const long a = tp[k]["start"] | 0L, b = tp[k]["end"] | 0L;
      return a && b && now >= a && now < b;
    };
    if (!tp.isNull()) d.market = in("regular") ? FIN_MKT_OPEN : in("pre") ? FIN_MKT_PRE : in("post") ? FIN_MKT_POST : FIN_MKT_CLOSED;
  }
  return true;
}

// Network task only. Answers financeDetailWant: from a small per-range cache (same
// symbol) when fresh enough, otherwise one chart request on the Finance keep-alive
// session. Always publishes an answer for the wanted symbol/range (data or failure),
// so the UI never re-posts in a loop.
static bool doFinanceDetail() {
  FinanceDetailRequest want;
  xSemaphoreTake(mtx, portMAX_DELAY); want = financeDetailWant; xSemaphoreGive(mtx);
  if (!want.symbol[0] || want.range >= FIN_RANGES) return false;
  static FinanceDetail *cache = nullptr;                 // FIN_RANGES entries, PSRAM, allocated once
  static FinanceDetail *work = nullptr;
  static char cacheSymbol[16] = "";
  if (!cache) cache = (FinanceDetail *)psramCalloc(FIN_RANGES + 1, sizeof(FinanceDetail));
  if (!cache) return false;
  work = cache + FIN_RANGES;
  if (strcmp(cacheSymbol, want.symbol)) {
    for (int i = 0; i < FIN_RANGES; ++i) { cache[i].ok = false; cache[i].fetchedMs = 0; }
    snprintf(cacheSymbol, sizeof cacheSymbol, "%s", want.symbol);
  }
  FinanceDetail &e = cache[want.range];
  bool ok = e.ok && millis() - e.fetchedMs < kFinRangeTtlMs[want.range];
  if (!ok && WiFi.status() == WL_CONNECTED && (int32_t)(millis() - financeBlockedUntilMs) >= 0) {
    String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + yahooSymbolParam(want.symbol) +
                 "?range=" + kFinRange[want.range][0] + "&interval=" + kFinRange[want.range][1] +
                 "&includePrePost=false&nocache=" + String(millis());
    uint32_t retryAfterMs = 0;
    if (financeGetWithRetry(url, "detalhe", retryAfterMs, 12000)) {
      JsonDocument filter(SpiRamJsonAllocator::instance());
      JsonObject f = filter["chart"]["result"].add<JsonObject>();
      f["meta"] = true; f["timestamp"] = true;
      f["indicators"]["quote"].add<JsonObject>()["close"] = true;
      f["indicators"]["quote"][0]["open"] = true;
      JsonDocument doc(SpiRamJsonAllocator::instance());
      if (!deserializeJson(doc, financeBody.c_str(), financeBody.length(), DeserializationOption::Filter(filter))) {
        *work = FinanceDetail();
        if (parseFinanceDetail(doc["chart"]["result"][0], want.range, *work)) {
          if (want.range && cache[0].ok) work->open = cache[0].open;   // day open comes from the 1D data
          snprintf(work->symbol, sizeof work->symbol, "%s", want.symbol);
          work->range = want.range; work->ok = true; work->fetchedMs = millis();
          e = *work;
          ok = true;
        } else Log.printf("Finance detalhe %s: sem dados\n", want.symbol);
      } else Log.printf("Finance detalhe %s: JSON inválido\n", want.symbol);
    }
    if (retryAfterMs) financeBlockedUntilMs = millis() + retryAfterMs;
  }
  if (!screenJobStillCurrent()) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  const uint32_t v = financeDetail.version + 1;
  if (ok) financeDetail = e;
  else { financeDetail = FinanceDetail(); financeDetail.ok = false; }
  snprintf(financeDetail.symbol, sizeof financeDetail.symbol, "%s", want.symbol);
  financeDetail.range = want.range;
  financeDetail.version = v;
  xSemaphoreGive(mtx);
  return ok;
}

// ---- job entry points (see kFinanceJobs in finance_screen.cpp)
bool financeFetchAll() { return doFinance(-1); }
bool financeFetchOne() { return doFinance(financeSingleIndex); }
bool financeFetchDetail() { return doFinanceDetail(); }
