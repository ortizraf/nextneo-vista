#pragma once
// App contract shared by every module: global state, screen IDs, the screen
// lifecycle / network job contract and the small UI kit. Screen-specific APIs live
// in each module's own header (modules/<screen>/<screen>.h).

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
#define DEVICE_HOSTNAME "nextneovista"   // http://nextneovista.local (mDNS) + DHCP name
#define SCR_W 480
#define SCR_H 320

// One asset on the Finance screen: Yahoo Finance symbol + optional caption.
// Examples: USDBRL=X (dólar), EURBRL=X, BTC-USD, AAPL, PETR4.SA, ^BVSP (Ibovespa).
static constexpr uint8_t FINANCE_MAX_ASSETS = 8;
static constexpr uint8_t FINANCE_HISTORY = 48;     // card sparkline: whole trading day downsampled
struct FinanceAsset {
  char symbol[16] = {};
  char label[32] = {};
};

// State written by the network task and read by the UI thread (always under mtx).
struct Shared {
  String status = "Not connected";
  String ssidList;
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
  bool isDay = true;                  // Open-Meteo is_day for "now"
  bool hourIsDay[8] = {true, true, true, true, true, true, true, true};
  int hourRain[8] = {0};
  int humidity = 0;
  float wind = 0;
  float feels = 0;                 // apparent temperature
  float uv = -1;                   // UV index (-1 = unknown)
  char dayName[7][6] = {{0}};
  float tmax[7] = {0}, tmin[7] = {0};
  int dayRain[7] = {0};               // daily precipitation_probability_max (%)
  int dcode[7] = {0};
  // ---- Finance (configurable list of Yahoo Finance symbols, see modules/finance)
  FinanceAsset financeAssets[FINANCE_MAX_ASSETS];
  uint8_t financeAssetCount = 0;
  uint32_t financeConfigVersion = 0;     // bumped when the asset list changes
  char financeCurrency[FINANCE_MAX_ASSETS][4] = {};
  float finance[FINANCE_MAX_ASSETS] = {};
  float previousFinance[FINANCE_MAX_ASSETS] = {}, financeChange[FINANCE_MAX_ASSETS] = {};
  bool haveChange[FINANCE_MAX_ASSETS] = {};
  float history[FINANCE_MAX_ASSETS][FINANCE_HISTORY] = {};
  uint8_t historyCount[FINANCE_MAX_ASSETS] = {};
  bool haveFinance[FINANCE_MAX_ASSETS] = {};
  bool financeRefreshing = false;
  uint8_t financeSuccessCount = 0;
  bool financeApiKnown[FINANCE_MAX_ASSETS] = {};
  bool financeApiOk[FINANCE_MAX_ASSETS] = {};
  uint32_t financeLastSuccessMs = 0;
  uint32_t financeVersion = 0;
  uint32_t financeQuoteEpoch[FINANCE_MAX_ASSETS] = {};
  uint32_t weatherVersion = 0;
  String pendingSsid, pendingPass, pendingCity;
};

struct Config {
  float lat = -23.5505, lon = -46.6333;
  int utcOffset = -10800;
  bool h24 = true;
  int bright = 100;
};

extern Shared sh;
extern Config cfg;
extern SemaphoreHandle_t mtx;
extern Preferences prefs;
extern QueueHandle_t cmdQ;

// ---------------------------------------------------------------- screens
// Every view of the app has an ID. Only the active screen may schedule API
// calls, parse responses or touch its LVGL widgets.
enum ScreenID : uint8_t {
  SCREEN_NONE = 0,
  SCREEN_MENU,
  SCREEN_WEATHER,
  SCREEN_FINANCE,
  SCREEN_CALENDARIO,
  SCREEN_FLIGHT_RADAR,
  SCREEN_SETTINGS,
  SCREEN_SERVERS,
  SCREEN_CLOCK,
  SCREEN_AIRPORT,
  SCREEN_PHOTOS,
  SCREEN_SKY,
  SCREEN_F1,
  SCREEN_ART,
  SCREEN_FOOTBALL,
  SCREEN_COUNT
};

