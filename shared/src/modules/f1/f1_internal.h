#pragma once
// F1 module internals shared by the network side (f1_fetch.cpp) and the screen
// (f1_screen.cpp). Not for other modules: they only see the ScreenLifecycle.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <stdio.h>
#include "core/i18n.h"

namespace f1 {

struct CircuitInfo {
  const char *cid, *track, *gp, *country;   // Jolpica circuitId, bacinger id, GP name, country (pt)
  const char *gpEn, *countryEn;             // same in English
};
inline const char *gpName(const CircuitInfo *c) { return gLang == LANG_EN ? c->gpEn : c->gp; }
inline const char *countryName(const CircuitInfo *c) { return gLang == LANG_EN ? c->countryEn : c->country; }
const CircuitInfo *circuitInfo(const char *cid);

enum { S_FP1, S_FP2, S_FP3, S_SQ, S_SPRINT, S_QUALI, S_RACE, S_COUNT };
struct Race {
  uint8_t round; char cid[20], name[40], locality[24], country[24];
  float lat, lon;
  time_t t[S_COUNT];                                      // 0 = no such session
};
struct Standing { uint8_t pos; char name[28], team[24], teamId[16]; float pts; uint8_t wins; };
struct F1Data {
  uint32_t version;
  int season, nRaces;
  Race races[30];
  int resultRound; char resultRace[40]; char podium[3][28];
  int nDrivers, nTeams;
  Standing drivers[24], teams[12];
  uint32_t schedMs, standMs;                              // millis() of the last good fetch (0 = never)
  // weather forecast at the circuit for each session of the next weekend (Open-Meteo)
  char wxCid[20]; uint32_t wxMs;
  long wxOff;                                             // UTC offset of the circuit (s), from the forecast
  bool wxHave[S_COUNT], wxNight[S_COUNT]; int8_t wxCode[S_COUNT]; float wxTemp[S_COUNT], wxMin[S_COUNT], wxMax[S_COUNT];
};

// ---- shared between the network task and the UI
extern F1Data *shared;                    // last complete copy, under `lock`
extern SemaphoreHandle_t lock;
extern volatile uint32_t mapVersion;      // bumped when a circuit file is written
extern char mapWanted[20];                // circuit the UI shows (network task fetches its map)
extern char netMsg[72];                   // last network problem, shown while nothing is loaded
extern volatile uint8_t mapStyle;         // ST_STREET / ST_DRAWN

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
// Big work arrays live in PSRAM: internal RAM must stay free for the TLS handshakes
// (mbedTLS needs one ~40-60 KB contiguous block of internal RAM).
template <class T> inline T *psram(T *&p, size_t n) {
  if (!p) p = (T *)heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p;
}
inline void copyStr(const char *s, char *out, size_t n) { snprintf(out, n, "%s", s ? s : ""); }
time_t parseUtc(const char *date, const char *tm);    // "2026-03-08", "04:00:00Z"

// ---- circuit files (LittleFS):  /f1/t_<cid>.bin track,  /f1/m_<cid>.bin surroundings
// Coordinates are metres (int16) east/north of the track centre (lat0, lon0).
struct TrackHdr { uint32_t magic; float lat0, lon0; uint16_t lengthM, n; };
static constexpr uint32_t T_MAGIC = 0x31543146, M_MAGIC = 0x314D3146;   // "F1T1", "F1M1"
enum { K_GREEN, K_SAND, K_WATER, K_BUILD, K_STAND, K_WLINE, K_PIT, K_CORNER, K_COAST, K_ROAD, K_STREET, K_ALT, K_PATH, K_PARKING, K_COUNT };
// K_ALT: other tracks (kart, old layouts) in grey; K_PATH: footpaths / service roads; K_PARKING: car parks
// K_CORNER: named piece of track; K_COAST: sea shore (water on its right); K_ROAD/K_STREET: big / small streets
bool fsOk();
void pathFor(char *out, size_t n, char kind, const char *cid);
bool loadTrackHdr(const char *cid, TrackHdr &h, int16_t *pts, int maxPts);

enum { ST_STREET, ST_DRAWN, ST_COUNT, ST_SAT = 9 };      // button cycles streets / drawing (satellite kept in code, unused)
static constexpr int MAP_W = 284, MAP_H = 274;
void imagePath(char *out, size_t n, uint8_t style, const char *cid);

bool f1Fetch();                           // network task: decides itself what is stale

}  // namespace f1
