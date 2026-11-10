#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
#define SCR_W 480
#define SCR_H 320

// One asset on the Finance screen: Yahoo Finance symbol + optional caption.
// Examples: USDBRL=X (dólar), EURBRL=X, BTC-USD, AAPL, PETR4.SA, ^BVSP (Ibovespa).
static constexpr uint8_t FINANCE_MAX_ASSETS = 8;
struct FinanceAsset {
  char symbol[16] = {};
  char label[32] = {};
};

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
  int hourRain[8] = {0};
  int humidity = 0;
  float wind = 0;
  float feels = 0;                 // apparent temperature
  float uv = -1;                   // UV index (-1 = unknown)
  char dayName[7][6] = {{0}};
  float tmax[7] = {0}, tmin[7] = {0};
  int dayRain[7] = {0};               // daily precipitation_probability_max (%)
  int dcode[7] = {0};
  // ---- Finance (configurable list of Yahoo Finance symbols, see finance_assets.h)
  FinanceAsset financeAssets[FINANCE_MAX_ASSETS];
  uint8_t financeAssetCount = 0;
  uint32_t financeConfigVersion = 0;     // bumped when the asset list changes
  char financeCurrency[FINANCE_MAX_ASSETS][4] = {};
  float finance[FINANCE_MAX_ASSETS] = {};
  float previousFinance[FINANCE_MAX_ASSETS] = {}, financeChange[FINANCE_MAX_ASSETS] = {};
  bool haveChange[FINANCE_MAX_ASSETS] = {};
  float history[FINANCE_MAX_ASSETS][24] = {};
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

// ---------------------------------------------------------------- screen context
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
  SCREEN_COUNT
};

// Lifecycle contract of a screen. All callbacks run on the LVGL (UI) thread.
struct ScreenLifecycle {
  ScreenID id;
  const char *name;
  lv_obj_t **view;              // container shown/hidden by the manager
  void (*onEnter)();            // screen became visible
  void (*onExit)();             // screen is about to be hidden
  void (*requestUpdate)();      // enqueue this screen's API job (never blocks)
  void (*render)();             // refresh widgets from cached data (active screen only)
  uint32_t updateIntervalMs;    // periodic refresh while the user stays (0 = none)
  uint32_t retryIntervalMs;     // faster retry after a failed request
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
// ~1.5-3 s job (keep-alive) + 2.5 s = about one update every 5 s.
static constexpr uint32_t AIRPORT_UPDATE_INTERVAL_MS      = 7000UL;    // ~3 s of requests + 7 s = ~10 s, no cache
static constexpr uint32_t AIRPORT_RETRY_INTERVAL_MS       = 10000UL;

enum Cmd : uint8_t {
  CMD_SCAN, CMD_CONNECT, CMD_GEOCODE,          // context-free commands (Settings / web page)
  CMD_WEATHER, CMD_FINANCE, CMD_FINANCE_ONE,   // CMD_FINANCE_ONE: asset in financeSingleIndex
  CMD_FLIGHT_RADAR, CMD_FLIGHT_DETAILS,        // screen-bound API jobs
  CMD_SERVERS,                                 // Beszel status (Servers screen)
  CMD_AIRPORT,                                 // OSM map + ADSB.lol traffic (Airport screen)
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

void post(int cmd);                    // context-free command
void postScreenJob(int cmd);           // API job bound to the active screen
ScreenID activeScreenId();
bool screenJobStillCurrent();          // network task: is the running job still in context?
void switchScreen(ScreenID id);
void setLabelIfChanged(lv_obj_t *label, const char *text);

extern lv_obj_t *mainMenu, *weatherView, *settingsView, *weatherTabs, *financeView, *calendarView, *flightRadarView;
extern lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
extern lv_obj_t *lblRange, *lblHumidity, *lblWind, *weatherArt, *homeTab;
extern lv_obj_t *fcDay[7], *fcCond[7], *fcTemp[7];
extern lv_obj_t *hourTime[8], *hourCond[8], *hourTemp[8], *hourRain[8];
extern lv_obj_t *hourArt[8], *dayArt[7];
extern lv_obj_t *ddSsid, *taPass, *taCity, *kb, *setStatusLbl, *wifiInfoLbl, *swH24;
extern lv_obj_t *financeStrip, *financeStatusLabel, *financeAgeLabel[FINANCE_MAX_ASSETS], *financeValue[FINANCE_MAX_ASSETS];
extern lv_obj_t *financeDelta[FINANCE_MAX_ASSETS], *financePlot[FINANCE_MAX_ASSETS], *financeLine[FINANCE_MAX_ASSETS];
extern lv_obj_t *financeHeader[FINANCE_MAX_ASSETS], *financeCard[FINANCE_MAX_ASSETS];
extern lv_point_precise_t financeLinePoints[FINANCE_MAX_ASSETS][24];
extern volatile int8_t financeSingleIndex;   // asset refreshed by CMD_FINANCE_ONE
extern lv_obj_t *calendarTitle, *calendarGrid;
extern int calendarOffset;
extern lv_point_t calTouchStart;

lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);
void showView(lv_obj_t *view);
const char *weatherText(int code);
bool isRainCode(int code);

void buildFinanceScreen(lv_obj_t *screen);
void buildCalendarScreen(lv_obj_t *screen);
void buildWeatherScreen(lv_obj_t *screen);
void buildConfigScreen(lv_obj_t *screen);
void buildConfigKeyboard();
void drawCalendar();
void calendarOnEnter();
void calendarRequestUpdate();
void financeOnEnter();
void updateFinanceScreen();
// Finance asset list (finance_assets.cpp). UI thread / web handler.
void loadFinanceAssets();
bool setFinanceAssets(const FinanceAsset *assets, uint8_t count);
bool normalizeFinanceSymbol(const char *in, char *out, size_t outSize);
void financeDisplayName(const char *symbol, char *out, size_t outSize);
const char *currencySymbol(const char *currency);
void configSyncRadarUi();
void weatherOnEnter();
void updateWeatherScreen();

// ---- Servers screen (servers.cpp): Beszel hub status for the homelab + proxy
static constexpr uint8_t SERVERS_MAX = 2;
extern lv_obj_t *serversView;
void buildServersScreen(lv_obj_t *screen);
void loadServersConfig();
void serversOnEnter();
void serversRender();
bool serversFetch();                                 // network task only
bool serversAlert();                                 // any server offline on last check
bool serversParseLink(const char *link, char *url, size_t urlSize, char *id, size_t idSize);
void serversSetServer(uint8_t i, const char *label, const char *url, const char *id);
void serversSetCredentials(const char *user, const char *pass);   // empty pass = keep
void serversGetConfig(uint8_t i, char *label, char *url, char *id, size_t n);
String serversUser();
bool serversHasPassword();
String serversStatusText(uint8_t i);

// ---- Clock screen (clock.cpp): animated sky + big local time
extern lv_obj_t *clockView;
void buildClockScreen(lv_obj_t *screen);
void clockOnEnter();
void clockOnExit();
void clockRender();

// ---- Airport screen (modules/flight_airport): OSM vector map + aircraft on the field
extern lv_obj_t *airportView;
void buildAirportScreen(lv_obj_t *screen);
void airportOnEnter();
void airportOnExit();
void airportRender();
bool airportFetch();                                 // network task only
void airportReleaseConnection();
extern volatile uint8_t gTouchCount;                 // fingers on the panel (touch driver)
extern volatile int16_t gTouch2X, gTouch2Y;          // 2nd finger, landscape coordinates                     // network task: drop the keep-alive TLS session
