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

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

// Guition JC3248W535C: AXS15231B 320x480 QSPI LCD, shown landscape (480x320).
#define GFX_BL 1
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define PANEL_W 320
#define PANEL_H 480
#define SCR_W 480
#define SCR_H 320

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, PANEL_W, PANEL_H);
// Keep the driver's native portrait geometry. Rotate the canvas instead of
// rotating the panel: AXS15231B full-frame QSPI writes can go blank when the
// panel is rotated independently from a landscape canvas.
Arduino_Canvas *gfx = new Arduino_Canvas(PANEL_W, PANEL_H, panel, 0, 0, 0);

// ---------------------------------------------------------------- shared state
// The worker task (WiFi/HTTP) writes; the LVGL loop reads. Guarded by `mtx`.
struct Shared {
  String status = "Not connected";
  String ssidList;          // '\n'-separated scan result
  uint32_t scanVersion = 0;
  bool wifiUp = false;
  bool haveWeather = false;
  String cityName = "São Paulo";
  float temp = 0;
  int code = 0;
  int hourCount = 0;
  char hourLabel[8][6] = {{0}};
  float hourTemp[8] = {0};
  int hourCode[8] = {0};
  int hourRain[8] = {0};
  int humidity = 0;
  float wind = 0;
  char dayName[7][6] = {{0}};
  float tmax[7] = {0}, tmin[7] = {0};
  int dcode[7] = {0};
  float finance[3] = {0, 0, 0};
  float previousFinance[3] = {0,0,0}, financeChange[3] = {0,0,0};
  bool haveChange[3] = {false,false,false};
  float history[3][24] = {};
  uint8_t historyCount[3] = {0,0,0};
  bool haveFinance[3] = {false, false, false};
  String pendingSsid, pendingPass, pendingCity;
} sh;
SemaphoreHandle_t mtx;

enum Cmd { CMD_SCAN, CMD_CONNECT, CMD_GEOCODE, CMD_WEATHER, CMD_FINANCE };
QueueHandle_t cmdQ;

struct Config {
  float lat = -23.5505, lon = -46.6333;
  int utcOffset = -10800;
  bool h24 = true;
  int bright = 100;
} cfg;
Preferences prefs;
WebServer configServer(80);
bool configServerStarted = false;

