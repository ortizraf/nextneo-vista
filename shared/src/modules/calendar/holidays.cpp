// Feriados nacionais (https://brasilapi.com.br/api/feriados/v1/<ano>): ~15 datas por ano,
// guardadas compactas em NVS ("hol<ano>"), renovadas a cada 30 dias.
// Regra: a API só é chamada enquanto a tela Calendário está ativa. O job confere isso
// antes de conectar e de novo depois do download (se o usuário saiu, descarta).
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "core/app_core.h"
#include "modules/calendar/holidays.h"
#include "modules/calendar/holidays_model.h"
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

namespace {
constexpr int SLOTS = 3;
struct Year { int16_t year = 0; bool loaded = false; HolidayList l{}; };
Year slots[SLOTS];
volatile int pendingYear = 0;
volatile uint32_t version = 1;
SemaphoreHandle_t holMtx = xSemaphoreCreateMutex();

Year *find(int y) { for (Year &s : slots) if (s.year == y) return &s; return nullptr; }
void key(char *k, int y) { snprintf(k, 8, "hol%04d", y); }
bool calendarActive() { return activeScreenId() == SCREEN_CALENDARIO && screenJobStillCurrent(); }
}

void holidaysWant(int year) {
  xSemaphoreTake(holMtx, portMAX_DELAY);
  Year *s = find(year);
  if (!s) {
    s = nullptr;
    for (Year &x : slots) if (x.year == 0) { s = &x; break; }
    if (!s) { memmove(&slots[0], &slots[1], sizeof(Year) * (SLOTS - 1)); s = &slots[SLOTS - 1]; }
    *s = Year(); s->year = (int16_t)year;
    char k[8]; key(k, year);
    if (prefs.getBytes(k, &s->l, sizeof s->l) == sizeof s->l && s->l.n <= HOLIDAY_MAX_DAYS) { s->loaded = true; version++; }
    else s->l = HolidayList{};
  }
  if (holidayCacheStale(s->loaded, s->l, (uint32_t)time(nullptr))) pendingYear = year;
  xSemaphoreGive(holMtx);
}

bool holidaysNeedFetch() {
  return pendingYear != 0 && activeScreenId() == SCREEN_CALENDARIO && WiFi.status() == WL_CONNECTED;
}

bool holidayIs(int year, int month, int day) {
  xSemaphoreTake(holMtx, portMAX_DELAY);
  const Year *s = find(year);
  const bool hit = s && holidayListHas(s->l, month, day);
  xSemaphoreGive(holMtx);
  return hit;
}

uint32_t holidaysVersion() { return version; }

bool holidaysFetch() {
  const int year = pendingYear;
  if (!year) return true;
  if (!calendarActive()) return false;               // never outside the Calendar screen
  if (WiFi.status() != WL_CONNECTED) return false;
  static PsramBuffer body(16 * 1024);
  WiFiClientSecure client;
  client.setInsecure();                              // public read-only data, nothing secret is sent
  client.setHandshakeTimeout(12);
  char url[64]; snprintf(url, sizeof url, "https://brasilapi.com.br/api/feriados/v1/%d", year);
  const int code = httpsFetch(client, url, body, 10000, false);
  client.stop();
  if (!calendarActive()) return false;               // left the screen meanwhile: drop it
  if (code != 200) { Log.printf("Feriados %d: HTTP %d\n", year, code); return false; }
  HolidayList l;
  if (!parseHolidaysJson(body.c_str(), body.length(), year, (uint32_t)time(nullptr), l)) {
    Log.printf("Feriados %d: resposta sem datas\n", year); return false;
  }
  char k[8]; key(k, year);
  prefs.putBytes(k, &l, sizeof l);
  xSemaphoreTake(holMtx, portMAX_DELAY);
  if (Year *s = find(year)) { s->l = l; s->loaded = true; }
  if (pendingYear == year) pendingYear = 0;
  version++;
  xSemaphoreGive(holMtx);
  Log.printf("Feriados %d: %u datas\n", year, l.n);
  return true;
}
