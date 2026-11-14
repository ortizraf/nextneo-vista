// Network task (core 0): runs the jobs posted by the active screen (and the few
// context-free commands of Settings / the web page). Every HTTP/TLS client lives on
// this task; it never touches LVGL objects. Also keeps Wi-Fi and the clock alive.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "app_screens.h"
#include "core/net_worker.h"
#include "core/screen_manager.h"
#include "modules/weather/weather.h"
#include "services/app_log.h"

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


// Geocode (Settings / web page). When the active screen shows the forecast
// (Weather, Clock), refresh it right away as that screen's job.
static bool runGeocode() {
  const bool ok = weatherGeocodeFetch();
  if (!ok) return false;
  const ScreenLifecycle *s = screenLifecycle(activeScreenId());
  for (uint8_t i = 0; i < s->jobCount; ++i) {
    if (s->jobs[i].cmd != CMD_WEATHER) continue;
    jobSetCurrent(jobForActiveScreen(CMD_WEATHER));
    jobReportOutcome(s->id, s->jobs[i].run());
    break;
  }
  return true;
}
static bool runScan() { doScan(); return true; }
static bool runConnect() { doConnect(); return true; }

// Commands not bound to a screen.
static const NetJobDef kGlobalJobs[] = {
  {CMD_SCAN, "scan", runScan, false},
  {CMD_CONNECT, "connect", runConnect, false},
  {CMD_GEOCODE, "geocode", runGeocode, false},
};

static const NetJobDef *findJob(const NetJob &job) {
  if (job.screen == SCREEN_NONE) {
    for (const NetJobDef &d : kGlobalJobs) if (d.cmd == job.cmd) return &d;
    return nullptr;
  }
  const ScreenLifecycle *s = screenLifecycle(job.screen);
  for (uint8_t i = 0; i < s->jobCount; ++i) if (s->jobs[i].cmd == job.cmd) return &s->jobs[i];
  return nullptr;
}

// Executes one job. Blocking HTTP is fine here: this is the dedicated network
// task, never the LVGL thread.
static void runJob(const NetJob &job) {
  jobSetCurrent(job);
  const uint32_t started = millis();
  const NetJobDef *def = findJob(job);
  const char *name = def ? def->name : "?";
  bool ok = false;
  bool ran = false;
  if (!def) {
    Log.printf("[net] comando %u sem tratador para a tela %s\n", job.cmd, screenLifecycle(job.screen)->name);
  } else if (job.screen == SCREEN_NONE || screenJobStillCurrent()) {
    ok = def->run();       // otherwise: stale request from a screen the user already left
    ran = true;
  }
  if (job.screen != SCREEN_NONE) {
    if (!ran) {
      LogV.printf("[net] %s descartado (tela inativa)\n", name);
    } else if (screenJobStillCurrent()) {
      if (def->updatesTimer) jobReportOutcome(job.screen, ok);
      (ok ? LogV : Log).printf("[net] %s %s em %lums (interno livre %u, maior bloco %u)\n", name,
                    ok ? "ok" : "falhou", (unsigned long)(millis() - started),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
      Log.printf("[net] %s concluído fora de contexto; resultado ignorado\n", name);
    }
  }
  jobClearCurrent();
  jobClearPending(job.cmd);
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

    // Free keep-alive TLS sessions of screens that are not active, so the active
    // one has internal heap for its handshakes (ScreenLifecycle::netRelease).
    const ScreenID active = activeScreenId();
    for (int i = 0; i < SCREEN_COUNT; ++i) {
      const ScreenLifecycle *s = screenLifecycle((ScreenID)i);
      if (s->id != active && s->netRelease) s->netRelease();
    }

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
        Log.println("Wi-Fi offline; tentando a rede salva");   // no SSID in the log (privacy)
        WiFi.disconnect(false, false);
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
    }
    xSemaphoreTake(mtx, portMAX_DELAY);
    sh.wifiUp = up;
    xSemaphoreGive(mtx);
  }
}

void netWorkerBegin() {
  xTaskCreatePinnedToCore(workerTask, "worker", 12288, NULL, 1, NULL, 0);
}
