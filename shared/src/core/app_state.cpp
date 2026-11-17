// Shared application state (sh / cfg), its lock and the tiny helpers every module uses.
#include <Arduino.h>
#include <time.h>
#include "core/app_core.h"

Shared sh;
SemaphoreHandle_t mtx;
QueueHandle_t cmdQ;
Config cfg;
Preferences prefs;

// getLocalTime(&t, 0) fails at random: with a 0 ms timeout its loop is skipped whenever
// millis() ticks between two reads. Read the clock directly instead.
bool localNow(struct tm *t) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;                  // NTP not synced yet
  localtime_r(&now, t);
  return true;
}

void setStatus(const String &s) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  sh.status = s;
  xSemaphoreGive(mtx);
}

void applyTime() { configTime(cfg.utcOffset, 0, "pool.ntp.org", "time.google.com"); }

