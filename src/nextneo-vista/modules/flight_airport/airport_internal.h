#pragma once
// Airport module internals: data side (airport_data.cpp: OSM map, traffic, details,
// Aena board; network task) shared with the screen (airport_screen.cpp).
// Everything here is guarded by mtx when the network task writes it.
#include <Arduino.h>
#include "modules/flight_radar/flight_types.h"

namespace airport {

enum FeatType : uint8_t { F_APRON, F_TERMINAL, F_TAXIWAY, F_RUNWAY, F_PARKING, F_HOLDING };
struct Feature { uint8_t type; bool closed; uint16_t count; uint32_t first; float widthM; char ref[8]; };
constexpr int MAX_FEATURES = 900, MAX_POINTS = 30000;
constexpr int BASE_PX = 1024;
constexpr float BASE_RADIUS_KM = 2.5f;            // ~1 km of city around the airfield
constexpr int MAX_AC = 24;
struct AirportTraffic { Aircraft ac[MAX_AC]; uint8_t src[MAX_AC]; float geomFt[MAX_AC]; uint16_t memoS[MAX_AC]; uint8_t count; uint8_t sourcesOk; uint32_t okMs; uint32_t version; bool failed; char error[40]; };
// traffic sources (bit mask in src[]): same readsb-style JSON, independent receiver networks
enum : uint8_t { SRC_ADSBLOL = 1, SRC_AIRPLANES = 2, SRC_ADSBFI = 4, SRC_ADSBONE = 8, SRC_AENA = 16 };
constexpr float SHOW_MAX_AGL_M = 200;   // touchdown / lift-off / short final; negative (baro error) shown too
constexpr float SHOW_KEEP_AGL_M = 300;  // once shown, kept until above this
struct AptDetails { char hex[7], cs[9], model[40], owner[40], airline[40], org[5], dst[5], orgCity[28], dstCity[28]; uint8_t state; };
enum : uint8_t { DET_NONE, DET_PENDING, DET_READY, DET_NOTFOUND, DET_FAILED };
constexpr int DET_CACHE = MAX_AC;
struct AenaStand { char hex[7], flight[9], other[5], gate[4], box[4], status[26], when[6]; bool dep; int16_t mins; };
constexpr int AENA_MAX = 30;

// ---- map model (PSRAM), written by the network task
extern Feature *feats;
extern int16_t *pts;                       // x,y pairs, local metres around the airport centre
extern int featCount, ptCount;
extern char mapCode[5];
extern double mapLat, mapLon;
extern float minX, maxX, minY, maxY;
extern volatile bool mapReady;
extern char mapMessage[48];
extern float mapRotDeg, rotC, rotS;        // map turned so the longest runway is horizontal
// ---- street basemap
extern uint16_t *basemap;                  // BASE_PX x BASE_PX RGB565
extern char baseCode[5];
extern volatile bool baseReady;
extern volatile uint32_t baseVersion;
// ---- traffic, details, Aena board
extern AirportTraffic *traffic;
extern AptDetails det;
extern AptDetails detCache[DET_CACHE];
extern int detCacheN;
extern AenaStand aenaStands[AENA_MAX];
extern int aenaN;

void codePair(const char *code, char *iata, char *icao);
void setRotation(float deg);
void currentAirport(char *code, double &lat, double &lon);
int detFind(const char *hex);              // caller holds mtx
float fieldElevM(const char *code);
float aglM(const Aircraft &a, float geomFt, float elevM);
bool isCgh(const char *code);
bool standPose(const char *box, double &lat, double &lon, float &heading, bool &hasHeading);
// lat/lon -> local metres (east, north), rotated clockwise by mapRotDeg
inline void proj(double lat, double lon, float &x, float &y) {
  const float e = (float)((lon - mapLon) * cos(mapLat * M_PI / 180.0) * 111320.0);
  const float n = (float)((lat - mapLat) * 110540.0);
  x = e * rotC + n * rotS;
  y = -e * rotS + n * rotC;
}

}  // namespace airport