// ---------------------------------------------------------------- helpers
static const char *weatherText(int c) {
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

static bool isRainCode(int c) {
  return (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
}

static bool fetchYahoo(const char *symbol, float &price, float &changePct, float &previousClose, float *series, uint8_t &count) {
  String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + symbol + "?range=1d&interval=5m&nocache=" + String(millis());
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(12000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Cache-Control", "no-cache, no-store, max-age=0"); http.addHeader("Pragma", "no-cache"); http.addHeader("User-Agent", "Mozilla/5.0");
  int status=http.GET(); String body=status==200?http.getString():""; http.end(); if(body.isEmpty()) return false;
  JsonDocument doc; if(deserializeJson(doc,body)) return false;
  JsonObject meta=doc["chart"]["result"][0]["meta"].as<JsonObject>();
  float p=meta["regularMarketPrice"]|0.0f, pct=meta["regularMarketChangePercent"]|0.0f, prev=meta["previousClose"]|0.0f;
  if(p<=0) return false;
  JsonArray close=doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
  uint8_t n=0; for(JsonVariant v:close){float x=v|0.0f;if(x>0){if(n<24)series[n++]=x;else {for(int k=1;k<24;k++)series[k-1]=series[k];series[23]=x;}}}
  price=p;changePct=pct;previousClose=prev;count=n;return true;
}

static void doFinance() {
  if (WiFi.status() != WL_CONNECTED) return;
  const char *symbols[] = {"BRL=X", "EURBRL=X", "BTC-USD"};
  for (int i=0; i<3; ++i) {
    float v=0,pct=0,prev=0,points[24]={0};uint8_t n=0;bool ok=fetchYahoo(symbols[i],v,pct,prev,points,n);
    xSemaphoreTake(mtx, portMAX_DELAY);
    if(ok){ sh.previousFinance[i]=prev;sh.financeChange[i]=pct;sh.haveChange[i]=fabsf(pct)>0.0001f;sh.finance[i]=v;sh.haveFinance[i]=true;sh.historyCount[i]=n;memcpy(sh.history[i],points,sizeof points); }
    else {sh.finance[i]=0;sh.haveFinance[i]=false;sh.haveChange[i]=false;}
    xSemaphoreGive(mtx);
  }
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
        case CMD_FINANCE: doFinance(); lastFinance = millis(); break;
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
    if (up && (lastFinance == 0 || millis() - lastFinance >= 120000UL)) { doFinance(); lastFinance = millis(); }
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
static lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
static lv_obj_t *lblRange, *lblHumidity, *lblWind, *weatherArt, *homeTab;
static lv_obj_t *fcDay[7], *fcCond[7], *fcTemp[7];
static lv_obj_t *hourTime[8], *hourCond[8], *hourTemp[8], *hourRain[8];
static lv_obj_t *hourArt[8], *dayArt[7];
static lv_obj_t *ddSsid, *taPass, *taCity, *kb, *setStatusLbl, *wifiInfoLbl, *swH24;
static lv_obj_t *mainMenu, *weatherView, *settingsView, *weatherTabs;
static lv_obj_t *financeView, *financeStrip, *financeValue[3], *financeDelta[3], *financePlot[3], *financeHeader[3], *financeCard[3];
static lv_obj_t *menuWifiIcon;
static lv_obj_t *calendarView;
static int calendarOffset = 0;
static lv_obj_t *calendarTitle;
static lv_obj_t *calendarGrid;
static lv_point_t calTouchStart;
static uint32_t seenScan = 0;
static int renderedCode = -999;
static int renderedHourCodes[8] = {-999,-999,-999,-999,-999,-999,-999,-999};
static int renderedDayCodes[7] = {-999,-999,-999,-999,-999,-999,-999};

static lv_obj_t *makeWeatherIcon(lv_obj_t *parent, int x, int y, int size, int code) {
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_set_size(icon, size, size);
  lv_obj_set_pos(icon, x, y);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  // Scale the exact shapes and relative positions used by the large Agora
  // illustration (designed on a 150 px canvas) so all forecast views match it.
  const float s = size / 150.0f;
  auto scaled = [s](int value) { return (int)(value * s + 0.5f); };
  if (code <= 2) {
    lv_obj_t *sun = lv_obj_create(icon);
    lv_obj_set_size(sun, scaled(62), scaled(62));
    lv_obj_set_pos(sun, scaled(68), scaled(5));
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFC96B), 0);
    lv_obj_set_style_border_width(sun, 0, 0);
    lv_obj_move_to_index(sun, 0);
  }
  lv_obj_t *base = lv_obj_create(icon);
  lv_obj_set_size(base, scaled(106), scaled(42));
  lv_obj_set_pos(base, scaled(22), scaled(61));
  lv_obj_set_style_radius(base, scaled(22), 0);
  lv_obj_set_style_bg_color(base, lv_color_hex(0xBBD2EA), 0);
  lv_obj_set_style_border_width(base, 0, 0);
  const int puffX[] = {34, 68};
  const int puffY[] = {38, 49};
  const int puffSize[] = {54, 44};
  for (int j = 0; j < 2; j++) {
    lv_obj_t *puff = lv_obj_create(icon);
    lv_obj_set_size(puff, scaled(puffSize[j]), scaled(puffSize[j]));
    lv_obj_set_pos(puff, scaled(puffX[j]), scaled(puffY[j]));
    lv_obj_set_style_radius(puff, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puff, lv_color_hex(0xD8E7F5), 0);
    lv_obj_set_style_border_width(puff, 0, 0);
  }
  if (isRainCode(code)) {
    for (int r = 0; r < 3; r++) {
      lv_obj_t *drop = lv_obj_create(icon);
      lv_obj_set_pos(drop, scaled(42 + r * 35), scaled(110));
      lv_obj_set_size(drop, scaled(6), scaled(22));
      lv_obj_set_style_radius(drop, scaled(4), 0);
      lv_obj_set_style_bg_color(drop, lv_color_hex(0x6FC8FF), 0);
      lv_obj_set_style_border_width(drop, 0, 0);
      lv_obj_set_style_transform_rotation(drop, 120, 0);
    }
  }
  return icon;
}

static void post(int cmd) { xQueueSend(cmdQ, &cmd, 0); }

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
  String status = sh.status, list = sh.ssidList, city = sh.cityName;
  bool up = sh.wifiUp, have = sh.haveWeather;
  float fx[3]; bool fxok[3],deltaOk[3];float delta[3],hist[3][24];uint8_t histN[3];
  memcpy(fx,sh.finance,sizeof fx);memcpy(fxok,sh.haveFinance,sizeof fxok);memcpy(delta,sh.financeChange,sizeof delta);memcpy(deltaOk,sh.haveChange,sizeof deltaOk);memcpy(hist,sh.history,sizeof hist);memcpy(histN,sh.historyCount,sizeof histN);
  float temp = sh.temp; int code = sh.code;
  int humidity = sh.humidity; float wind = sh.wind;
  int hourCount = sh.hourCount;
  char hours[8][6]; float htemp[8]; int hcode[8], hrain[8];
  memcpy(hours, sh.hourLabel, sizeof hours);
  memcpy(htemp, sh.hourTemp, sizeof htemp);
  memcpy(hcode, sh.hourCode, sizeof hcode);
  memcpy(hrain, sh.hourRain, sizeof hrain);
  uint32_t sv = sh.scanVersion;
  char dn[7][6]; float mx[7], mn[7]; int dc[7];
  memcpy(dn, sh.dayName, sizeof dn);
  memcpy(mx, sh.tmax, sizeof mx); memcpy(mn, sh.tmin, sizeof mn); memcpy(dc, sh.dcode, sizeof dc);
  xSemaphoreGive(mtx);

  lv_label_set_text(lblStatus, status.c_str());
  lv_label_set_text(setStatusLbl, status.c_str());
  String wifiInfo = up ? "Conectado: " + WiFi.SSID() + "  |  IP " + WiFi.localIP().toString()
                       : "Desconectado  |  aguardando reconexão";
  lv_label_set_text(wifiInfoLbl, wifiInfo.c_str());
  lv_obj_set_style_text_color(lblWifiDot, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
  lv_obj_set_style_text_color(menuWifiIcon, up ? lv_color_hex(0x42D392) : lv_color_hex(0xF07878), 0);
  for(int i=0;i<3;i++){ if(fxok[i]) lv_label_set_text_fmt(financeValue[i], i==2?"%.0f":"%.2f", fx[i]); else lv_label_set_text(financeValue[i], "");
    lv_color_t trend=deltaOk[i]?lv_color_hex(delta[i]>=0?0x35D879:0xFF5555):lv_color_hex(0xAEB0B8);
    lv_obj_set_style_text_color(financeHeader[i],trend,0); lv_obj_set_style_border_color(financeCard[i],deltaOk[i]?trend:lv_color_hex(0x45484D),0);
    if(deltaOk[i]){lv_label_set_text_fmt(financeDelta[i], "%s %+.2f%%",delta[i]>=0?LV_SYMBOL_UP:LV_SYMBOL_DOWN,delta[i]);lv_obj_set_style_text_color(financeDelta[i],lv_color_hex(delta[i]>=0?0x35D879:0xFF5555),0);}else lv_label_set_text(financeDelta[i],"");
    lv_obj_clean(financePlot[i]); if(histN[i]>1){float lo=hist[i][0],hi=lo;for(int j=1;j<histN[i];j++){lo=min(lo,hist[i][j]);hi=max(hi,hist[i][j]);} if(hi-lo<0.0001f)hi=lo+0.0001f; for(int j=1;j<histN[i];j++){int x1=(j-1)*164/(histN[i]-1),x2=j*164/(histN[i]-1);int y1=33-(int)((hist[i][j-1]-lo)*27/(hi-lo)),y2=33-(int)((hist[i][j]-lo)*27/(hi-lo));lv_obj_t*seg=lv_obj_create(financePlot[i]);lv_obj_set_size(seg,max(2,x2-x1),2);lv_obj_set_pos(seg,x1,min(y1,y2));lv_obj_set_style_bg_color(seg,lv_color_hex(deltaOk[i]&&delta[i]<0?0xFF5555:0x35D879),0);lv_obj_set_style_bg_opa(seg,LV_OPA_COVER,0);lv_obj_set_style_border_width(seg,0,0);}}
  }
  lv_label_set_text(lblCity, city.c_str());
  if (have) {
    lv_label_set_text_fmt(lblTemp, "%.0f°C", temp);
    lv_label_set_text(lblCond, weatherText(code));
    lv_label_set_text_fmt(lblRange, "%.0f° / %.0f°", mx[0], mn[0]);
    lv_label_set_text_fmt(lblHumidity, "%d%% umid.", humidity);
    lv_label_set_text_fmt(lblWind, "%.0f km/h vento", wind);
    for (int i = 0; i < 7; i++) {
      lv_label_set_text(fcDay[i], i == 0 ? "Hoje" : dn[i]);
      lv_label_set_text(fcCond[i], weatherText(dc[i]));
      lv_label_set_text_fmt(fcTemp[i], "%.0f° / %.0f°", mx[i], mn[i]);
      if (renderedDayCodes[i] != dc[i]) {
        if (dayArt[i]) lv_obj_del(dayArt[i]);
        dayArt[i] = makeWeatherIcon(lv_obj_get_parent(fcDay[i]), 17, 35, 40, dc[i]);
        renderedDayCodes[i] = dc[i];
      }
      lv_obj_move_foreground(fcDay[i]);
      lv_obj_move_foreground(fcTemp[i]);
    }
    for (int i = 0; i < 8; i++) {
      if (i < hourCount) {
        lv_label_set_text_fmt(hourTime[i], "%sh", hours[i]);
        lv_label_set_text(hourCond[i], weatherText(hcode[i]));
        lv_label_set_text_fmt(hourTemp[i], "%.0f°", htemp[i]);
        lv_label_set_text_fmt(hourRain[i], "%d%%", hrain[i]);
        if (renderedHourCodes[i] != hcode[i]) {
          if (hourArt[i]) lv_obj_del(hourArt[i]);
          hourArt[i] = makeWeatherIcon(lv_obj_get_parent(hourTime[i]), 17, 35, 40, hcode[i]);
          renderedHourCodes[i] = hcode[i];
        }
        lv_obj_move_foreground(hourTime[i]);
        lv_obj_move_foreground(hourTemp[i]);
        lv_obj_move_foreground(hourRain[i]);
      } else {
        lv_label_set_text(hourTime[i], "--");
        lv_label_set_text(hourCond[i], "--");
        lv_label_set_text(hourTemp[i], "--");
        lv_label_set_text(hourRain[i], "");
      }
    }
    if (renderedCode != code) {
      renderedCode = code;
      // Change the scene colours to suit the current condition.
      uint32_t top = (code == 0) ? 0x17253D : (isRainCode(code) ? 0x101B30 : 0x1A263B);
      uint32_t bottom = (code == 0) ? 0x334C70 : (isRainCode(code) ? 0x203B5C : 0x344960);
      lv_obj_set_style_bg_color(homeTab, lv_color_hex(top), 0);
      lv_obj_set_style_bg_grad_color(homeTab, lv_color_hex(bottom), 0);
      lv_obj_clean(weatherArt);
      // Keep the original large weather artwork on Agora; card icons below
      // are a compact rendering of this same cloud/sun/drop visual language.
      lv_obj_t *cloudBase = lv_obj_create(weatherArt);
      lv_obj_set_pos(cloudBase, 22, 61); lv_obj_set_size(cloudBase, 106, 42);
      lv_obj_set_style_radius(cloudBase, 22, 0);
      lv_obj_set_style_bg_color(cloudBase, lv_color_hex(0xBBD2EA), 0);
      lv_obj_set_style_border_width(cloudBase, 0, 0);
      lv_obj_t *puff1 = lv_obj_create(weatherArt);
      lv_obj_set_pos(puff1, 34, 38); lv_obj_set_size(puff1, 54, 54);
      lv_obj_set_style_radius(puff1, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(puff1, lv_color_hex(0xD8E7F5), 0);
      lv_obj_set_style_border_width(puff1, 0, 0);
      lv_obj_t *puff2 = lv_obj_create(weatherArt);
      lv_obj_set_pos(puff2, 68, 49); lv_obj_set_size(puff2, 44, 44);
      lv_obj_set_style_radius(puff2, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(puff2, lv_color_hex(0xD8E7F5), 0);
      lv_obj_set_style_border_width(puff2, 0, 0);
      if (code <= 2) {
        lv_obj_t *sun = lv_obj_create(weatherArt);
        lv_obj_set_pos(sun, 68, 5); lv_obj_set_size(sun, 62, 62);
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sun, lv_color_hex(0xFFC96B), 0);
        lv_obj_set_style_border_width(sun, 0, 0);
        lv_obj_move_to_index(sun, 0);
      }
      if (isRainCode(code)) {
        for (int r = 0; r < 3; r++) {
          lv_obj_t *drop = lv_obj_create(weatherArt);
          lv_obj_set_pos(drop, 42 + r * 35, 110); lv_obj_set_size(drop, 6, 22);
          lv_obj_set_style_radius(drop, 4, 0);
          lv_obj_set_style_bg_color(drop, lv_color_hex(0x6FC8FF), 0);
          lv_obj_set_style_border_width(drop, 0, 0);
          lv_obj_set_style_transform_rotation(drop, 120, 0);
        }
      }
    }
  } else {
    lv_label_set_text(lblTemp, "--");
    lv_label_set_text(lblCond, up ? "Carregando previsão..." : "Configure o Wi-Fi em Ajustes");
  }
  if (sv != seenScan) {
    seenScan = sv;
    lv_dropdown_set_options(ddSsid, list.isEmpty() ? "(none)" : list.c_str());
  }
}

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
  configServer.on("/", HTTP_GET, []() { configServer.send(200, "text/html; charset=utf-8", configPage()); });
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
    configServer.send(200, "text/html; charset=utf-8", configPage(city.isEmpty() ? "Formato de hora salvo." : "Ajustes salvos; atualizando a previsão."));
  });
  configServer.onNotFound([]() { configServer.send(404, "text/plain; charset=utf-8", "Nao encontrado"); });
  configServer.begin();
  configServerStarted = true;
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static lv_obj_t *makeCard(lv_obj_t *parent, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x243247), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_70, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 16, 0);
  lv_obj_set_style_pad_all(c, 8, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void showView(lv_obj_t *view) {
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
static const char *calMonths[] = {"JAN.","FEV.","MAR.","ABR.","MAI.","JUN.","JUL.","AGO.","SET.","OUT.","NOV.","DEZ."};
static void drawCalendar() {
  lv_obj_clean(calendarGrid);
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today); struct tm base=today;
  base.tm_mday = 1; base.tm_mon += calendarOffset; mktime(&base);
  char title[20]; snprintf(title,sizeof(title),"%s %d",calMonths[base.tm_mon],base.tm_year+1900); lv_label_set_text(calendarTitle,title);
  const char *wd[] = {"D","S","T","Q","Q","S","S"};
  for(int i=0;i<7;i++){ auto l=makeLabel(calendarGrid,wd[i],&lv_font_weather_20,i==0?0xF04B54:0xFFFFFF); lv_obj_set_size(l,52,32); lv_obj_set_pos(l,i*60,0); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); }
  int first=base.tm_wday; struct tm next=base; next.tm_mon++; next.tm_mday=0; mktime(&next); int days=next.tm_mday;
  for(int d=1;d<=days;d++){int cell=first+d-1,row=cell/7,col=cell%7; bool selected=(d==today.tm_mday && calendarOffset==0); lv_obj_t *cellBox=lv_obj_create(calendarGrid); lv_obj_set_size(cellBox,44,36); lv_obj_set_pos(cellBox,col*60+4,row*35+34); lv_obj_set_style_bg_opa(cellBox,selected?LV_OPA_COVER:LV_OPA_TRANSP,0); lv_obj_set_style_bg_color(cellBox,lv_color_hex(0xF04B54),0); lv_obj_set_style_border_width(cellBox,0,0); lv_obj_set_style_radius(cellBox,7,0); lv_obj_set_style_pad_all(cellBox,0,0); lv_obj_remove_flag(cellBox,LV_OBJ_FLAG_SCROLLABLE); char n[4];snprintf(n,sizeof(n),"%d",d); lv_obj_t *num=lv_label_create(cellBox); lv_label_set_text(num,n); lv_obj_set_style_text_font(num,&lv_font_weather_20,0); lv_obj_set_style_text_color(num,lv_color_hex((col==0&&!selected)?0xF04B54:0xFFFFFF),0); lv_obj_center(num);}
}
static void calendarGesture(lv_event_t *e){lv_event_code_t c=lv_event_get_code(e);lv_indev_t *in=lv_indev_active();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);if(c==LV_EVENT_PRESSED){calTouchStart=p;return;}if(c==LV_EVENT_RELEASED){int dx=p.x-calTouchStart.x;if(abs(dx)>45){calendarOffset+=dx<0?1:-1;drawCalendar();}}}
static void showCalendarEvent(lv_event_t *) {showView(calendarView);drawCalendar();}

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

