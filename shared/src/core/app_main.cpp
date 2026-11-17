// Boot e laço do app, COMUNS a todas as placas: configuração salva, Wi-Fi, mDNS, tarefa
// de rede e telas. O que é da placa vem pelo contrato core/board.h (boardDisplayBegin,
// boardWifiPrepare, kBoard) e pela tabela de telas (core/screen_lifecycle.h).
// O main.cpp de cada placa só chama appSetup()/appLoop().
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "core/app_core.h"
#include "core/board.h"
#include "core/net_worker.h"
#include "core/screen_manager.h"
#include "core/web_config.h"
#include "modules/finance/finance.h"
#include "modules/servers/servers.h"
#include "modules/weather/weather.h"
#include "services/app_log.h"

static void loadConfig() {
  prefs.begin("clock", false);
  cfg.lat = prefs.getFloat("lat", cfg.lat);
  cfg.lon = prefs.getFloat("lon", cfg.lon);
  cfg.utcOffset = prefs.getInt("tz", cfg.utcOffset);
  cfg.h24 = prefs.getBool("h24", true);
  cfg.bright = prefs.getInt("bl", 100);
  wx.cityName = prefs.getString("city", "São Paulo");
  if (wx.cityName == "Sao Paulo") {
    wx.cityName = "São Paulo";
    prefs.putString("city", wx.cityName);
  }
  gLogVerbose = prefs.getBool("logVerbose", false);   // off by default
  gLang = prefs.getUChar("lang", LANG_PT);             // before the screens are built (core/i18n.h)
  if (gLang >= LANG_COUNT) gLang = LANG_PT;
#ifdef NNV_FORCE_LANG   // test builds only (e.g. PLATFORMIO_BUILD_FLAGS=-DNNV_FORCE_LANG=1): nothing is saved
  gLang = NNV_FORCE_LANG;
#endif
  loadFinanceAssets();   // Finance symbols (editable on the web page)
  loadServersConfig();   // Beszel hubs + login (Settings / web page)
}

static void startWifi() {
  boardWifiPrepare();                           // ex.: P4 configura o coprocessador ESP32-C6
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
    setStatus(T("Conectando a ", "Connecting to ") + ssid + "...");
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
  Log.printf("NextNeo Vista boot (%s, %ux%u)\n", kBoard.name, kBoard.width, kBoard.height);

  mtx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(NetJob));
  loadConfig();

  boardDisplayBegin();          // tela, touch e LVGL desta placa (core/board.h)
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

