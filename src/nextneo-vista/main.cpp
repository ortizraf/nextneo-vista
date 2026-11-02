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
static uint32_t financeRetryAfterMs = 0;
static constexpr uint32_t FINANCE_REFRESH_MS = 240000UL;
static constexpr uint32_t FINANCE_MAX_BACKOFF_MS = 16UL * 60 * 1000;

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

static bool fetchYahoo(const char *symbol, float &price, float &changePct, float &previousClose, uint32_t &quoteEpoch, float *series, uint8_t &count) {
  String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + symbol + "?range=1d&interval=5m&nocache=" + String(millis());
  String body;
  int status = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(12000);
    if (!http.begin(client, url)) return false;
    const char *responseHeaders[] = {"Retry-After"};
    http.collectHeaders(responseHeaders, 1);
    http.addHeader("Cache-Control", "no-cache, no-store, max-age=0");
    http.addHeader("Pragma", "no-cache");
    http.addHeader("User-Agent", "Mozilla/5.0");
    status = http.GET();
    body = status == HTTP_CODE_OK ? http.getString() : "";
    if (status == HTTP_CODE_TOO_MANY_REQUESTS) {
      uint32_t retrySeconds = (uint32_t)http.header("Retry-After").toInt();
      if (retrySeconds) financeRetryAfterMs = max(financeRetryAfterMs, min(retrySeconds, FINANCE_MAX_BACKOFF_MS / 1000UL) * 1000UL);
    }
    http.end();
    if (status == HTTP_CODE_OK && !body.isEmpty()) break;
    Serial.printf("Finance %s HTTP %d (tentativa %d/2)\n", symbol, status, attempt + 1);
    if (attempt == 0 && (status <= 0 || status >= HTTP_CODE_INTERNAL_SERVER_ERROR)) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    return false;
  }
  if (body.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.printf("Finance %s: resposta JSON inválida\n", symbol);
    return false;
  }
  JsonObject meta=doc["chart"]["result"][0]["meta"].as<JsonObject>();
  float p=meta["regularMarketPrice"]|0.0f, prev=meta["previousClose"]|0.0f;
  float pct=meta["regularMarketChangePercent"]|NAN;
  uint32_t quoteTime=meta["regularMarketTime"]|0U;
  if (p <= 0 || !isfinite(p)) {
    Serial.printf("Finance %s: cotação ausente na resposta\n", symbol);
    return false;
  }
  if (!isfinite(pct) && prev > 0) pct = (p - prev) * 100.0f / prev;
  if (!isfinite(pct)) pct = 0;
  JsonArray close=doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
  uint8_t n=0; for(JsonVariant v:close){float x=v|0.0f;if(x>0){if(n<24)series[n++]=x;else {for(int k=1;k<24;k++)series[k-1]=series[k];series[23]=x;}}}
  price=p;changePct=pct;previousClose=prev;quoteEpoch=quoteTime;count=n;return true;
}

static uint8_t doFinance() {
  if (WiFi.status() != WL_CONNECTED) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.financeRefreshing = false;
    sh.financeSuccessCount = 0;
    sh.financeVersion++;
    xSemaphoreGive(mtx);
    return 0;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.financeRefreshing = true;
  xSemaphoreGive(mtx);
  const char *symbols[] = {"BRL=X", "EURBRL=X", "BTC-USD"};
  uint8_t successes = 0;
  for (int i=0; i<3; ++i) {
    float v=0,pct=0,prev=0,points[24]={0};uint32_t quoteEpoch=0;uint8_t n=0;bool ok=fetchYahoo(symbols[i],v,pct,prev,quoteEpoch,points,n);
    xSemaphoreTake(mtx, portMAX_DELAY);
    if(ok){
      successes++;
      sh.previousFinance[i]=prev;sh.financeChange[i]=pct;sh.haveChange[i]=fabsf(pct)>0.0001f;sh.finance[i]=v;sh.haveFinance[i]=true;
      sh.financeQuoteEpoch[i]=quoteEpoch;
      // When Yahoo omits intraday candles (often outside active FX hours), keep the
      // graph useful with the previous close and current quote. Never erase a good chart.
      if (n < 2 && prev > 0) { points[0] = prev; points[1] = v; n = 2; }
      if (n >= 2) { sh.historyCount[i]=n;memcpy(sh.history[i],points,sizeof points); }
    }
    else {
      // Keep the last valid quote and graph during temporary API/network failures.
      Serial.printf("Finance %s: atualização falhou; mantendo última cotação válida\n", symbols[i]);
    }
    xSemaphoreGive(mtx);
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.financeRefreshing = false;
  sh.financeSuccessCount = successes;
  if (successes) sh.financeLastSuccessMs = millis();
  sh.financeVersion++;
  xSemaphoreGive(mtx);
  return successes;
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

static bool httpGetJson(const String &url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();  // public read-only weather data; no secrets are sent
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getString());
  else Serial.printf("HTTP %d for %s\n", code, url.c_str());
  http.end();
  return ok;
}

// ---------------------------------------------------------------- worker task
static void applyTime() { configTime(cfg.utcOffset, 0, "pool.ntp.org", "time.google.com"); }

static void doWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m"
               "&hourly=temperature_2m,weather_code,precipitation_probability"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min"
               "&timezone=auto&forecast_days=7&forecast_hours=24";
  JsonDocument doc;
  if (!httpGetJson(url, doc)) { setStatus("Falha ao atualizar a previsão"); return; }

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
  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray hourlyTemp = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray hourlyCode = doc["hourly"]["weather_code"].as<JsonArray>();
  JsonArray hourlyRain = doc["hourly"]["precipitation_probability"].as<JsonArray>();
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
  sh.status = "Previsão atualizada";
  xSemaphoreGive(mtx);
}

