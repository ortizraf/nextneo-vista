#pragma once
// Futebol module internals: ESPN data model shared by the network side
// (football_fetch.cpp) and the screen (football_screen.cpp).
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>

namespace football {

enum League : uint8_t { L_CL, L_LIB, L_CWC, L_WC, L_SUL, L_UEL, L_INT, L_COUNT };   // L_INT has no tab: shown under MUN
extern const char *const kLeagueShort[L_COUNT];   // until the logo arrives
constexpr int MAX_M = 24, CREST = 40, LEAGUE_LOGO = 30;
inline uint32_t leagueImgId(int L) { return 0x80000200u + (uint32_t)L; }   // new base: old ids meant other leagues   // crest cache/file key
constexpr uint32_t LIVE_TTL_MS = 40000, IDLE_TTL_MS = 10UL * 60 * 1000;
enum MState : uint8_t { ST_PRE = 0, ST_IN, ST_POST };

struct FTeam { uint32_t id; char abbr[6]; char name[24]; uint32_t color, alt; char logo[64]; int8_t score; };
struct FMatch { uint32_t id, start; uint8_t league, state; char clock[12]; char phase[48]; FTeam home, away; };
struct FData {
  uint32_t version, updatedMs, logoVersion;
  FMatch m[L_COUNT][MAX_M];
  uint8_t n[L_COUNT];
  uint32_t fetchedMs[L_COUNT];
  bool live[L_COUNT];
  char leagueLogo[L_COUNT][64];                  // path on a.espncdn.com (ESPN's competition logo)
  char msg[64];
};

extern SemaphoreHandle_t lock;
extern FData *shared;                      // last complete copy, under `lock`

inline void copyStr(const char *s, char *out, size_t n) { snprintf(out, n, "%s", s ? s : ""); }
void crestPath(uint32_t id, char *out, size_t n);   // crest / logo file on LittleFS

}  // namespace football