// Per-screen refresh intervals while the user stays on the screen.
static constexpr uint32_t SCREEN_ENTER_DEBOUNCE_MS        = 300UL;
static constexpr uint32_t WEATHER_UPDATE_INTERVAL_MS      = 10UL * 60UL * 1000UL;
static constexpr uint32_t WEATHER_RETRY_INTERVAL_MS       = 60UL * 1000UL;
static constexpr uint32_t FINANCE_UPDATE_INTERVAL_MS      = 2UL * 60UL * 1000UL;
static constexpr uint32_t FINANCE_RETRY_INTERVAL_MS       = 60UL * 1000UL;
static constexpr uint32_t CALENDARIO_UPDATE_INTERVAL_MS   = 10UL * 60UL * 1000UL;
static constexpr uint32_t FLIGHT_RADAR_UPDATE_INTERVAL_MS = 12UL * 1000UL;
static constexpr uint32_t FLIGHT_RADAR_RETRY_INTERVAL_MS  = 12UL * 1000UL;
static constexpr uint32_t SERVERS_UPDATE_INTERVAL_MS      = 10UL * 1000UL;
static constexpr uint32_t SERVERS_RETRY_INTERVAL_MS       = 10UL * 1000UL;
// The screen timer restarts when a job ends, so the real cycle is job time + this.
static constexpr uint32_t AIRPORT_UPDATE_INTERVAL_MS      = 1000UL;    // ADSB.lol only, almost continuous: ~2 s request + 1 s = ~3 s
static constexpr uint32_t AIRPORT_RETRY_INTERVAL_MS       = 10000UL;
static constexpr uint32_t SKY_UPDATE_INTERVAL_MS          = 30UL * 60UL * 1000UL;   // skyFetch skips if < 6 h old
static constexpr uint32_t SKY_RETRY_INTERVAL_MS           = 3UL * 60UL * 1000UL;
static constexpr uint32_t F1_UPDATE_INTERVAL_MS           = 15UL * 60UL * 1000UL;  // f1Fetch skips fresh data
static constexpr uint32_t F1_RETRY_INTERVAL_MS            = 2UL * 60UL * 1000UL;
// Futebol: the job runs often and fetches only what is stale (live leagues ~40 s, others 10 min)
static constexpr uint32_t FOOTBALL_UPDATE_INTERVAL_MS     = 20UL * 1000UL;
static constexpr uint32_t FOOTBALL_RETRY_INTERVAL_MS      = 30UL * 1000UL;

// ---------------------------------------------------------------- network jobs
enum Cmd : uint8_t {
  CMD_SCAN, CMD_CONNECT, CMD_GEOCODE,          // context-free commands (Settings / web page)
  CMD_WEATHER, CMD_FINANCE, CMD_FINANCE_ONE,   // CMD_FINANCE_ONE: asset in financeSingleIndex
  CMD_FLIGHT_RADAR, CMD_FLIGHT_DETAILS,        // screen-bound API jobs
  CMD_SERVERS,                                 // Beszel status (Servers screen)
  CMD_AIRPORT,                                 // OSM map + ADSB.lol traffic (Airport screen)
  CMD_SKY,                                     // CelesTrak satellite elements (Sky screen)
  CMD_F1,                                      // Jolpica calendar/standings + circuit maps (F1 screen)
  CMD_FOOTBALL,                                // ESPN scores/table/crests (Futebol screen)
  CMD_FINANCE_DETAIL,                          // Yahoo chart of one asset/range (Finance detail, on demand)
  CMD_COUNT
};

// Item carried by cmdQ. Screen-bound jobs remember which screen (and which
// visit of it, via epoch) requested them; the network task drops them as
// soon as that context is no longer the active one.
struct NetJob {
  uint8_t cmd;
  ScreenID screen;
  uint32_t epoch;
};

// One network job a screen can run. `run` executes on the network task (blocking
// HTTP is fine there) and returns true on success.
struct NetJobDef {
  uint8_t cmd;
  const char *name;
  bool (*run)();
  bool updatesTimer;            // result restarts the screen's refresh timer (false: on-demand job)
};

// Lifecycle contract of a screen: each module defines one (core/screen_registry.h)
// and the screen manager drives it. UI callbacks run on the LVGL thread.
struct ScreenLifecycle {
  ScreenID id;
  const char *name;
  lv_obj_t **view;              // container shown/hidden by the manager
  void (*build)(lv_obj_t *screen);   // create the widgets once at boot
  void (*onEnter)();            // screen became visible
  void (*onExit)();             // screen is about to be hidden
  void (*render)();             // refresh widgets from cached data (active screen only)
  void (*requestUpdate)();      // periodic refresh; nullptr = post jobs[0] (if any)
  uint32_t updateIntervalMs;    // periodic refresh while the user stays (0 = none)
  uint32_t retryIntervalMs;     // faster retry after a failed request
  const NetJobDef *jobs;        // jobs this screen may post (jobs[0] = periodic update)
  uint8_t jobCount;
  void (*netRelease)();         // network task: free keep-alive sessions while not active
};

void post(int cmd);                    // context-free command
void postScreenJob(int cmd);           // API job bound to the active screen
ScreenID activeScreenId();
bool screenJobStillCurrent();          // network task: is the running job still in context?
void switchScreen(ScreenID id);
const ScreenLifecycle *screenLifecycle(ScreenID id);   // never null (unknown id -> SCREEN_NONE)
lv_obj_t *screenView(ScreenID id);     // container of a screen (tests, diagnostics)

// ---------------------------------------------------------------- app
void appSetup();                       // whole app boot (setup() calls it; tests too)
void appLoop();
bool localNow(struct tm *t);           // local time; false until NTP synced
void setStatus(const String &s);       // sh.status under mtx
void applyTime();                      // (re)configure NTP with cfg.utcOffset

// ---------------------------------------------------------------- UI kit (core/ui_kit.cpp)
lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);     // the app's one back button (goes to the menu)
const char *uiSafeText(const char *in, char *out, size_t n);   // replaces glyphs our fonts lack
void setLabelIfChanged(lv_obj_t *label, const char *text);
void renderWifiIndicator(lv_obj_t *icon, bool up);