static lv_obj_t *addBackButton(lv_obj_t *parent) {
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

static void build_home(lv_obj_t *t) {
  homeTab = t;
  lv_obj_set_style_bg_color(t, lv_color_hex(0x17253D), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x334C70), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

  lblCity = makeLabel(t, "São Paulo", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_width(lblCity, 370);
  lv_label_set_long_mode(lblCity, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lblCity, 52, 13);
  lblDate = makeLabel(t, "", &lv_font_weather_14, 0xAFC0D6);
  lv_obj_set_pos(lblDate, 22, 42);
  lblClock = makeLabel(t, "--:--", &lv_font_montserrat_28, 0xEFF4FA);
  lv_obj_set_pos(lblClock, 20, 59);
  lblWifiDot = makeLabel(t, LV_SYMBOL_WIFI, &lv_font_weather_20, 0xD05050);
  lv_obj_align(lblWifiDot, LV_ALIGN_TOP_RIGHT, -18, 12);

  lblTemp = makeLabel(t, "--°", &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_set_pos(lblTemp, 20, 105);
  lblCond = makeLabel(t, "Aguardando clima", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(lblCond, 24, 158);
  lblRange = makeLabel(t, "--° / --°", &lv_font_weather_14, 0xC2D0E3);
  lv_obj_set_pos(lblRange, 24, 185);

  weatherArt = lv_obj_create(t);
  lv_obj_set_size(weatherArt, 130, 118);
  lv_obj_align(weatherArt, LV_ALIGN_TOP_RIGHT, -24, 66);
  lv_obj_set_style_bg_opa(weatherArt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weatherArt, 0, 0);
  lv_obj_set_style_pad_all(weatherArt, 0, 0);
  lv_obj_remove_flag(weatherArt, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *humidityCard = makeCard(t, 142, 38);
  lv_obj_align(humidityCard, LV_ALIGN_BOTTOM_LEFT, 18, -1);
  lblHumidity = makeLabel(humidityCard, "Umidade  --%", &lv_font_weather_14, 0xE1EAF6);
  lv_obj_center(lblHumidity);
  lv_obj_t *windCard = makeCard(t, 142, 38);
  lv_obj_align(windCard, LV_ALIGN_BOTTOM_LEFT, 168, -1);
  lblWind = makeLabel(windCard, "Vento  -- km/h", &lv_font_weather_14, 0xE1EAF6);
  lv_obj_center(lblWind);
  lblStatus = makeLabel(t, "", &lv_font_weather_14, 0x9FB0C7);
  lv_obj_align(lblStatus, LV_ALIGN_BOTTOM_RIGHT, 8, -10);
}

static void build_hours(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x111C2E), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x1B2D47), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = makeLabel(t, "Previsão por hora", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);

  lv_obj_t *strip = lv_obj_create(t);
  lv_obj_set_size(strip, 452, 232);
  lv_obj_set_pos(strip, 14, 29);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 4, 0);
  lv_obj_set_style_pad_column(strip, 8, 0);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < 8; i++) {
    lv_obj_t *card = makeCard(strip, 92, 158);
    lv_obj_set_style_translate_y(card, -16, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    hourTime[i] = makeLabel(card, "--", &lv_font_weather_14, 0xAFC0D6);
    lv_obj_align(hourTime[i], LV_ALIGN_TOP_MID, 0, 5);
    hourArt[i] = NULL;
    hourCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xDCE8F7);
    lv_obj_add_flag(hourCond[i], LV_OBJ_FLAG_HIDDEN);
    hourTemp[i] = makeLabel(card, "--", &lv_font_weather_20, 0xFFFFFF);
    lv_obj_align(hourTemp[i], LV_ALIGN_CENTER, 0, 27);
    hourRain[i] = makeLabel(card, "", &lv_font_weather_14, 0x74D2FF);
    lv_obj_set_width(hourRain[i], 80);
    lv_label_set_long_mode(hourRain[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(hourRain[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hourRain[i], LV_ALIGN_BOTTOM_MID, 0, -5);
  }
  lv_obj_t *hint = makeLabel(t, "A cada 3 h  ·  deslize para ver mais", &lv_font_weather_14, 0x9FB0C7);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void build_days(lv_obj_t *t) {
  lv_obj_set_style_bg_color(t, lv_color_hex(0x111C2E), 0);
  lv_obj_set_style_bg_grad_color(t, lv_color_hex(0x253750), 0);
  lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *title = makeLabel(t, "Próximos 7 dias", &lv_font_weather_20, 0xEAF2FF);
  lv_obj_set_pos(title, 82, 8);
  lv_obj_t *strip = lv_obj_create(t);
  lv_obj_set_size(strip, 452, 232);
  lv_obj_set_pos(strip, 14, 21);
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 4, 0);
  lv_obj_set_style_pad_column(strip, 8, 0);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < 7; i++) {
    lv_obj_t *card = makeCard(strip, 92, 158);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    fcDay[i] = makeLabel(card, "", &lv_font_weather_14, 0xAFC0D6);
    lv_obj_set_width(fcDay[i], 82);
    lv_obj_set_style_text_align(fcDay[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fcDay[i], LV_ALIGN_TOP_MID, 0, 5);
    dayArt[i] = NULL;
    fcCond[i] = makeLabel(card, "", &lv_font_weather_14, 0xEAF2FF);
    lv_obj_add_flag(fcCond[i], LV_OBJ_FLAG_HIDDEN);
    fcTemp[i] = makeLabel(card, "", &lv_font_weather_20, 0xFFD477);
    lv_obj_set_style_text_align(fcTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fcTemp[i], LV_ALIGN_CENTER, 0, 27);
  }
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

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

  mainMenu = lv_obj_create(scr);
  lv_obj_set_size(mainMenu, SCR_W, SCR_H);
  lv_obj_set_pos(mainMenu, 0, 0);
  lv_obj_set_style_pad_all(mainMenu, 0, 0);
  lv_obj_set_style_border_width(mainMenu, 0, 0);
  build_main_menu(mainMenu);

  financeView=lv_obj_create(scr); lv_obj_set_size(financeView,SCR_W,SCR_H); lv_obj_set_pos(financeView,0,0);
  lv_obj_add_flag(financeView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(financeView,lv_color_hex(0x231C2A),0); lv_obj_set_style_bg_grad_color(financeView,lv_color_hex(0x563A45),0); lv_obj_set_style_bg_grad_dir(financeView,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(financeView,LV_OPA_COVER,0); lv_obj_set_style_border_width(financeView,0,0); lv_obj_set_style_pad_all(financeView,0,0); lv_obj_remove_flag(financeView,LV_OBJ_FLAG_SCROLLABLE);
  financeStrip=lv_obj_create(financeView);lv_obj_set_size(financeStrip,SCR_W,266);lv_obj_set_pos(financeStrip,0,54);lv_obj_set_style_bg_opa(financeStrip,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financeStrip,0,0);lv_obj_set_style_pad_all(financeStrip,0,0);lv_obj_set_style_pad_column(financeStrip,16,0);lv_obj_set_scroll_dir(financeStrip,LV_DIR_HOR);lv_obj_set_flex_flow(financeStrip,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(financeStrip,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  // Keep the 240px cards inside the scroll viewport so the colored top border never gets clipped.
  lv_obj_set_style_pad_bottom(financeStrip,14,0);
  const char *ft[] = {"$  USD/BRL", "€  EUR/BRL", "$  BTC/USD"}; const char *fs[] = {"Dólar para real", "Euro para real", "Bitcoin em dólar"};
  for(int i=0;i<3;i++){ lv_obj_t *c=financeCard[i]=lv_obj_create(financeStrip); lv_obj_set_size(c,210,240); lv_obj_set_style_bg_color(c,lv_color_hex(0x151719),0); lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0); lv_obj_set_style_radius(c,22,0); lv_obj_set_style_border_width(c,2,0); lv_obj_set_style_border_color(c,lv_color_hex(0x45484D),0); lv_obj_set_style_shadow_width(c,12,0); lv_obj_set_style_shadow_opa(c,LV_OPA_40,0); lv_obj_remove_flag(c,LV_OBJ_FLAG_SCROLLABLE); financeHeader[i]=makeLabel(c,ft[i],&lv_font_weather_14,0xAEB0B8); lv_obj_set_pos(financeHeader[i],14,12); lv_obj_t *s=makeLabel(c,fs[i],&lv_font_weather_14,0xAEB0B8); lv_obj_set_pos(s,14,36); financePlot[i]=lv_obj_create(c);lv_obj_set_size(financePlot[i],164,56);lv_obj_set_pos(financePlot[i],14,62);lv_obj_set_style_bg_opa(financePlot[i],LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financePlot[i],0,0);lv_obj_set_style_pad_all(financePlot[i],0,0);lv_obj_remove_flag(financePlot[i],LV_OBJ_FLAG_SCROLLABLE); financeDelta[i]=makeLabel(c,"",&lv_font_weather_14,0x30C875);lv_obj_set_pos(financeDelta[i],14,128); financeValue[i]=makeLabel(c,"",&lv_font_montserrat_48,0xEEEEEE); lv_obj_align(financeValue[i],LV_ALIGN_BOTTOM_RIGHT,-10,-5); }
  lv_obj_t *financeTitle=makeLabel(financeView,"Finance",&lv_font_weather_20,0xFFFFFF); lv_obj_align(financeTitle,LV_ALIGN_TOP_LEFT,62,8); addBackButton(financeView); lv_obj_move_foreground(lv_obj_get_child(financeView,lv_obj_get_child_cnt(financeView)-1));

  calendarView = lv_obj_create(scr); lv_obj_set_size(calendarView,SCR_W,SCR_H); lv_obj_set_pos(calendarView,0,0); lv_obj_set_style_bg_color(calendarView,lv_color_hex(0x05091C),0); lv_obj_set_style_bg_opa(calendarView,LV_OPA_COVER,0); lv_obj_set_style_border_width(calendarView,0,0); lv_obj_set_style_pad_all(calendarView,0,0); lv_obj_remove_flag(calendarView,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(calendarView,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(calendarView,LV_OBJ_FLAG_GESTURE_BUBBLE);
  calendarTitle=makeLabel(calendarView,"",&lv_font_weather_20,0xFFFFFF); lv_obj_align(calendarTitle,LV_ALIGN_TOP_MID,0,30);
  calendarGrid=lv_obj_create(calendarView); lv_obj_set_size(calendarGrid,430,265); lv_obj_align(calendarGrid,LV_ALIGN_CENTER,0,40); lv_obj_set_style_bg_opa(calendarGrid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(calendarGrid,0,0); lv_obj_set_style_pad_all(calendarGrid,0,0); lv_obj_remove_flag(calendarGrid,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(calendarGrid,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_PRESSED,NULL); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_RELEASED,NULL); addBackButton(calendarView); drawCalendar();

  weatherView = lv_obj_create(scr);
  lv_obj_set_size(weatherView, SCR_W, SCR_H);
  lv_obj_set_pos(weatherView, 0, 0);
  lv_obj_set_style_pad_all(weatherView, 0, 0);
  lv_obj_set_style_border_width(weatherView, 0, 0);
  lv_obj_set_style_bg_opa(weatherView, LV_OPA_TRANSP, 0);
  weatherTabs = lv_tabview_create(weatherView);
  lv_obj_set_size(weatherTabs, SCR_W, SCR_H);
  lv_obj_set_pos(weatherTabs, 0, 0);
  lv_tabview_set_tab_bar_position(weatherTabs, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(weatherTabs, 42);
  lv_obj_set_style_bg_color(weatherTabs, lv_color_hex(0x101820), 0);
  lv_obj_t *tabBar = lv_tabview_get_tab_bar(weatherTabs);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x0B1424), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(tabBar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x203C5C), LV_PART_ITEMS);
  lv_obj_set_style_text_color(tabBar, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(tabBar, 2, LV_PART_ITEMS);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x8AB9DD), LV_PART_ITEMS);
  lv_obj_set_style_text_opa(tabBar, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x173D65), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(tabBar, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x20A7F2), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tabBar, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_remove_flag(lv_tabview_get_content(weatherTabs), LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_t *home = lv_tabview_add_tab(weatherTabs, "Agora");
  lv_obj_t *hours = lv_tabview_add_tab(weatherTabs, "Horas");
  lv_obj_t *days = lv_tabview_add_tab(weatherTabs, "Dias");
  // Tab buttons are created by add_tab(), so apply their visible idle style afterwards.
  lv_obj_t *tabs = lv_tabview_get_tab_bar(weatherTabs);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *tabButton = lv_obj_get_child(tabs, i);
    if (!tabButton) continue;
    lv_obj_set_style_bg_color(tabButton, lv_color_hex(0x17263B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabButton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(tabButton, lv_color_hex(0xD7E2EF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(tabButton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tabButton, lv_color_hex(0x334A64), LV_PART_MAIN);
    lv_obj_set_style_border_width(tabButton, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tabButton, lv_color_hex(0x087FC0), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabButton, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_CHECKED);
  }
  build_home(home);
  build_hours(hours);
  build_days(days);
  addBackButton(home);
  addBackButton(hours);
  addBackButton(days);

  settingsView = lv_obj_create(scr);
  lv_obj_set_size(settingsView, SCR_W, SCR_H);
  lv_obj_set_pos(settingsView, 0, 0);
  lv_obj_set_style_pad_all(settingsView, 0, 0);
  lv_obj_set_style_border_width(settingsView, 0, 0);
  lv_obj_set_style_bg_color(settingsView, lv_color_hex(0x101820), 0);
  build_settings(settingsView);
  lv_obj_add_flag(weatherView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsView, LV_OBJ_FLAG_HIDDEN);

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
