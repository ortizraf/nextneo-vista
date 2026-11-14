// NextNeo Vista: boot sequence and main loop. Everything else lives in core/
// (infrastructure), modules/ (one folder per screen) and services/ (HTTP, memory, log).
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "app_screens.h"
#include "core/display_touch.h"
#include "core/net_worker.h"
#include "core/screen_manager.h"
#include "core/web_config.h"
#include "modules/finance/finance.h"
#include "modules/servers/servers.h"
#include "services/app_log.h"

// LVGL renders on the Arduino loop task. Deep widget trees (Weather tabs over the
// sky photo) overflowed the default 8 KB stack ("Stack canary watchpoint (loopTask)").
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

static void loadConfig() {
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
  gLogVerbose = prefs.getBool("logVerbose", false);   // off by default
  loadFinanceAssets();   // Finance symbols (editable on the web page)
  loadServersConfig();   // Beszel hubs + login (Settings / web page)
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);            // name shown in the router's client list (DHCP)
  // Wi-Fi modem sleep off: with power save the radio only wakes on DTIM beacons, so mDNS
  // queries (nextneovista.local) were answered late or missed and the browser retried for
  // seconds. Also lowers web page / API latency. Cost: a few tens of mA (screen is on anyway).
  WiFi.setSleep(false);
  webConfigBeginMdns();                         // must run before connecting
  WiFi.setAutoReconnect(true);
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
    setStatus("Connecting to " + ssid + "...");
  }
}

// ---------------------------------------------------------------- setup / loop
// appSetup/appLoop hold the app; setup()/loop() only call them, so the on-device
// tests (test/, PIO_UNIT_TESTING) can boot the real app from their own setup().
void appSetup() {
  Serial.begin(115200);
  // USB serial must never block the app: with the cable plugged in but no monitor reading,
  // every log line waited for the TX timeout (seen as ~2 s gaps between log lines).
  Serial.setTxTimeoutMs(0);
  appLogBegin();   // keep the last minutes of log in PSRAM for http://IP/log
  delay(1000);
  Log.println("NextNeo Vista boot");

  mtx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(NetJob));
  loadConfig();

  displayBegin();
  lv_obj_t *root = lv_screen_active();
  lv_obj_set_style_bg_color(root, lv_color_hex(0x101820), 0);
  screenManagerBegin(root);     // builds every screen and shows the menu

  startWifi();
  netWorkerBegin();
}

void appLoop() {
  webConfigLoop();
  const uint32_t idle = lv_timer_handler();      // ms until the next LVGL timer
  delay(idle > 5 ? 5 : idle ? idle : 1);
}

#ifndef PIO_UNIT_TESTING
void setup() { appSetup(); }
void loop() { appLoop(); }
#endif