static void doGeocode() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = sh.pendingCity;
  xSemaphoreGive(mtx);
  if (city.isEmpty() || WiFi.status() != WL_CONNECTED) { setStatus("Conecte ao Wi-Fi primeiro"); return; }
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
  JsonDocument doc;
  if (!httpGetJson(lookup, doc) ||
      doc["results"].isNull()) {
    setStatus("Cidade não encontrada");
    return;
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
  xSemaphoreGive(mtx);
  doWeather();
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
    doWeather();
  } else {
    setStatus("Falha ao conectar (verifique a senha)");
  }
}

static void workerTask(void *) {
  uint32_t lastWeather = 0;
  uint32_t lastFinance = 0;
  uint32_t financeIntervalMs = FINANCE_REFRESH_MS;
  uint8_t financeFailureStreak = 0;
  uint32_t lastReconnectCheck = millis();
  uint32_t reconnectStarted = 0;
  bool first = true;
  for (;;) {
    int cmd;
    if (xQueueReceive(cmdQ, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      switch (cmd) {
        case CMD_SCAN: doScan(); break;
        case CMD_CONNECT: doConnect(); lastWeather = millis(); break;
        case CMD_GEOCODE: doGeocode(); lastWeather = millis(); break;
        case CMD_WEATHER: doWeather(); lastWeather = millis(); break;
        case CMD_FINANCE: {
          uint8_t successes = doFinance();
          if (WiFi.status() == WL_CONNECTED) {
            lastFinance = millis();
            if (successes == 3) { financeIntervalMs = FINANCE_REFRESH_MS; financeFailureStreak = 0; financeRetryAfterMs = 0; }
            else {
              uint8_t shift = min(financeFailureStreak, (uint8_t)2);
              uint32_t backoff = min(FINANCE_REFRESH_MS << shift, FINANCE_MAX_BACKOFF_MS);
              financeFailureStreak++;
              financeIntervalMs = max(backoff, financeRetryAfterMs);
              financeRetryAfterMs = 0;
            }
          } else lastFinance = 0;
          break;
        }
      }
    }
    bool up = WiFi.status() == WL_CONNECTED;
    uint32_t now = millis();
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
    if (up && (first || millis() - lastWeather > 15UL * 60 * 1000)) {
      if (first) { applyTime(); setStatus("Conectado: " + WiFi.localIP().toString()); }
      first = false;
      doWeather();
      lastWeather = millis();
    }
    if (up && (lastFinance == 0 || millis() - lastFinance >= financeIntervalMs)) {
      lastFinance = millis();
      uint8_t successes = doFinance();
      if (successes == 3) { financeIntervalMs = FINANCE_REFRESH_MS; financeFailureStreak = 0; financeRetryAfterMs = 0; }
      else {
        uint8_t shift = min(financeFailureStreak, (uint8_t)2);
        uint32_t backoff = min(FINANCE_REFRESH_MS << shift, FINANCE_MAX_BACKOFF_MS);
        financeFailureStreak++;
        financeIntervalMs = max(backoff, financeRetryAfterMs);
        financeRetryAfterMs = 0;
      }
    }
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
lv_obj_t *financeView, *financeStrip, *financeStatusLabel, *financeAgeLabel[3], *financeValue[3], *financeDelta[3], *financePlot[3], *financeLine[3], *financeHeader[3], *financeCard[3];
lv_point_precise_t financeLinePoints[3][24] = {};
static lv_obj_t *menuWifiIcon;
lv_obj_t *calendarView;
int calendarOffset = 0;
lv_obj_t *calendarTitle;
lv_obj_t *calendarGrid;
lv_point_t calTouchStart;
static uint32_t seenScan = 0;

void post(int cmd) { xQueueSend(cmdQ, &cmd, 0); }

static void ui_update(lv_timer_t *) {
  struct tm ti;
  if (getLocalTime(&ti, 0)) {
    char b[24];
    strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M %p", &ti);
    lv_label_set_text(lblClock, b);
    static const char *days[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};
    static const char *months[] = {"Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez"};
    snprintf(b, sizeof b, "%s, %02d %s", days[ti.tm_wday], ti.tm_mday, months[ti.tm_mon]);
    lv_label_set_text(lblDate, b);
  } else {
    lv_label_set_text(lblClock, "--:--");
    lv_label_set_text(lblDate, "Aguardando hora...");
  }

  xSemaphoreTake(mtx, portMAX_DELAY);
  String status = sh.status;
  String list = sh.ssidList;
  bool up = sh.wifiUp;
  uint32_t scanVersion = sh.scanVersion;
  xSemaphoreGive(mtx);

  lv_label_set_text(lblStatus, status.c_str());
  lv_label_set_text(setStatusLbl, status.c_str());
  String wifiInfo = up ? "Conectado: " + WiFi.SSID() + "  |  IP " + WiFi.localIP().toString()
                       : "Desconectado  |  aguardando reconexão";
  lv_label_set_text(wifiInfoLbl, wifiInfo.c_str());
  lv_obj_set_style_text_color(lblWifiDot, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
  lv_obj_set_style_text_color(menuWifiIcon, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
  updateFinanceScreen();
  updateWeatherScreen();
  if (scanVersion != seenScan) {
    seenScan = scanVersion;
    lv_dropdown_set_options(ddSsid, list.isEmpty() ? "(none)" : list.c_str());
  }
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
  xSemaphoreGive(mtx);
  String page = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Weather | Ajustes</title>
<style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(145deg,#091629,#1b3b62);color:#edf4ff;font:16px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;display:grid;place-items:center;padding:18px}.card{width:min(100%,520px);padding:24px;border:1px solid #ffffff20;border-radius:20px;background:#ffffff0d;box-shadow:0 20px 60px #0005}h1{font-size:24px;margin:0 0 4px}.muted{color:#b4c7df}.status{margin:18px 0;padding:14px;border-radius:14px;background:#ffffff10;color:#d9e8fa;line-height:1.7}.label{display:block;margin:20px 0 8px;color:#d3e1f1}input{width:100%;padding:14px;border:1px solid #ffffff30;border-radius:12px;background:#0c1728;color:#fff;font-size:16px}button{margin-top:14px;width:100%;padding:14px;border:0;border-radius:12px;background:#18a4ef;color:#071525;font-size:16px;font-weight:700}small{display:block;margin-top:16px;line-height:1.5;color:#adbed2}.row{display:flex;gap:12px;align-items:center}.row input{width:auto}</style>
<main class="card"><h1>Clima e Finance</h1><div class="muted">Ajustes do relógio e da previsão</div><div class="status">%NOTICE%<br>Wi-Fi: %STATUS%<br>Endereço: http://%IP%</div><form method="post" action="/save-city" accept-charset="UTF-8"><label class="label" for="city">Cidade (ex.: São Paulo, BR)</label><input id="city" name="city" value="%CITY%" required><label class="label row"><input type="checkbox" name="h24" value="1" %H24%> Relógio 24 horas</label><button type="submit">Salvar ajustes</button></form><small>As cotações financeiras são consultadas automaticamente pelas APIs.</small></main></html>)HTML";
  page.replace("%NOTICE%", htmlEscape(notice));
  page.replace("%STATUS%", htmlEscape(status));
  page.replace("%IP%", WiFi.localIP().toString());
  page.replace("%CITY%", htmlEscape(city));
  page.replace("%H24%", cfg.h24 ? "checked" : "");
  return page;
}

static void startConfigServer() {
  configServer.on("/", HTTP_GET, []() {
    configServer.send(200, "text/html; charset=utf-8", configPage());
  });
  configServer.on("/save-city", HTTP_POST, []() {
    String city = configServer.arg("city");
    city.trim();
    cfg.h24 = configServer.hasArg("h24");
    prefs.putBool("h24", cfg.h24);
    post(CMD_FINANCE);
    if (!city.isEmpty()) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;
      sh.status = "Buscando cidade...";
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
    }
    configServer.send(200, "text/html; charset=utf-8",
                      configPage(city.isEmpty() ? "Formato de hora salvo." : "Ajustes salvos; atualizando a previsão."));
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

void showView(lv_obj_t *view) {
  lv_obj_add_flag(mainMenu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(financeView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(calendarView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(weatherView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(view, LV_OBJ_FLAG_HIDDEN);
}

static void showMenuEvent(lv_event_t *) { showView(mainMenu); }
static void showWeatherEvent(lv_event_t *) { showView(weatherView); }
static void showSettingsEvent(lv_event_t *) { showView(settingsView); }

static void showFinanceEvent(lv_event_t *) { showView(financeView); }
static void showCalendarEvent(lv_event_t *) { showView(calendarView); drawCalendar(); }

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
  menuTile(grid, LV_SYMBOL_VOLUME_MAX, "Sound", 0x4764CF, NULL);
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
  settingsView = lv_obj_create(scr);
  buildConfigScreen(settingsView);
  lv_obj_add_flag(weatherView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsView, LV_OBJ_FLAG_HIDDEN);
  buildConfigKeyboard();

  lv_timer_create(ui_update, 500, NULL);
  ui_update(NULL);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("NextNeo Vista boot");

  mtx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(int));

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
