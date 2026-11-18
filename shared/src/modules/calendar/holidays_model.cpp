#include "modules/calendar/holidays_model.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>

static constexpr uint32_t CLOCK_VALID_EPOCH = 1700000000UL;

bool parseHolidaysJson(const char *json, size_t len, int year, uint32_t now, HolidayList &out) {
  out = HolidayList{};
  out.fetchedAt = now;
  if (!json || !len) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json, len) || !doc.is<JsonArray>()) return false;
  for (JsonObject h : doc.as<JsonArray>()) {
    const char *d = h["date"] | "";
    if (strlen(d) < 10 || d[4] != '-' || d[7] != '-') continue;
    if (atoi(d) != year) continue;
    const int m = atoi(d + 5), day = atoi(d + 8);
    if (m < 1 || m > 12 || day < 1 || day > 31 || holidayListHas(out, m, day)) continue;
    if (out.n >= HOLIDAY_MAX_DAYS) break;
    out.md[out.n][0] = (uint8_t)m; out.md[out.n][1] = (uint8_t)day; out.n++;
  }
  return out.n > 0;
}

bool holidayListHas(const HolidayList &l, int month, int day) {
  for (int i = 0; i < l.n && i < HOLIDAY_MAX_DAYS; ++i)
    if (l.md[i][0] == month && l.md[i][1] == day) return true;
  return false;
}

bool holidayCacheStale(bool loaded, const HolidayList &l, uint32_t now) {
  if (!loaded || l.n == 0 || l.n > HOLIDAY_MAX_DAYS) return true;
  if (now < CLOCK_VALID_EPOCH) return false;          // relógio não sincronizado: usa o cache
  return now < l.fetchedAt || now - l.fetchedAt > HOLIDAY_CACHE_MAX_AGE_S;
}

// ---------------------------------------------------------------- calendar cat (pure)
#include "modules/calendar/calendar_cats.h"

const char *calendarCatName(uint8_t cat) {
  static const char *const kNames[CAL_CAT_COUNT] = {"Frajola", "Tricolor", "Laranja", "Cinza rajado", "Aleat\xC3\xB3rio"};
  return cat < CAL_CAT_COUNT ? kNames[cat] : kNames[CAL_CAT_FRAJOLA];
}

uint8_t calendarCatFromArg(const char *arg) {
  if (!arg || !*arg) return CAL_CAT_FRAJOLA;
  char *end = nullptr;
  const long v = strtol(arg, &end, 10);
  if (*end != '\0' || v < 0 || v >= CAL_CAT_COUNT) return CAL_CAT_FRAJOLA;
  return (uint8_t)v;
}

uint8_t calendarCatResolve(uint8_t setting, uint8_t previous, uint32_t rnd) {
  if (setting < CAL_CAT_ART_COUNT) return setting;
  if (previous >= CAL_CAT_ART_COUNT) return (uint8_t)(rnd % CAL_CAT_ART_COUNT);
  // pick among the other three, so two openings in a row never show the same cat
  return (uint8_t)((previous + 1 + rnd % (CAL_CAT_ART_COUNT - 1)) % CAL_CAT_ART_COUNT);
}
