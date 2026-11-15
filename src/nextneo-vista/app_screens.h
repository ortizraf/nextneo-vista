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
#include "core/text_utils.h"
#include "core/i18n.h"

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
#define DEVICE_HOSTNAME "nextneovista"   // http://nextneovista.local (mDNS) + DHCP name
#define SCR_W 480
#define SCR_H 320

// App-wide state (Wi-Fi, status line, network scan) written by the network task and read
// by the UI thread, always under mtx. Screen data lives in its module (wx in weather.h, fin in finance.h).
struct Shared {
  String status = "Not connected";
  String ssidList;
  uint32_t scanVersion = 0;
  bool wifiUp = false;
  String pendingSsid, pendingPass, pendingCity;   // Settings / web page -> network task
};

struct Config {
  float lat = -23.5505, lon = -46.6333;
  int utcOffset = -10800;
  bool h24 = true;
  int bright = 100;
};

extern Shared sh;
extern Config cfg;
extern SemaphoreHandle_t mtx;          // guards sh only; each module has its own lock (wxMtx, finMtx...)
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
  SCREEN_GARDEN,
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
// Context-free commands of the network task (Settings / web page), not bound to a screen.
enum GlobalCmd : uint8_t { CMD_SCAN, CMD_CONNECT, CMD_GEOCODE, CMD_GLOBAL_COUNT };

// Screen jobs have no global ID: a job is "index i in the active screen's jobs[]".
// jobs[JOB_UPDATE] is always the periodic refresh; screens with more jobs name the
// others with a local enum (e.g. FIN_JOB_DETAIL in finance_screen.cpp).
static constexpr uint8_t JOB_UPDATE = 0;
static constexpr uint8_t MAX_SCREEN_JOBS = 4;

// Item carried by cmdQ. job = GlobalCmd when screen is SCREEN_NONE, otherwise the index
// in that screen's jobs[]. Screen jobs remember the visit (epoch) that requested them;
// the network task drops them as soon as that context is no longer the active one.
struct NetJob {
  uint8_t job;
  ScreenID screen;
  uint32_t epoch;
};

// One network job. `run` executes on the network task (blocking HTTP is fine there)
// and returns true on success.
struct NetJobDef {
  const char *name;
  bool (*run)();
  bool updatesTimer;            // result restarts the screen's refresh timer (false: on-demand job)
  bool followsLocation = false; // re-run when the location (city) changes
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
  const NetJobDef *jobs;        // jobs this screen may post (jobs[JOB_UPDATE] = periodic update)
  uint8_t jobCount;
  void (*netRelease)();         // network task: free keep-alive sessions while not active
};

void post(GlobalCmd cmd);              // context-free command (Settings / web page)
void postScreenJob(uint8_t job = JOB_UPDATE);   // job of the active screen (index in its jobs[])
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
void setLabelIfChanged(lv_obj_t *label, const char *text);
void renderWifiIndicator(lv_obj_t *icon, bool up);
