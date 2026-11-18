#pragma once
// Lógica pura dos feriados (sem Arduino): parse da BrasilAPI, busca de datas e
// validade do cache. Testada no computador (test/test_native_holidays).
#include <stddef.h>
#include <stdint.h>

static constexpr int HOLIDAY_MAX_DAYS = 32;
static constexpr uint32_t HOLIDAY_CACHE_MAX_AGE_S = 30UL * 86400UL;   // renova a cada 30 dias

struct HolidayList {                 // também é o layout gravado em NVS
  uint32_t fetchedAt;                // epoch da busca
  uint8_t n;
  uint8_t md[HOLIDAY_MAX_DAYS][2];   // [mês 1-12, dia 1-31]
};

// JSON da BrasilAPI ([{"date":"YYYY-MM-DD",...}]) -> datas do ano pedido.
// false se o JSON for inválido ou não tiver nenhuma data daquele ano.
bool parseHolidaysJson(const char *json, size_t len, int year, uint32_t now, HolidayList &out);
bool holidayListHas(const HolidayList &l, int month, int day);
// Precisa buscar? Sem cache, cache inválido ou mais velho que 30 dias (só com relógio válido).
bool holidayCacheStale(bool loaded, const HolidayList &l, uint32_t now);
