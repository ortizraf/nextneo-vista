// Flight Airport: vector map of the selected airport (OpenStreetMap via the public
// Overpass API) with the aircraft on/around it (ADSB.lol, same provider as the radar).
// Same architecture as the other screens:
//   * network task: airportFetch() -> map once per airport, traffic every poll
//   * UI thread:    airportOnEnter/OnExit/Render, map rendered once into a canvas,
//                   aircraft are small vector markers moved on top of it.
// Dark "night aviation" palette, text only where needed. Nothing is invented:
// fields the source does not send are shown as "—".
#include "../../app_screens.h"
#include "../flight_radar/flight_radar.h"
#include "../../services/aircraft_data_provider.h"
#include "../../services/https_fetch.h"
#include "../../services/psram_alloc.h"
#include "../../services/app_log.h"
#include "../../services/radar_map_provider.h"
#include "../flight_radar/flight_parser.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <math.h>
#include <LittleFS.h>
#include <time.h>
#include <algorithm>

// ---------------------------------------------------------------- palette (spec §6)
static constexpr uint32_t C_BG = 0x050D18, C_BG2 = 0x091626, C_RUNWAY = 0x182B3D, C_RUNWAY_EDGE = 0x29445B,
                          C_TAXI = 0x24435D, C_APRON = 0x101F30, C_TERMINAL = 0x14283B, C_TEXT = 0xB7C8D8,
                          C_TEXT2 = 0x637D95, C_AIRCRAFT = 0x43BFE8, C_SELECTED = 0xD5A84B, C_GATE = 0x8A9BAA,
                          C_ALERT = 0xC87965;

// ---------------------------------------------------------------- map model (PSRAM)
enum FeatType : uint8_t { F_APRON, F_TERMINAL, F_TAXIWAY, F_RUNWAY, F_PARKING, F_HOLDING };
struct Feature { uint8_t type; bool closed; uint16_t count; uint32_t first; float widthM; char ref[8]; };
static constexpr int MAX_FEATURES = 900, MAX_POINTS = 30000;
static Feature *feats;
static int16_t *pts;                 // x,y pairs, local metres (east, north) around the airport centre
static int featCount = 0, ptCount = 0;
static char mapCode[5] = "";         // airport the map belongs to
static double mapLat = 0, mapLon = 0;
static float minX, maxX, minY, maxY;
static volatile bool mapReady = false;
static uint32_t mapRetryAtMs = 0;
// Street basemap around the airport (same Esri source as the Flight Radar), north-up,
// re-tinted to the dark navy palette and sampled rotated under the vector map.
static constexpr int BASE_PX = 1024;
static constexpr float BASE_RADIUS_KM = 2.5f;            // ~1 km of city around the airfield
static uint16_t *basemap;                                 // BASE_PX x BASE_PX RGB565 (PSRAM)
static char baseCode[5] = "";
static volatile bool baseReady = false;
static volatile uint32_t baseVersion = 0;
static uint32_t baseRetryAtMs = 0;
static char mapMessage[48] = "";

// ---------------------------------------------------------------- traffic
static constexpr int MAX_AC = 24;
struct AirportTraffic { Aircraft ac[MAX_AC]; uint8_t src[MAX_AC]; float geomFt[MAX_AC]; uint16_t memoS[MAX_AC]; uint8_t count; uint8_t sourcesOk; uint32_t okMs; uint32_t version; bool failed; char error[40]; };
// traffic sources (bit mask in src[]): same readsb-style JSON, independent receiver networks
enum : uint8_t { SRC_ADSBLOL = 1, SRC_AIRPLANES = 2, SRC_ADSBFI = 4, SRC_ADSBONE = 8 };
static uint8_t *fetchSrc;
static float *fetchGeom;                   // GNSS altitude (alt_geom, ft) or NAN: independent of QNH
static Aircraft *secBuf; static uint8_t *secSrc; static float *secGeom;   // cache of the secondary networks
static WiFiClientSecure lolClient;         // keep-alive session to ADSB.lol while the screen is open

// Only aircraft on the ground or up to this height above the field are drawn
// (touchdown / lift-off / runway); higher traffic belongs to the Flight Radar.
static constexpr float SHOW_MAX_AGL_M = 200;   // touchdown / lift-off / short final; negative (baro error) shown too
static constexpr float SHOW_KEEP_AGL_M = 300;  // once shown, kept until above this
struct FieldElev { const char *code; float m; };
static const FieldElev kElev[] = {
  {"CGH",802},{"SBSP",802},{"GRU",750},{"SBGR",750},{"VCP",661},{"SBKP",661},{"SDU",3},{"SBRJ",3},{"GIG",9},{"SBGL",9},
  {"BSB",1066},{"SBBR",1066},{"CNF",827},{"SBCF",827},{"CWB",908},{"SBCT",908},{"POA",3},{"SBPA",3},{"FLN",5},{"SBFL",5},
  {"REC",10},{"SBRF",10},{"SSA",20},{"SBSV",20},{"FOR",25},{"SBFZ",25},{"BEL",16},{"SBBE",16},{"MAO",80},{"SBEG",80},
  {"NAT",83},{"SBSG",83},{"MCZ",118},{"SBMO",118},{"JPA",66},{"SBJP",66},{"IGU",240},{"SBFI",240},{"GYN",747},{"SBGO",747}};
static float fieldElevM(const char *code) { for (auto &e : kElev) if (!strcmp(code, e.code)) return e.m; return NAN; }
// height above the field in metres (0 on the ground, NAN unknown)
static float aglM(const Aircraft &a, float geomFt, float elevM) {
  if (a.onGround) return 0;
  if (isnan(elevM)) return NAN;
  if (!isnan(geomFt)) return geomFt * 0.3048f - elevM;           // GNSS: best
  if (a.hasAltitude) return a.altitudeFeet * 0.3048f - elevM;     // barometric (std 1013): +/- tens of m
  return NAN;
}
static AirportTraffic *traffic;        // written by the network task under mtx
static Aircraft *fetchBuf;

// ---------------------------------------------------------------- UI
lv_obj_t *airportView;
static lv_obj_t *canvas, *title, *statusDot, *statusLbl, *footLbl, *attribLbl, *msgLbl;
static lv_obj_t *statNum[3], *srcLbl;   // footer: 3 stats (number + caption) and the sources
static lv_obj_t *info, *infoCall, *infoState, *infoLine1, *infoLine2, *infoRoute, *infoModel;
static lv_obj_t *marker[MAX_AC];          // edge chevrons (aircraft outside the view)
static lv_obj_t *planeImg[MAX_AC];        // aircraft icons, same images/colours as the Flight Radar
extern const lv_image_dsc_t radar_icon_jet, radar_icon_light, radar_icon_heli;
static constexpr int ICON_PX = 28;
static lv_obj_t *northArrow, *northLbl;
static lv_point_precise_t northPts[5];
static lv_point_precise_t markerPts[MAX_AC][10];
static uint16_t *canvasBuf;
static constexpr int MAP_Y = 34, MAP_H = 256;          // map area under the header, above the footer
static float viewX = 0, viewY = 0, scale = 0.1f;       // view centre (m) and px per metre
static bool mapDrawn = false;
static uint32_t renderedTrafficVersion = UINT32_MAX;
static char selectedHex[7] = "";
static lv_point_t dragStart; static float dragViewX, dragViewY; static bool dragging = false;
static lv_point_t dragBase[2][24];        // icon/chevron positions when the drag started

// ---------------------------------------------------------------- helpers
struct AirportCodes { const char *iata, *icao; };
static const AirportCodes kCodes[] = {
  {"CGH","SBSP"},{"GRU","SBGR"},{"VCP","SBKP"},{"SDU","SBRJ"},{"GIG","SBGL"},{"BSB","SBBR"},{"CNF","SBCF"},
  {"CWB","SBCT"},{"POA","SBPA"},{"FLN","SBFL"},{"REC","SBRF"},{"SSA","SBSV"},{"FOR","SBFZ"},{"BEL","SBBE"},
  {"MAO","SBEG"},{"NAT","SBSG"},{"MCZ","SBMO"},{"JPA","SBJP"},{"IGU","SBFI"},{"GYN","SBGO"}};
static void codePair(const char *code, char *iata, char *icao) {
  snprintf(iata, 5, "%s", code); snprintf(icao, 5, "%s", code);
  for (auto &c : kCodes) {
    if (!strcmp(code, c.iata)) { snprintf(icao, 5, "%s", c.icao); return; }
    if (!strcmp(code, c.icao)) { snprintf(iata, 5, "%s", c.iata); return; }
  }
}
// Map orientation: the whole picture (map + aircraft + headings) is turned clockwise
// on screen by mapRotDeg so that the longest runway lies horizontal (landscape screen).
static float mapRotDeg = 0, rotC = 1, rotS = 0;
static void setRotation(float deg) { mapRotDeg = deg; rotC = cosf(deg * (float)M_PI / 180); rotS = sinf(deg * (float)M_PI / 180); }
// lat/lon -> local metres (east, north), then rotated clockwise by mapRotDeg
static inline void proj(double lat, double lon, float &x, float &y) {
  const float e = (float)((lon - mapLon) * cos(mapLat * M_PI / 180.0) * 111320.0);
  const float n = (float)((lat - mapLat) * 110540.0);
  x = e * rotC + n * rotS;          // clockwise rotation (y axis points north/up)
  y = -e * rotS + n * rotC;
}
static inline float sx(float x) { return SCR_W / 2.0f + (x - viewX) * scale; }
static inline float sy(float y) { return MAP_H / 2.0f - (y - viewY) * scale; }
static void currentAirport(char *code, double &lat, double &lon) {
  uint8_t r; getFlightRadarSettings(code, 5, nullptr, 0, r);
  getFlightRadarCenter(lat, lon);
}

// ---------------------------------------------------------------- network: map (Overpass)
static void urlEncode(const char *in, String &out) {
  static const char *hex = "0123456789ABCDEF";
  for (const char *p = in; *p; ++p) {
    const uint8_t c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
}

// ---------------------------------------------------------------- map cache in flash
// Normalised map saved per airport so a reboot does not download it again.
struct MapFileHeader { uint32_t magic; char code[5]; double lat, lon; float rot, minX, maxX, minY, maxY; int32_t nf, np; uint32_t savedAt; };
static constexpr uint32_t MAP_MAGIC = 0x4E564131;            // "NVA1"
static constexpr uint32_t MAP_MAX_AGE_S = 30UL * 24 * 3600;  // refresh from OSM after ~30 days
static bool fsReady() {
  static int state = -1;
  if (state < 0) state = LittleFS.begin(true) ? 1 : 0;      // formats an empty data partition once
  return state == 1;
}
static void mapPath(const char *code, char *out, size_t n) { snprintf(out, n, "/apt_%s.bin", code); }

static bool loadMapFile(const char *code, double lat, double lon) {
  if (!fsReady()) return false;
  mapReady = false;                       // the UI must not read the arrays while they are replaced
  char path[24]; mapPath(code, path, sizeof path);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  MapFileHeader h;
  bool ok = f.read((uint8_t *)&h, sizeof h) == sizeof h && h.magic == MAP_MAGIC && !strcmp(h.code, code) &&
            fabs(h.lat - lat) < 1e-4 && fabs(h.lon - lon) < 1e-4 && h.nf <= MAX_FEATURES && h.np <= MAX_POINTS;
  const time_t now = time(nullptr);
  if (ok && now > 1700000000 && (uint32_t)now - h.savedAt > MAP_MAX_AGE_S) ok = false;   // too old: re-download
  if (ok) ok = f.read((uint8_t *)feats, h.nf * sizeof(Feature)) == h.nf * sizeof(Feature) &&
               f.read((uint8_t *)pts, h.np * 4) == (size_t)h.np * 4;
  f.close();
  if (!ok) return false;
  mapLat = h.lat; mapLon = h.lon; setRotation(h.rot);
  minX = h.minX; maxX = h.maxX; minY = h.minY; maxY = h.maxY;
  featCount = h.nf; ptCount = h.np;
  snprintf(mapCode, sizeof mapCode, "%s", code);
  LogV.printf("Airport: mapa %s carregado da flash (%d elementos)\n", code, featCount);
  mapReady = true;
  return true;
}

static void saveMapFile(const char *code) {
  if (!fsReady()) return;
  char path[24]; mapPath(code, path, sizeof path);
  File f = LittleFS.open(path, "w");
  if (!f) return;
  MapFileHeader h = {MAP_MAGIC, {}, mapLat, mapLon, mapRotDeg, minX, maxX, minY, maxY, featCount, ptCount, (uint32_t)time(nullptr)};
  snprintf(h.code, sizeof h.code, "%s", code);
  f.write((const uint8_t *)&h, sizeof h);
  f.write((const uint8_t *)feats, featCount * sizeof(Feature));
  f.write((const uint8_t *)pts, ptCount * 4);
  f.close();
  LogV.printf("Airport: mapa %s salvo na flash (%u bytes)\n", code, (unsigned)(sizeof h + featCount * sizeof(Feature) + ptCount * 4));
}

// JSON (Overpass format) -> compact model. Used for downloads and for the built-in maps.
static bool parseMapJson(const char *json, size_t len, const char *code, double lat, double lon, uint32_t t0, bool save) {
  JsonDocument filter(SpiRamJsonAllocator::instance());
  JsonObject f = filter["elements"][0].to<JsonObject>();
  f["type"] = true; f["lat"] = true; f["lon"] = true;
  f["tags"]["aeroway"] = true; f["tags"]["ref"] = true; f["tags"]["width"] = true;
  f["geometry"][0]["lat"] = true; f["geometry"][0]["lon"] = true;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  const DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (err) { snprintf(mapMessage, sizeof mapMessage, "Mapa: JSON %s", err.c_str()); return false; }

  // convert to the compact model (the UI is not reading it: mapReady is false)
  mapReady = false;
  mapLat = lat; mapLon = lon;
  setRotation(0);                       // parse north-up, then align to the main runway below
  featCount = ptCount = 0;
  minX = minY = 1e9f; maxX = maxY = -1e9f;
  for (JsonObjectConst e : doc["elements"].as<JsonArrayConst>()) {
    if (featCount >= MAX_FEATURES) break;
    const char *aw = e["tags"]["aeroway"] | "";
    Feature ft = {};
    if (!strcmp(aw, "runway")) ft.type = F_RUNWAY;
    else if (!strcmp(aw, "taxiway")) ft.type = F_TAXIWAY;
    else if (!strcmp(aw, "apron")) ft.type = F_APRON;
    else if (!strcmp(aw, "terminal")) ft.type = F_TERMINAL;
    else if (!strcmp(aw, "parking_position") || !strcmp(aw, "gate")) ft.type = F_PARKING;
    else if (!strcmp(aw, "holding_position")) ft.type = F_HOLDING;
    else continue;
    snprintf(ft.ref, sizeof ft.ref, "%s", e["tags"]["ref"] | "");
    ft.widthM = atof(e["tags"]["width"] | "0");
    ft.first = ptCount;
    auto add = [&](double la, double lo) {
      if (ptCount >= MAX_POINTS) return;
      float x, y; proj(la, lo, x, y);
      pts[ptCount * 2] = (int16_t)lroundf(x); pts[ptCount * 2 + 1] = (int16_t)lroundf(y);
      ++ptCount; ++ft.count;
      if (ft.type == F_RUNWAY) { minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y); }
    };
    if (!strcmp(e["type"] | "", "node")) add(e["lat"] | 0.0, e["lon"] | 0.0);
    else for (JsonObjectConst g : e["geometry"].as<JsonArrayConst>()) add(g["lat"] | 0.0, g["lon"] | 0.0);
    if (!ft.count) continue;
    ft.closed = ft.count > 2 && pts[ft.first * 2] == pts[(ft.first + ft.count - 1) * 2] &&
                pts[ft.first * 2 + 1] == pts[(ft.first + ft.count - 1) * 2 + 1];
    feats[featCount++] = ft;
  }
  // turn everything so the longest runway is horizontal
  {
    float best = 0, bdx = 1, bdy = 0;
    for (int i = 0; i < featCount; ++i) {
      if (feats[i].type != F_RUNWAY || feats[i].count < 2) continue;
      const int a = feats[i].first, b = feats[i].first + feats[i].count - 1;
      const float dx = pts[b * 2] - pts[a * 2], dy = pts[b * 2 + 1] - pts[a * 2 + 1];
      if (dx * dx + dy * dy > best) { best = dx * dx + dy * dy; bdx = dx; bdy = dy; }
    }
    if (best > 0) {
      const float bearing = atan2f(bdx, bdy) * 180 / (float)M_PI;      // compass bearing of the runway
      float rot = fmodf(270.0f - bearing, 180.0f); if (rot < 0) rot += 180;
      setRotation(rot);
      minX = minY = 1e9f; maxX = maxY = -1e9f;
      for (int k = 0; k < ptCount; ++k) {
        const float e = pts[k * 2], n = pts[k * 2 + 1];
        const float x = e * rotC + n * rotS, y = -e * rotS + n * rotC;
        pts[k * 2] = (int16_t)lroundf(x); pts[k * 2 + 1] = (int16_t)lroundf(y);
      }
      for (int i = 0; i < featCount; ++i) if (feats[i].type == F_RUNWAY)
        for (uint32_t k = feats[i].first; k < feats[i].first + feats[i].count; ++k) {
          minX = min(minX, (float)pts[k * 2]); maxX = max(maxX, (float)pts[k * 2]);
          minY = min(minY, (float)pts[k * 2 + 1]); maxY = max(maxY, (float)pts[k * 2 + 1]);
        }
      LogV.printf("Airport: pista principal %.0f graus -> mapa girado %.0f graus\n", bearing, rot);
    }
  }
  if (minX > maxX) { minX = -1500; maxX = 1500; minY = -1200; maxY = 1200; }   // nothing mapped
  { const float mx = (maxX - minX) * 0.06f + 60, my = (maxY - minY) * 0.06f + 60; minX -= mx; maxX += mx; minY -= my; maxY += my; }
  snprintf(mapCode, sizeof mapCode, "%s", code);
  mapMessage[0] = 0;
  LogV.printf("Airport: mapa %s %u bytes, %d elementos, %d pontos em %lums\n", code, (unsigned)len,
             featCount, ptCount, (unsigned long)(millis() - t0));
  mapReady = true;
  if (save) saveMapFile(code);
  return true;
}

static bool fetchMap(const char *code, double lat, double lon) {
  if (!feats) {
    feats = (Feature *)psramCalloc(MAX_FEATURES, sizeof(Feature));
    pts = (int16_t *)psramMalloc(MAX_POINTS * 2 * sizeof(int16_t));
    if (!feats || !pts) { snprintf(mapMessage, sizeof mapMessage, "Sem memória para o mapa"); return false; }
  }
  // ~2.4 km around the reference point covers the airfield of the airports in the list
  char q[640];
  const double dLat = 0.022, dLon = 0.026;
  char bb[80];
  snprintf(bb, sizeof bb, "(%.5f,%.5f,%.5f,%.5f)", lat - dLat, lon - dLon, lat + dLat, lon + dLon);
  snprintf(q, sizeof q,
           "[out:json][timeout:25];(way[\"aeroway\"~\"^(runway|taxiway|apron|terminal|parking_position)$\"]%s;"
           "node[\"aeroway\"~\"^(parking_position|holding_position|gate)$\"]%s;);out geom qt;", bb, bb);
  String body = "data=";
  urlEncode(q, body);

  static PsramBuffer resp(3 * 1024 * 1024);
  resp.clear();
  WiFiClientSecure client;
  client.setInsecure();                 // public read-only data
  client.setHandshakeTimeout(15);
  if (!tlsMemoryAvailable("overpass")) { snprintf(mapMessage, sizeof mapMessage, "Memória baixa p/ TLS"); return false; }
  const uint32_t t0 = millis();
  // main instance first, then public mirrors (the main one answers 406/429 to some clients)
  static const char *kEndpoints[] = {"https://overpass-api.de/api/interpreter",
                                     "https://overpass.private.coffee/api/interpreter",
                                     "https://maps.mail.ru/osm/tools/overpass/api/interpreter"};
  HttpsFetchOptions opt;
  opt.userAgent = "NextNeoVista/1.0 (ESP32-S3 airport display; github.com/nextneo)";
  // ONE server per attempt (rotating), so a slow/overloaded instance never blocks
  // the traffic polling for long; the next attempt (30 s later) tries the next one.
  static uint8_t nextEndpoint = 0;
  const char *ep = kEndpoints[nextEndpoint];
  resp.clear();
  int status = httpsFetch(client, ep, resp, 25000, false, body.c_str(), "application/x-www-form-urlencoded", opt);
  client.stop();
  LogV.printf("Airport: Overpass %s -> HTTP %d (%u bytes)\n", ep + 8, status, (unsigned)resp.length());
  if (!(status == 200 && !resp.overflowed() && resp.length() && resp.c_str()[0] == '{')) {
    nextEndpoint = (nextEndpoint + 1) % (sizeof kEndpoints / sizeof kEndpoints[0]);
    if (status == 200) status = -2;    // 200 with an HTML error page
  }
  if (status != 200 || resp.overflowed()) {
    snprintf(mapMessage, sizeof mapMessage, "Servidor do mapa ocupado (%d), tentando outro em 30 s", status);
    Log.printf("Airport: Overpass HTTP %d (%u bytes)\n", status, (unsigned)resp.length());
    return false;
  }
  if (!screenJobStillCurrent()) return false;

  return parseMapJson(resp.c_str(), resp.length(), code, lat, lon, t0, true);
}

// Merge one aircraft by ICAO hex: the fresher position wins, missing fields are filled in.
static void mergeOne(Aircraft *buf, uint8_t *src, float *geomBuf, uint8_t &n, const Aircraft &a, float geom, uint8_t srcBit) {
    int k = -1;
    for (int i = 0; i < n; ++i) if (!strcmp(buf[i].icao24, a.icao24)) { k = i; break; }
    if (k < 0) {                                          // seen only by this network
      if (n >= MAX_AC) return;
      buf[n] = a; src[n] = srcBit; geomBuf[n] = geom; ++n;
      return;
    }
    Aircraft &b = buf[k];
    src[k] |= srcBit;
    const float ageA = a.hasPositionAge ? a.positionAgeSeconds : 999, ageB = b.hasPositionAge ? b.positionAgeSeconds : 999;
    if (isnan(geomBuf[k]) || (ageA < ageB && !isnan(geom))) geomBuf[k] = geom;
    if (ageA < ageB) {                                    // fresher position from this source
      b.latitude = a.latitude; b.longitude = a.longitude; b.positionAgeSeconds = a.positionAgeSeconds; b.hasPositionAge = a.hasPositionAge;
      if (a.hasHeading) { b.headingDegrees = a.headingDegrees; b.hasHeading = true; }
      if (a.hasSpeed) { b.speedKnots = a.speedKnots; b.hasSpeed = true; }
      if (a.hasAltitude || a.onGround) { b.altitudeFeet = a.altitudeFeet; b.hasAltitude = a.hasAltitude; b.onGround = a.onGround; }
    }
    if (!b.callsign[0]) memcpy(b.callsign, a.callsign, sizeof b.callsign);
    if (!b.type[0]) memcpy(b.type, a.type, sizeof b.type);
    if (!b.registration[0]) memcpy(b.registration, a.registration, sizeof b.registration);
    if (!b.hasHeading && a.hasHeading) { b.headingDegrees = a.headingDegrees; b.hasHeading = true; }
    if (!b.hasSpeed && a.hasSpeed) { b.speedKnots = a.speedKnots; b.hasSpeed = true; }
}

// ---------------------------------------------------------------- network: extra traffic sources
// GET one readsb-style endpoint and merge into fetchBuf (n entries) by ICAO hex.
// The fresher position wins; flags/fields missing in one source are filled from the other.
static void mergeOne(Aircraft *buf, uint8_t *src, float *geomBuf, uint8_t &n, const Aircraft &a, float geom, uint8_t srcBit);

// GET one readsb-style endpoint and merge it into buf/src/geomBuf (n entries).
// keep != nullptr: reuse that keep-alive TLS session (no handshake on every poll).
static int lastFetchStatus = 0;           // HTTP status of the last fetchExtra (429 detection)
static bool fetchExtra(const char *url, uint8_t srcBit, double lat, double lon, float radiusNm,
                       Aircraft *buf, uint8_t *src, float *geomBuf, uint8_t &n, WiFiClientSecure *keep = nullptr) {
  static PsramBuffer body(256 * 1024);
  body.clear();
  if (!tlsMemoryAvailable("airport-src")) return false;
  WiFiClientSecure fresh;
  WiFiClientSecure &client = keep ? *keep : fresh;
  if (!keep) { fresh.setInsecure(); fresh.setHandshakeTimeout(10); }   // public read-only data
  else if (!client.connected()) client.stop();          // server closed it: free the old context first
  const bool reused = keep && client.connected();            // diagnostic: did keep-alive work?
  const uint32_t t0 = millis();
  const int status = httpsFetch(client, url, body, 8000, keep != nullptr);
  lastFetchStatus = status;
  if (!keep || status != 200) client.stop();
  const char *srcName = srcBit == SRC_AIRPLANES ? "airplanes.live" : srcBit == SRC_ADSBFI ? "adsb.fi" : srcBit == SRC_ADSBONE ? "adsb.one" : "ADSB.lol";
  if (status != 200 || !body.length()) { Log.printf("Airport: %s HTTP %d\n", srcName, status); return false; }
  if (!screenJobStillCurrent()) return false;
  JsonDocument filter(SpiRamJsonAllocator::instance());
  for (const char *key : {"ac", "aircraft"})
    for (const char *f : {"hex", "flight", "t", "r", "category", "squawk", "emergency", "lat", "lon", "alt_baro",
                          "gs", "track", "true_heading", "baro_rate", "geom_rate", "seen_pos"})
      filter[key][0][f] = true;
  for (const char *key : {"ac", "aircraft"})                // readsb moves old positions here (seen_pos > 60 s)
    for (const char *f : {"lat", "lon", "seen_pos"}) filter[key][0]["lastPosition"][f] = true;
  for (const char *key : {"ac", "aircraft"}) filter[key][0]["alt_geom"] = true;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) return false;
  JsonArray list = doc["ac"].as<JsonArray>();
  if (list.isNull()) list = doc["aircraft"].as<JsonArray>();
  if (list.isNull()) return false;
  Aircraft a;
  for (JsonVariant item : list) {
    if (!item.is<JsonObject>()) continue;
    // Aircraft on the ground send positions rarely (and are shadowed by buildings):
    // readsb then drops lat/lon and keeps the last one in "lastPosition". Use it.
    if (item["lat"].isNull() && item["lastPosition"].is<JsonObject>()) {
      item["lat"] = item["lastPosition"]["lat"]; item["lon"] = item["lastPosition"]["lon"];
      item["seen_pos"] = item["lastPosition"]["seen_pos"];
    }
    if (!parseAdsbLolAircraftObject(item.as<JsonObjectConst>(), a)) continue;
    const float geom = item["alt_geom"].is<float>() ? item["alt_geom"].as<float>() : NAN;
    // ground positions may be up to 10 min old (aircraft holding / taxiing slowly); airborne: 2 min
    if (a.hasPositionAge && a.positionAgeSeconds > (a.onGround || !a.hasAltitude ? 600 : 120)) continue;
    const double dLat = (a.latitude - lat) * 60.0, dLon = (a.longitude - lon) * 60.0 * cos(lat * M_PI / 180);
    if (sqrt(dLat * dLat + dLon * dLon) > radiusNm) continue;
    mergeOne(buf, src, geomBuf, n, a, geom, srcBit);
  }
  LogV.printf("Airport: %s ok em %lums (%u bytes)%s\n", srcName,
             (unsigned long)(millis() - t0), (unsigned)body.length(),
             keep ? (reused ? " [conexao reaproveitada]" : " [conexao nova]") : "");
  return true;
}

// Built-in maps (airport_maps.c): no download needed for these airports.
extern "C" const char kMapCGH[];
extern "C" const size_t kMapCGHLen;
static bool loadBuiltinMap(const char *code, double lat, double lon) {
  if (strcmp(code, "CGH") && strcmp(code, "SBSP")) return false;
  return parseMapJson(kMapCGH, kMapCGHLen, code, lat, lon, millis(), false);
}

// ---------------------------------------------------------------- network: job entry
// ---------------------------------------------------------------- details of the selected aircraft
// Same source as the Flight Radar (adsbdb.com): model, owner, airline, route.
struct AptDetails { char hex[7], cs[9], model[40], owner[40], airline[40], org[5], dst[5], orgCity[28], dstCity[28]; uint8_t state; };
enum : uint8_t { DET_NONE, DET_PENDING, DET_READY, DET_NOTFOUND, DET_FAILED };
static AptDetails det;                      // guarded by mtx

static void copyStr(JsonVariantConst v, char *out, size_t n) { snprintf(out, n, "%s", v.is<const char *>() ? v.as<const char *>() : ""); }

static void fetchDetails() {
  AptDetails d;
  xSemaphoreTake(mtx, portMAX_DELAY); d = det; xSemaphoreGive(mtx);
  if (d.state != DET_PENDING) return;
  lolClient.stop();                                       // one TLS context at a time
  char url[128];
  snprintf(url, sizeof url, "https://api.adsbdb.com/v0/aircraft/%s%s%s", d.hex, d.cs[0] ? "?callsign=" : "", d.cs);
  static PsramBuffer body(32 * 1024);
  body.clear();
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
  const int status = tlsMemoryAvailable("adsbdb") ? httpsFetch(client, url, body, 8000, false) : -1;
  client.stop();
  JsonDocument doc(SpiRamJsonAllocator::instance());
  const bool parsed = (status == 200 || status == 404) && !deserializeJson(doc, body.c_str(), body.length());
  JsonObjectConst ac = doc["response"]["aircraft"], fr = doc["response"]["flightroute"];
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (strcmp(det.hex, d.hex)) { xSemaphoreGive(mtx); return; }   // selection changed meanwhile
  if (!parsed) det.state = DET_FAILED;
  else if (ac.isNull() && fr.isNull()) det.state = DET_NOTFOUND;
  else {
    char man[24] = "", type[24] = "";
    copyStr(ac["manufacturer"], man, sizeof man); copyStr(ac["type"], type, sizeof type);
    snprintf(det.model, sizeof det.model, "%s%s%s", man, man[0] ? " " : "", type);
    copyStr(ac["registered_owner"], det.owner, sizeof det.owner);
    copyStr(fr["airline"]["name"], det.airline, sizeof det.airline);
    copyStr(fr["origin"]["iata_code"], det.org, sizeof det.org);
    copyStr(fr["destination"]["iata_code"], det.dst, sizeof det.dst);
    copyStr(fr["origin"]["municipality"], det.orgCity, sizeof det.orgCity);
    copyStr(fr["destination"]["municipality"], det.dstCity, sizeof det.dstCity);
    det.state = DET_READY;
  }
  xSemaphoreGive(mtx);
  LogV.printf("Airport: detalhes %s -> HTTP %d (%s)\n", d.hex, status, det.state == DET_READY ? "ok" : "sem dados");
}

// Network task: close the keep-alive session when the Airport screen is not active.
static volatile bool lolOpen = false;
void airportReleaseConnection() {
  if (!lolOpen) return;
  lolClient.stop();
  lolOpen = false;
  LogV.println("Airport: conexão ADSB.lol liberada");
}

bool airportFetch() {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t cycleStart = millis();
  static uint32_t lastCycleStart = 0;
  char code[5]; double lat, lon;
  currentAirport(code, lat, lon);
  if (!traffic) {
    traffic = (AirportTraffic *)psramCalloc(1, sizeof(AirportTraffic));
    fetchBuf = (Aircraft *)psramCalloc(MAX_AC, sizeof(Aircraft));
    fetchSrc = (uint8_t *)psramCalloc(MAX_AC, 1);
    fetchGeom = (float *)psramCalloc(MAX_AC, sizeof(float));
    secBuf = (Aircraft *)psramCalloc(MAX_AC, sizeof(Aircraft));
    secSrc = (uint8_t *)psramCalloc(MAX_AC, 1);
    secGeom = (float *)psramCalloc(MAX_AC, sizeof(float));
    if (!traffic || !fetchBuf || !fetchSrc || !fetchGeom || !secBuf || !secSrc || !secGeom) return false;
  }
  if (strcmp(code, mapCode) && (int32_t)(millis() - mapRetryAtMs) >= 0) {   // map once per airport
    if (!feats) {
      feats = (Feature *)psramCalloc(MAX_FEATURES, sizeof(Feature));
      pts = (int16_t *)psramMalloc(MAX_POINTS * 2 * sizeof(int16_t));
    }
    if (feats && pts && loadBuiltinMap(code, lat, lon)) { /* built into the firmware */ }
    else if (feats && pts && loadMapFile(code, lat, lon)) { /* from flash */ }
    else if (!fetchMap(code, lat, lon)) { mapRetryAtMs = millis() + 30000; }
    if (!screenJobStillCurrent()) return false;
  }
  // basemap once per airport (after the vector map), cheap to retry later on failure
  if (mapReady && !strcmp(mapCode, code) && strcmp(baseCode, code) && (int32_t)(millis() - baseRetryAtMs) >= 0) {
    if (!basemap) basemap = (uint16_t *)psramMalloc(BASE_PX * BASE_PX * 2);
    if (basemap) {
      lolClient.stop();                                   // one TLS context at a time
      char err[48] = "";
      baseReady = false;
      if (fetchRadarMap(lat, lon, BASE_RADIUS_KM, basemap, BASE_PX, 0, err, sizeof err)) {
        snprintf(baseCode, sizeof baseCode, "%s", code); baseReady = true; ++baseVersion;
        LogV.printf("Airport: mapa de ruas %s carregado\n", code);
      } else { baseRetryAtMs = millis() + 120000; Log.printf("Airport: mapa de ruas falhou (%s)\n", err); }
    }
    if (!screenJobStillCurrent()) return false;
  }

  // Aggregate 3 public networks: each has receivers in different places, so together
  // they see more aircraft on the apron/taxiways. Merge by ICAO hex, keep the freshest.
  const float radiusNm = 2.0f;                         // airfield + final approach / initial climb
  uint8_t n = 0, okMask = 0; char err[40] = "";
  char url[128];
  // Every poll (~10 s, only while this screen is shown): ADSB.lol and the other networks,
  // nothing cached between polls. Every request re-checks that the Airport screen is still
  // the active one, so leaving it stops the queries at once. One TLS context at a time.
  static bool lolConfigured = false;
  if (!lolConfigured) { lolClient.setInsecure(); lolClient.setHandshakeTimeout(10); lolConfigured = true; }
  snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.5f/%.5f/%u", lat, lon, (unsigned)ceilf(radiusNm));
  // ADSB.lol closes the connection after every answer (log: always "conexao nova"), so each
  // poll pays a ~2 s TLS handshake, and it answers 429 when polled faster than ~2-3 s.
  // Respect that: after a 429 wait 6 s between ADSB.lol polls for the next 60 s.
  static uint32_t lolSlowUntil = 0, lolLastMs = 0;
  const bool slow = (int32_t)(millis() - lolSlowUntil) < 0;
  if (!slow || millis() - lolLastMs >= 6000) {
    lolOpen = true; lolLastMs = millis();
    if (fetchExtra(url, SRC_ADSBLOL, lat, lon, radiusNm, fetchBuf, fetchSrc, fetchGeom, n, &lolClient)) okMask |= SRC_ADSBLOL;
    else {
      snprintf(err, sizeof err, "ADSB.lol sem resposta");
      if (lastFetchStatus == 429) { lolSlowUntil = millis() + 60000; Log.println("Airport: ADSB.lol pediu calma (429): 6 s por 60 s"); }
    }
  }

  static uint32_t secMs = 0;
  static uint8_t secN = 0, secOk = 0;
  if (screenJobStillCurrent()) {                          // every poll, no cache: all networks each time
    // keep the ADSB.lol session if there is room for a second TLS context (saves a ~2 s handshake)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 60000) lolClient.stop();
    uint8_t m = 0, bits = 0;
    static uint32_t airplanesPausedUntil = 0;           // airplanes.live answers 403 to this device
    if (screenJobStillCurrent() && (int32_t)(millis() - airplanesPausedUntil) >= 0) {
      snprintf(url, sizeof url, "https://api.airplanes.live/v2/point/%.5f/%.5f/%u", lat, lon, (unsigned)ceilf(radiusNm));
      if (fetchExtra(url, SRC_AIRPLANES, lat, lon, radiusNm, secBuf, secSrc, secGeom, m)) bits |= SRC_AIRPLANES;
      else airplanesPausedUntil = millis() + 30UL * 60 * 1000;
    }
    snprintf(url, sizeof url, "https://opendata.adsb.fi/api/v2/lat/%.5f/lon/%.5f/dist/%u", lat, lon, (unsigned)ceilf(radiusNm));
    if (screenJobStillCurrent() && fetchExtra(url, SRC_ADSBFI, lat, lon, radiusNm, secBuf, secSrc, secGeom, m)) bits |= SRC_ADSBFI;
    static uint32_t onePausedUntil = 0;                 // adsb.one also answers 403: back off 30 min
    if (screenJobStillCurrent() && (int32_t)(millis() - onePausedUntil) >= 0) {
      snprintf(url, sizeof url, "https://api.adsb.one/v2/point/%.5f/%.5f/%u", lat, lon, (unsigned)ceilf(radiusNm));
      if (fetchExtra(url, SRC_ADSBONE, lat, lon, radiusNm, secBuf, secSrc, secGeom, m)) bits |= SRC_ADSBONE;
      else onePausedUntil = millis() + 30UL * 60 * 1000;
    }
    secN = m; secOk = bits; secMs = millis();
  }
  if (secOk) {                                          // merge what the other networks answered now
    for (int i = 0; i < secN; ++i) {
      Aircraft a = secBuf[i];
      if (a.hasPositionAge) a.positionAgeSeconds += (millis() - secMs) / 1000.0f;   // age it honestly
      mergeOne(fetchBuf, fetchSrc, fetchGeom, n, a, secGeom[i], secSrc[i]);
    }
    okMask |= secOk;
  }
  if (!screenJobStillCurrent()) return false;
  const bool ok = okMask != 0;

  // Memory (1 min): an aircraft that stops being reported stays at its last position,
  // drawn dimmed with "visto há Xs", so short gaps in ground coverage don't empty the map.
  static Aircraft recent[MAX_AC]; static float recentGeom[MAX_AC]; static uint8_t recentSrc[MAX_AC];
  static uint32_t recentMs[MAX_AC]; static uint8_t recentN = 0;
  static uint16_t memo[MAX_AC];
  const uint32_t nowMs = millis();
  if (ok) {
    for (int i = 0; i < n; ++i) {                       // refresh the memory with what is live now
      int k = -1;
      for (int j = 0; j < recentN; ++j) if (!strcmp(recent[j].icao24, fetchBuf[i].icao24)) { k = j; break; }
      if (k < 0 && recentN < MAX_AC) k = recentN++;
      if (k >= 0) { recent[k] = fetchBuf[i]; recentGeom[k] = fetchGeom[i]; recentSrc[k] = fetchSrc[i]; recentMs[k] = nowMs; }
      memo[i] = 0;
    }
    for (int j = 0; j < recentN;) {                     // expire after 60 s
      if (nowMs - recentMs[j] > 60000UL) { recent[j] = recent[--recentN]; recentGeom[j] = recentGeom[recentN]; recentSrc[j] = recentSrc[recentN]; recentMs[j] = recentMs[recentN]; continue; }
      bool live = false;
      for (int i = 0; i < n; ++i) if (!strcmp(fetchBuf[i].icao24, recent[j].icao24)) { live = true; break; }
      if (!live && n < MAX_AC) {                        // append the remembered one
        fetchBuf[n] = recent[j]; fetchGeom[n] = recentGeom[j]; fetchSrc[n] = recentSrc[j];
        memo[n] = (uint16_t)((nowMs - recentMs[j]) / 1000 + 1);
        ++n;
      }
      ++j;
    }
  }

  int g = 0, multi = 0, byOnly[3] = {0, 0, 0};
  for (int i = 0; i < n; ++i) {
    g += fetchBuf[i].onGround;
    if (fetchSrc[i] & (fetchSrc[i] - 1)) ++multi;
    else byOnly[fetchSrc[i] == SRC_ADSBLOL ? 0 : fetchSrc[i] == SRC_AIRPLANES ? 1 : 2]++;
  }
  if (ok) LogV.printf("Airport: %u aeronaves (%d em solo) | fontes %c%c%c | em >1 fonte %d, so ADSB.lol %d, so airplanes.live %d, so adsb.fi %d\n",
                     n, g, okMask & SRC_ADSBLOL ? 'L' : '-', okMask & SRC_AIRPLANES ? 'A' : '-', okMask & SRC_ADSBFI ? 'F' : '-',
                     multi, byOnly[0], byOnly[1], byOnly[2]);
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (ok) {
    memcpy(traffic->ac, fetchBuf, n * sizeof(Aircraft)); memcpy(traffic->src, fetchSrc, n); memcpy(traffic->geomFt, fetchGeom, n * sizeof(float)); memcpy(traffic->memoS, memo, n * sizeof(uint16_t));
    traffic->count = n; traffic->sourcesOk = okMask; traffic->okMs = millis(); traffic->failed = false;
  } else { traffic->failed = true; snprintf(traffic->error, sizeof traffic->error, "%s", err); }
  traffic->version++;
  xSemaphoreGive(mtx);
  if (screenJobStillCurrent()) fetchDetails();   // adsbdb only for the aircraft tapped on this screen
  LogV.printf("Airport: ciclo %lums, intervalo %lums, interno livre %u (maior bloco %u)\n", (unsigned long)(millis() - cycleStart),
             (unsigned long)(lastCycleStart ? cycleStart - lastCycleStart : 0), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  lastCycleStart = cycleStart;
  return ok;
}

// ---------------------------------------------------------------- map rendering (canvas, once per view)
static void fillPolygon(const Feature &ft, uint16_t color) {
  // even-odd scanline fill straight into the RGB565 canvas buffer
  static float xs[64];
  for (int y = 0; y < MAP_H; ++y) {
    int nx = 0;
    const float fy = y + 0.5f;
    for (uint32_t k = 0; k + 1 < ft.count; ++k) {
      const float y1 = sy(pts[(ft.first + k) * 2 + 1]), y2 = sy(pts[(ft.first + k + 1) * 2 + 1]);
      if ((y1 <= fy && y2 > fy) || (y2 <= fy && y1 > fy)) {
        const float x1 = sx(pts[(ft.first + k) * 2]), x2 = sx(pts[(ft.first + k + 1) * 2]);
        if (nx < 64) xs[nx++] = x1 + (fy - y1) * (x2 - x1) / (y2 - y1);
      }
    }
    if (nx < 2) continue;
    std::sort(xs, xs + nx);
    uint16_t *row = canvasBuf + y * SCR_W;
    for (int i = 0; i + 1 < nx; i += 2) {
      int a = max(0, (int)ceilf(xs[i])), b = min(SCR_W - 1, (int)floorf(xs[i + 1]));
      for (int x = a; x <= b; ++x) row[x] = color;
    }
  }
}

static void strokeFeature(lv_layer_t *layer, const Feature &ft, uint32_t color, int width, lv_opa_t opa = LV_OPA_COVER) {
  lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(color); d.width = width; d.opa = opa; d.round_start = d.round_end = 1;
  for (uint32_t k = 0; k + 1 < ft.count; ++k) {
    d.p1.x = (lv_value_precise_t)sx(pts[(ft.first + k) * 2]);     d.p1.y = (lv_value_precise_t)sy(pts[(ft.first + k) * 2 + 1]);
    d.p2.x = (lv_value_precise_t)sx(pts[(ft.first + k + 1) * 2]); d.p2.y = (lv_value_precise_t)sy(pts[(ft.first + k + 1) * 2 + 1]);
    if ((d.p1.x < -50 && d.p2.x < -50) || (d.p1.x > SCR_W + 50 && d.p2.x > SCR_W + 50) ||
        (d.p1.y < -50 && d.p2.y < -50) || (d.p1.y > MAP_H + 50 && d.p2.y > MAP_H + 50)) continue;
    lv_draw_line(layer, &d);
  }
}

// Inverse mapping: each canvas pixel -> local metres -> un-rotate -> basemap pixel.
// The radar provider tints teal; re-map its luminance to a dim navy ramp so the city
// stays in the background and runways/taxiways remain the main element.
static void drawBasemap() {
  static uint16_t lut[64]; static bool lutReady = false;
  if (!lutReady) {
    const float a[3] = {0x05, 0x0D, 0x18}, b[3] = {0x2C, 0x45, 0x60};    // C_BG -> muted steel blue
    for (int i = 0; i < 64; ++i) {
      const float v = powf(i / 63.0f, 1.2f) * 0.85f;
      lut[i] = lv_color_to_u16(lv_color_make((uint8_t)(a[0] + (b[0] - a[0]) * v), (uint8_t)(a[1] + (b[1] - a[1]) * v),
                                             (uint8_t)(a[2] + (b[2] - a[2]) * v)));
    }
    lutReady = true;
  }
  const float mPerPx = BASE_RADIUS_KM * 2000.0f / BASE_PX;
  const float inv = 1.0f / scale, half = BASE_PX / 2.0f;
  const uint16_t bg = lv_color_to_u16(lv_color_hex(C_BG));
  // (u,v) is affine in (px,py): step it in 16.16 fixed point instead of 4 float ops per pixel
  const float k = inv / mPerPx;
  const int32_t duX = (int32_t)lroundf(rotC * k * 65536), dvX = (int32_t)lroundf(-rotS * k * 65536);   // per +1 px
  const int32_t duY = (int32_t)lroundf(rotS * k * 65536), dvY = (int32_t)lroundf(rotC * k * 65536);    // per +1 row (y down)
  for (int py = 0; py < MAP_H; ++py) {
    uint16_t *row = canvasBuf + py * SCR_W;
    const float y = viewY - (py - MAP_H / 2.0f) * inv, x0 = viewX - (SCR_W / 2.0f) * inv;
    const float e0 = x0 * rotC - y * rotS, n0 = x0 * rotS + y * rotC;  // undo the clockwise turn
    int32_t uf = (int32_t)((half + e0 / mPerPx) * 65536), vf = (int32_t)((half - n0 / mPerPx) * 65536);
    (void)duY; (void)dvY;
    for (int px = 0; px < SCR_W; ++px, uf += duX, vf += dvX) {
      const int u = uf >> 16, v = vf >> 16;
      if (u < 0 || v < 0 || u >= BASE_PX || v >= BASE_PX) { row[px] = bg; continue; }
      const uint16_t p = basemap[v * BASE_PX + u];
      if (!p) { row[px] = bg; continue; }                            // outside the provider's circle
      const uint32_t r = (p >> 11) << 1, g = (p >> 5) & 0x3F, b = (p & 0x1F) << 1;
      row[px] = lut[(r * 77 + g * 150 + b * 29) >> 8];
    }
  }
}

static void drawMap() {
  const uint32_t t0 = millis();
  if (!canvasBuf) return;
  lv_canvas_fill_bg(canvas, lv_color_hex(C_BG), LV_OPA_COVER);
  if (!mapReady) { lv_obj_invalidate(canvas); return; }
  if (baseReady && !strcmp(baseCode, mapCode)) drawBasemap();
  const uint16_t apron = lv_color_to_u16(lv_color_hex(C_APRON)), term = lv_color_to_u16(lv_color_hex(C_TERMINAL));
  for (int i = 0; i < featCount; ++i) if (feats[i].type == F_APRON && feats[i].closed) fillPolygon(feats[i], apron);
  for (int i = 0; i < featCount; ++i) if (feats[i].type == F_TERMINAL && feats[i].closed) fillPolygon(feats[i], term);

  // lines in small batches: each segment is an LVGL draw task
  lv_layer_t layer;
  int batch = 0;
  lv_canvas_init_layer(canvas, &layer);
  auto flushBatch = [&](int add) {
    batch += add;
    if (batch > 120) { lv_canvas_finish_layer(canvas, &layer); lv_canvas_init_layer(canvas, &layer); batch = 0; }
  };
  for (int i = 0; i < featCount; ++i) if (feats[i].type == F_TERMINAL) { strokeFeature(&layer, feats[i], C_RUNWAY_EDGE, 1); flushBatch(feats[i].count); }
  for (int i = 0; i < featCount; ++i) if (feats[i].type == F_TAXIWAY) {
    const float wm = feats[i].widthM > 0 ? feats[i].widthM : 18;
    strokeFeature(&layer, feats[i], C_TAXI, max(2, (int)lroundf(wm * scale))); flushBatch(feats[i].count);
  }
  for (int i = 0; i < featCount; ++i) if (feats[i].type == F_RUNWAY) {
    const Feature &ft = feats[i];
    const float wm = ft.widthM > 0 ? ft.widthM : 45;
    const int w = max(4, (int)lroundf(wm * scale));
    if (ft.closed) fillPolygon(ft, lv_color_to_u16(lv_color_hex(C_RUNWAY)));          // runway mapped as area
    else { strokeFeature(&layer, ft, C_RUNWAY_EDGE, w + 2); strokeFeature(&layer, ft, C_RUNWAY, w); }
    flushBatch(ft.count * 2);
  }
  // parking positions / gates and holding points: small, discreet
  for (int i = 0; i < featCount; ++i) {
    const Feature &ft = feats[i];
    if (ft.type != F_PARKING && ft.type != F_HOLDING) continue;
    if (ft.count >= 2 && ft.type == F_PARKING) { strokeFeature(&layer, ft, C_GATE, 1, LV_OPA_50); flushBatch(ft.count); continue; }
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(ft.type == F_PARKING ? C_GATE : C_ALERT); r.bg_opa = ft.type == F_PARKING ? LV_OPA_70 : LV_OPA_50;
    r.radius = LV_RADIUS_CIRCLE;
    const int x = (int)sx(pts[ft.first * 2]), y = (int)sy(pts[ft.first * 2 + 1]);
    lv_area_t a = {x - 1, y - 1, x + 1, y + 1};
    lv_draw_rect(&layer, &r, &a); flushBatch(1);
  }
  // labels: runway designators, and gate refs only when zoomed in
  lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
  ld.font = &lv_font_weather_14; ld.color = lv_color_hex(C_TEXT2);
  char done[6][8] = {};                             // OSM splits runways in several ways: label each ref once
  int nDone = 0;
  for (int i = 0; i < featCount; ++i) {
    const Feature &ft = feats[i];
    if (!ft.ref[0]) continue;
    if (ft.type == F_RUNWAY) {
      bool seen = false;
      for (int k = 0; k < nDone; ++k) if (!strcmp(done[k], ft.ref)) seen = true;
      if (seen) continue;
      if (nDone < 6) snprintf(done[nDone++], 8, "%s", ft.ref);
      const int mid = ft.first + ft.count / 2;
      const int x = (int)sx(pts[mid * 2]), y = (int)sy(pts[mid * 2 + 1]);
      ld.text = ft.ref; ld.color = lv_color_hex(C_TEXT2);
      lv_area_t a = {x + 8, y - 8, x + 70, y + 10}; lv_draw_label(&layer, &ld, &a); flushBatch(4);
    } else if (ft.type == F_PARKING && scale > 0.45f) {
      const int x = (int)sx(pts[ft.first * 2]), y = (int)sy(pts[ft.first * 2 + 1]);
      ld.text = ft.ref; ld.color = lv_color_hex(0x4E6378);
      lv_area_t a = {x + 3, y - 7, x + 40, y + 9}; lv_draw_label(&layer, &ld, &a); flushBatch(4);
    }
  }
  lv_canvas_finish_layer(canvas, &layer);
  lv_obj_invalidate(canvas);
  mapDrawn = true;
  LogV.printf("Airport: mapa desenhado em %lums (escala %.2f px/m)\n", (unsigned long)(millis() - t0), scale);
}

static void fitView() {
  const float w = max(200.0f, maxX - minX), h = max(200.0f, maxY - minY);
  viewX = (minX + maxX) / 2; viewY = (minY + maxY) / 2;
  scale = min((SCR_W - 20) / w, (MAP_H - 20) / h) * 1.22f;   // a bit closer: runway ends may touch the edges
}

// ---------------------------------------------------------------- aircraft markers
// ---- same look as the Flight Radar: icon by type/category, colour by altitude
static bool typeIn(const char *type, const char *const *list, size_t n) {
  for (size_t i = 0; i < n; ++i) if (!strncmp(type, list[i], strlen(list[i]))) return true;
  return false;
}
static const lv_image_dsc_t *iconFor(const Aircraft &a) {
  static const char *const helis[] = {"R22", "R44", "R66", "EC", "AS3", "AS5", "B06", "B407", "B412",
                                      "S76", "S92", "A109", "A119", "A139", "H1", "H2"};
  static const char *const lights[] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "PA",
                                       "BE", "SR2", "TBM", "PC12", "P28", "DA4", "M20", "BL8"};
  if (!strcmp(a.category, "A7") || typeIn(a.type, helis, sizeof helis / sizeof *helis)) return &radar_icon_heli;
  const bool bigCategory = a.category[0] == 'A' && a.category[1] >= '3' && a.category[1] <= '6';
  if (!strcmp(a.category, "A1") || (!bigCategory && typeIn(a.type, lights, sizeof lights / sizeof *lights))) return &radar_icon_light;
  return &radar_icon_jet;
}
static uint32_t iconScale(const Aircraft &a) {
  static const char *const heavy[] = {"A30", "A31", "A33", "A34", "A35", "A38", "B74", "B76", "B77",
                                      "B78", "IL96", "MD11", "B2", "C5", "A124", "A400"};
  if (!strcmp(a.category, "A5") || typeIn(a.type, heavy, sizeof heavy / sizeof *heavy)) return 256;
  if (!strcmp(a.category, "A1") || !strcmp(a.category, "A7")) return 180;
  if (!strcmp(a.category, "A2")) return 196;
  return 216;
}
static uint32_t altitudeColor(const Aircraft &a) {
  if (a.emergency) return 0xFF5A5A;
  if (a.onGround) return 0xF2B66D;                 // ground: same amber as the radar
  if (!a.hasAltitude) return 0xB8C7C4;
  if (a.altitudeFeet < 2000) return 0x4ADE80;
  if (a.altitudeFeet < 10000) return 0x6DE8FF;
  if (a.altitudeFeet < 25000) return 0x7AA2FF;
  return 0xC79BFF;
}

// Between polls, moving aircraft are advanced along their track with their ground
// speed (max 6 s, only above 3 kt) so they glide instead of jumping. Never for memory.
static bool pinching = false;            // pinch-zoom in progress (canvasEvent)
// No prediction: the icon only ever shows positions the networks actually reported.
// A new report is reached with a slide lasting as long as the gap between reports, and an
// aircraft never goes backwards: a report behind the icon (repeated or older data from
// a slower network) is ignored. State is in map metres keyed by hex, so pan/zoom keeps it.
static const float GLIDE_S = 10.0f;     // default slide = poll interval (continuous motion)
struct Glide { bool on; char hex[8]; float fx, fy, tx, ty, hx, hy; uint32_t t0, durMs; };
static Glide glide[MAX_AC];
static lv_timer_t *glideTimer;
static void glidePos(const Glide &g, float &mx, float &my) {
  const float k = min(1.0f, lv_tick_elaps(g.t0) / (float)(g.durMs ? g.durMs : 1));
  mx = g.fx + (g.tx - g.fx) * k; my = g.fy + (g.ty - g.fy) * k;
}
static void glideCb(lv_timer_t *) {
  if (pinching || dragging) return;
  for (int i = 0; i < MAX_AC; ++i) {
    if (!glide[i].on || lv_obj_has_flag(planeImg[i], LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_tick_elaps(glide[i].t0) > glide[i].durMs + 150) continue;   // arrived
    float mx, my; glidePos(glide[i], mx, my);
    const int x = (int)sx(mx) - ICON_PX / 2, y = MAP_Y + (int)sy(my) - ICON_PX / 2;
    if (x != lv_obj_get_x(planeImg[i]) || y != lv_obj_get_y(planeImg[i])) lv_obj_set_pos(planeImg[i], x, y);
  }
}

static void placeMarker(int i, const Aircraft &a, bool selected, bool stale) {
  lv_obj_t *img = planeImg[i];
  const lv_image_dsc_t *icon = iconFor(a);
  if (lv_image_get_src(img) != icon) lv_image_set_src(img, icon);
  lv_obj_set_style_image_recolor(img, lv_color_hex(selected ? 0xFFD54A : altitudeColor(a)), 0);
  lv_obj_set_style_image_opa(img, stale ? LV_OPA_40 : LV_OPA_COVER, 0);
  lv_image_set_scale(img, iconScale(a) + (selected ? 40 : 0));
  // the map is turned by mapRotDeg, so the heading is too
  lv_image_set_rotation(img, (int32_t)lroundf(((a.hasHeading ? a.headingDegrees : 0) + mapRotDeg) * 10.0f));
  float mx, my; proj(a.latitude, a.longitude, mx, my);

  Glide prev = {}; bool havePrev = false;               // this aircraft's previous state (any slot)
  for (int k = 0; k < MAX_AC; ++k)
    if (glide[k].on && !strcmp(glide[k].hex, a.icao24)) { prev = glide[k]; havePrev = true; break; }

  Glide &g = glide[i];
  g.on = true;
  snprintf(g.hex, sizeof g.hex, "%s", a.icao24);
  if (a.hasHeading) {
    const float hr = (a.headingDegrees + mapRotDeg) * (float)M_PI / 180.0f;
    g.hx = sinf(hr); g.hy = cosf(hr);                    // unit heading in map metres (y up)
  } else { g.hx = g.hy = 0; }
  if (!havePrev) {
    g.fx = g.tx = mx; g.fy = g.ty = my; g.durMs = 1; g.t0 = lv_tick_get() - 1000;
  } else {
    float cx, cy; glidePos(prev, cx, cy);
    const float dx = mx - prev.tx, dy = my - prev.ty;
    const bool same = dx * dx + dy * dy < 4.0f;          // nothing new (< 2 m)
    const bool behind = (dx * prev.hx + dy * prev.hy) < 0 && dx * dx + dy * dy < 1500.0f * 1500.0f;
    if (same || (behind && !stale)) {                     // keep going to the old target
      g.fx = prev.fx; g.fy = prev.fy; g.tx = prev.tx; g.ty = prev.ty; g.t0 = prev.t0; g.durMs = prev.durMs;
    } else {                                              // real new report: slide there
      // take as long as the gap between reports, so it keeps moving until the next one
      // arrives instead of jumping and waiting (shown ~one poll behind, never backwards)
      const uint32_t gap = lv_tick_elaps(prev.t0);
      g.durMs = (uint32_t)constrain((float)gap, 3000.0f, 12000.0f);
      if (prev.durMs <= 1) g.durMs = (uint32_t)(GLIDE_S * 1000);
      g.fx = cx; g.fy = cy; g.tx = mx; g.ty = my; g.t0 = lv_tick_get();
    }
    glidePos(g, mx, my);
  }
  lv_obj_set_pos(img, (int)sx(mx) - ICON_PX / 2, MAP_Y + (int)sy(my) - ICON_PX / 2);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(marker[i], LV_OBJ_FLAG_HIDDEN);
}

// Aircraft outside the view (approach/departure): small chevron on the map edge
// pointing to where it is, dimmer than a real marker.
static void placeEdgeMarker(int i, float x, float y, bool selected, bool stale) {
  const float cx = SCR_W / 2.0f, cy = MAP_H / 2.0f, dx = x - cx, dy = y - cy;
  const float k = min(fabsf((SCR_W / 2.0f - 14) / (dx ? dx : 1e-3f)), fabsf((MAP_H / 2.0f - 14) / (dy ? dy : 1e-3f)));
  const float ex = cx + dx * k, ey = cy + dy * k, a = atan2f(dy, dx);
  static const float sh[4][2] = {{-4, -5}, {4, 0}, {-4, 5}, {-4, -5}};
  for (int j = 0; j < 4; ++j) {
    markerPts[i][j].x = (lv_value_precise_t)lroundf(10 + sh[j][0] * cosf(a) - sh[j][1] * sinf(a));
    markerPts[i][j].y = (lv_value_precise_t)lroundf(10 + sh[j][0] * sinf(a) + sh[j][1] * cosf(a));
  }
  lv_line_set_points(marker[i], markerPts[i], 4);
  lv_obj_set_style_line_color(marker[i], lv_color_hex(selected ? C_SELECTED : C_AIRCRAFT), 0);
  lv_obj_set_style_line_opa(marker[i], stale ? LV_OPA_30 : LV_OPA_60, 0);
  lv_obj_set_pos(marker[i], (int)ex - 10, MAP_Y + (int)ey - 10);
  lv_obj_remove_flag(marker[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(planeImg[i], LV_OBJ_FLAG_HIDDEN);
  glide[i].on = false; glide[i].hex[0] = 0;
}

// "N" arrow: the map is turned so the main runway is horizontal, so show where north is
static void updateNorthArrow() {
  const float a = -mapRotDeg * (float)M_PI / 180.0f;          // north on screen, measured from "up"
  const float ux = sinf(-a), uy = -cosf(-a);
  northPts[0].x = (lv_value_precise_t)lroundf(14 - ux * 9); northPts[0].y = (lv_value_precise_t)lroundf(14 - uy * 9);
  northPts[1].x = (lv_value_precise_t)lroundf(14 + ux * 9); northPts[1].y = (lv_value_precise_t)lroundf(14 + uy * 9);
  // arrow head
  const float px = -uy, py = ux;
  northPts[2].x = (lv_value_precise_t)lroundf(14 + ux * 3 + px * 4); northPts[2].y = (lv_value_precise_t)lroundf(14 + uy * 3 + py * 4);
  northPts[3] = northPts[1];
  northPts[4].x = (lv_value_precise_t)lroundf(14 + ux * 3 - px * 4); northPts[4].y = (lv_value_precise_t)lroundf(14 + uy * 3 - py * 4);
  lv_line_set_points(northArrow, northPts, 5);
  lv_obj_align(northLbl, LV_ALIGN_TOP_LEFT, 10 + (int)lroundf(14 + ux * 17) - 4, MAP_Y + MAP_H - 44 + (int)lroundf(14 + uy * 17) - 8);
}

static void fmtOr(char *b, size_t n, bool has, const char *fmt, float v) {
  if (has) snprintf(b, n, fmt, v); else snprintf(b, n, "\xE2\x80\x94");
}

static void renderInfo(const AirportTraffic &t) {
  int sel = -1;
  for (int i = 0; i < t.count; ++i) if (selectedHex[0] && !strcmp(t.ac[i].icao24, selectedHex)) sel = i;
  if (sel < 0) { lv_obj_add_flag(info, LV_OBJ_FLAG_HIDDEN); selectedHex[0] = 0; return; }
  const Aircraft &a = t.ac[sel];
  setLabelIfChanged(infoCall, a.callsign[0] ? a.callsign : (a.registration[0] ? a.registration : a.icao24));
  setLabelIfChanged(infoState, a.onGround ? "EM SOLO" : a.hasAltitude ? "NO AR" : "DESCONHECIDO");
  char spd[16], hdg[16], alt[20], age[16], b[96];
  fmtOr(spd, sizeof spd, a.hasSpeed, "%.0f kt", a.speedKnots);
  fmtOr(hdg, sizeof hdg, a.hasHeading, "%03.0f\xC2\xB0", a.headingDegrees);
  char code[5]; double la, lo; currentAirport(code, la, lo);
  const float agl = aglM(a, t.geomFt[sel], fieldElevM(code));
  if (a.onGround) snprintf(alt, sizeof alt, "solo");
  else if (!isnan(agl)) snprintf(alt, sizeof alt, "%.0f m acima", agl);
  else fmtOr(alt, sizeof alt, a.hasAltitude, "%.0f m", a.altitudeFeet * 0.3048f);
  const float ageS = (millis() - t.okMs) / 1000.0f + (a.hasPositionAge ? a.positionAgeSeconds : 0) + t.memoS[sel];
  snprintf(age, sizeof age, "%.0f s", ageS);
  if (t.memoS[sel]) setLabelIfChanged(infoState, "SEM SINAL (\xC3\xBAltima posi\xC3\xA7\xC3\xA3o)");
  snprintf(b, sizeof b, "VEL %s    RUMO %s    ALT %s", spd, hdg, alt); setLabelIfChanged(infoLine1, b);
  AptDetails d;
  xSemaphoreTake(mtx, portMAX_DELAY); d = det; xSemaphoreGive(mtx);
  char rb[120], mb[120];
  const bool mine = !strcmp(d.hex, a.icao24);
  if (!mine || d.state == DET_PENDING) { snprintf(rb, sizeof rb, "buscando rota e modelo..."); mb[0] = 0; }
  else if (d.state == DET_READY) {
    if (d.org[0] || d.dst[0]) snprintf(rb, sizeof rb, "%s %s  " LV_SYMBOL_RIGHT "  %s %s%s%s", d.org[0] ? d.org : "?", d.orgCity,
                                       d.dst[0] ? d.dst : "?", d.dstCity, d.airline[0] ? "  \xC2\xB7  " : "", d.airline);
    else snprintf(rb, sizeof rb, "rota n\xC3\xA3o informada%s%s", d.airline[0] ? "  \xC2\xB7  " : "", d.airline);
    snprintf(mb, sizeof mb, "%s%s%s%s%s", d.model[0] ? d.model : a.type, a.registration[0] ? "  \xC2\xB7  " : "", a.registration,
             d.owner[0] ? "  \xC2\xB7  " : "", d.owner);
  } else {
    snprintf(rb, sizeof rb, d.state == DET_FAILED ? "detalhes indispon\xC3\xADveis" : "rota n\xC3\xA3o encontrada");
    snprintf(mb, sizeof mb, "%s%s%s", a.type, a.registration[0] ? "  \xC2\xB7  " : "", a.registration);
  }
  setLabelIfChanged(infoRoute, rb); setLabelIfChanged(infoModel, mb);
  const uint8_t sm = t.src[sel];
  char srcs[40];
  snprintf(srcs, sizeof srcs, "%s%s%s%s", sm & SRC_ADSBLOL ? "ADSB.lol " : "", sm & SRC_AIRPLANES ? "airplanes.live " : "",
           sm & SRC_ADSBFI ? "adsb.fi " : "", sm & SRC_ADSBONE ? "adsb.one" : "");
  snprintf(b, sizeof b, "POSI\xC3\x87\xC3\x83O h\xC3\xA1 %s  \xC2\xB7  %s  \xC2\xB7  %s", age, a.icao24, srcs);
  setLabelIfChanged(infoLine2, b);
  lv_obj_remove_flag(info, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- touch: select / pan
static void recenter(lv_event_t *);
// pinch state (second finger comes from the touch driver: gTouchCount / gTouch2X / gTouch2Y)
static float pinchD0 = 1, pinchRatio = 1;
static lv_point_t pinchMid;
static uint32_t lastTapMs = 0;
static void hideAircraft(bool hide) {
  for (int i = 0; i < MAX_AC; ++i) if (hide) { lv_obj_add_flag(planeImg[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(marker[i], LV_OBJ_FLAG_HIDDEN); }
}
// No transformed preview (scaling the canvas layer broke when zooming out): the real
// map is redrawn at the new scale, at most every 200 ms while the fingers move.
static constexpr float MIN_SCALE = 0.09f, MAX_SCALE = 2.5f;   // min ~ keeps the view inside the street map
static float pinchScale0 = 0.1f, pinchViewX0, pinchViewY0;
static uint32_t pinchDrawMs = 0;
static void applyPinch() {
  // keep the world point under the fingers' midpoint fixed while changing the scale
  const float newScale = constrain(pinchScale0 * pinchRatio, MIN_SCALE, MAX_SCALE);
  const float mx = pinchMid.x - SCR_W / 2.0f, my = (pinchMid.y - MAP_Y) - MAP_H / 2.0f;
  viewX = pinchViewX0 + mx / pinchScale0 - mx / newScale;
  viewY = pinchViewY0 - (my / pinchScale0 - my / newScale);
  scale = newScale;
  drawMap();
  renderedTrafficVersion = UINT32_MAX;                            // aircraft follow every pinch step
  airportRender();
  pinchDrawMs = lv_tick_get();
}
static void endPinch() {
  applyPinch();
  pinching = false; pinchRatio = 1;
  renderedTrafficVersion = UINT32_MAX;
  airportRender();
}

static void canvasEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  // Registered with LV_EVENT_ALL: ignore drawing/style events. Handling them re-entered
  // this function from inside LVGL (infinite recursion -> stack overflow while pinching).
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
  static bool busy = false;                                       // never re-enter
  if (busy) return;
  struct Guard { Guard() { busy = true; } ~Guard() { busy = false; } } guard;
  lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
  if ((code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) && gTouchCount >= 2) {
    const float dx = gTouch2X - p.x, dy = gTouch2Y - p.y, d = sqrtf(dx * dx + dy * dy);
    if (!pinching) {                                              // start: remember distance + midpoint
      if (dragging) { viewX = dragViewX; viewY = dragViewY; }    // a pan had started: undo it
      pinching = true; dragging = false; pinchD0 = max(20.0f, d); pinchRatio = 1;
      pinchMid = {(int32_t)((p.x + gTouch2X) / 2), (int32_t)((p.y + gTouch2Y) / 2)};
      pinchScale0 = scale; pinchViewX0 = viewX; pinchViewY0 = viewY;
      lv_obj_set_pos(canvas, 0, MAP_Y);
      // aircraft stay visible: they are re-placed together with every redraw (applyPinch)
    }
    pinchRatio = constrain(d / pinchD0, 0.25f, 4.0f);
    if (lv_tick_elaps(pinchDrawMs) > 200) applyPinch();                    // live, throttled
    return;
  }
  if (pinching && (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || gTouchCount < 2)) {
    if (code == LV_EVENT_PRESSING && gTouchCount == 1) return;    // wait for both fingers to lift
    endPinch();
    return;
  }
  if (pinching) return;
  if (code == LV_EVENT_PRESSED) {
    dragStart = p; dragViewX = viewX; dragViewY = viewY; dragging = false;
    for (int i = 0; i < MAX_AC; ++i) {
      dragBase[0][i] = {lv_obj_get_x(planeImg[i]), lv_obj_get_y(planeImg[i])};
      dragBase[1][i] = {lv_obj_get_x(marker[i]), lv_obj_get_y(marker[i])};
    }
    return;
  }
  if (code == LV_EVENT_PRESSING) {
    const int dx = p.x - dragStart.x, dy = p.y - dragStart.y;
    if (!dragging && abs(dx) + abs(dy) > 10) dragging = true;
    if (dragging) {                                               // instant feedback, re-render on release
      lv_obj_set_pos(canvas, dx, MAP_Y + dy);
      for (int i = 0; i < MAX_AC; ++i) {                          // aircraft move with the map
        if (!lv_obj_has_flag(planeImg[i], LV_OBJ_FLAG_HIDDEN)) lv_obj_set_pos(planeImg[i], dragBase[0][i].x + dx, dragBase[0][i].y + dy);
        if (!lv_obj_has_flag(marker[i], LV_OBJ_FLAG_HIDDEN)) lv_obj_set_pos(marker[i], dragBase[1][i].x + dx, dragBase[1][i].y + dy);
      }
    }
    return;
  }
  if (code == LV_EVENT_RELEASED && dragging) {
    viewX = dragViewX - (p.x - dragStart.x) / scale; viewY = dragViewY + (p.y - dragStart.y) / scale;
    lv_obj_set_pos(canvas, 0, MAP_Y);
    drawMap(); renderedTrafficVersion = UINT32_MAX; dragging = false;
    airportRender();                                              // re-anchor aircraft in the same frame
    return;
  }
  if (code == LV_EVENT_SHORT_CLICKED) {                            // nearest aircraft within 22 px
    if (!traffic) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    int best = -1; float bestD = 22 * 22;
    for (int i = 0; i < traffic->count; ++i) {
      float mx, my; proj(traffic->ac[i].latitude, traffic->ac[i].longitude, mx, my);
      const float x = sx(mx), y = MAP_Y + sy(my);
      const float d = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y);
      if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0 && lv_tick_elaps(lastTapMs) < 400) {                // double tap on the map: recenter
      xSemaphoreGive(mtx); lastTapMs = 0; recenter(nullptr); return;
    }
    lastTapMs = lv_tick_get();
    if (best >= 0) snprintf(selectedHex, sizeof selectedHex, "%s", traffic->ac[best].icao24); else selectedHex[0] = 0;
    const bool newSel = best >= 0 && strcmp(det.hex, selectedHex);
    if (newSel) {
      memset(&det, 0, sizeof det);
      snprintf(det.hex, sizeof det.hex, "%s", selectedHex);
      snprintf(det.cs, sizeof det.cs, "%s", traffic->ac[best].callsign);
      det.state = DET_PENDING;
    }
    xSemaphoreGive(mtx);
    if (newSel) postScreenJob(CMD_AIRPORT);                       // fetch details right away
    renderedTrafficVersion = UINT32_MAX;
  }
}

void airportRender();
static void zoomBy(float f) {
  scale = constrain(scale * f, MIN_SCALE, MAX_SCALE);
  drawMap(); renderedTrafficVersion = UINT32_MAX;
  airportRender();                                                // aircraft follow the zoom at once
}
static void zoomIn(lv_event_t *) { zoomBy(1.5f); }
static void zoomOut(lv_event_t *) { zoomBy(1 / 1.5f); }
static void recenter(lv_event_t *) { fitView(); drawMap(); renderedTrafficVersion = UINT32_MAX; airportRender(); }

static lv_obj_t *roundBtn(lv_obj_t *p, const char *sym, lv_event_cb_t cb, int y) {
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, 36, 36);
  lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, y);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BG2), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(C_RUNWAY_EDGE), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *l = makeLabel(b, sym, &lv_font_weather_20, C_TEXT); lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

// ---------------------------------------------------------------- build
void buildAirportScreen(lv_obj_t *scr) {
  airportView = lv_obj_create(scr);
  lv_obj_set_size(airportView, SCR_W, SCR_H); lv_obj_set_pos(airportView, 0, 0);
  lv_obj_set_style_radius(airportView, 0, 0);
  lv_obj_set_style_border_width(airportView, 0, 0); lv_obj_set_style_pad_all(airportView, 0, 0);
  lv_obj_set_style_bg_color(airportView, lv_color_hex(C_BG), 0); lv_obj_set_style_bg_opa(airportView, LV_OPA_COVER, 0);
  lv_obj_remove_flag(airportView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(airportView, LV_OBJ_FLAG_HIDDEN);

  canvas = lv_canvas_create(airportView);
  canvasBuf = (uint16_t *)psramMalloc(SCR_W * MAP_H * 2);
  if (canvasBuf) lv_canvas_set_buffer(canvas, canvasBuf, SCR_W, MAP_H, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas, 0, MAP_Y);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, canvasEvent, LV_EVENT_ALL, NULL);
  if (canvasBuf) lv_canvas_fill_bg(canvas, lv_color_hex(C_BG), LV_OPA_COVER);

  for (int i = 0; i < MAX_AC; ++i) {
    marker[i] = lv_line_create(airportView);
    lv_obj_set_size(marker[i], 20, 20);
    lv_obj_set_style_line_width(marker[i], 3, 0);
    lv_obj_set_style_line_rounded(marker[i], true, 0);
    lv_obj_remove_flag(marker[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(marker[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = 0; i < MAX_AC; ++i) {
    lv_obj_t *img = planeImg[i] = lv_image_create(airportView);
    lv_image_set_src(img, &radar_icon_jet);
    lv_obj_set_size(img, ICON_PX, ICON_PX);
    lv_image_set_pivot(img, ICON_PX / 2, ICON_PX / 2);
    lv_image_set_antialias(img, true);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);       // taps go to the map (nearest aircraft)
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
  }
  northArrow = lv_line_create(airportView);
  lv_obj_set_size(northArrow, 28, 28); lv_obj_set_pos(northArrow, 10, MAP_Y + MAP_H - 44);
  lv_obj_set_style_line_width(northArrow, 2, 0); lv_obj_set_style_line_rounded(northArrow, true, 0);
  lv_obj_set_style_line_color(northArrow, lv_color_hex(C_TEXT2), 0);
  lv_obj_remove_flag(northArrow, LV_OBJ_FLAG_CLICKABLE);
  northLbl = makeLabel(airportView, "N", &lv_font_weather_14, C_TEXT2);
  msgLbl = makeLabel(airportView, "", &lv_font_weather_14, C_TEXT2);
  lv_obj_align(msgLbl, LV_ALIGN_CENTER, 0, 0);

  // header
  lv_obj_t *hdr = lv_obj_create(airportView);
  lv_obj_set_size(hdr, SCR_W, MAP_Y); lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0); lv_obj_set_style_border_width(hdr, 0, 0); lv_obj_set_style_pad_all(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_BG2), 0); lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
  addBackButton(airportView);                           // standard back button (main.cpp)
  title = makeLabel(airportView, "Aeroporto", &lv_font_weather_14, C_TEXT);
  lv_obj_set_pos(title, 50, 9);
  statusDot = lv_obj_create(airportView);
  lv_obj_set_size(statusDot, 8, 8); lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(statusDot, 0, 0); lv_obj_set_style_bg_color(statusDot, lv_color_hex(C_TEXT2), 0);
  lv_obj_remove_flag(statusDot, LV_OBJ_FLAG_CLICKABLE);
  statusLbl = makeLabel(airportView, "conectando", &lv_font_weather_14, C_TEXT2);
  lv_obj_set_width(statusLbl, 150); lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(statusLbl, SCR_W - 160, 9);
  lv_obj_set_pos(statusDot, SCR_W - 170, 14);

  // footer
  lv_obj_t *ftr = lv_obj_create(airportView);
  lv_obj_set_size(ftr, SCR_W, SCR_H - MAP_Y - MAP_H); lv_obj_set_pos(ftr, 0, MAP_Y + MAP_H);
  lv_obj_set_style_radius(ftr, 0, 0); lv_obj_set_style_border_width(ftr, 0, 0); lv_obj_set_style_pad_all(ftr, 0, 0);
  lv_obj_set_style_bg_color(ftr, lv_color_hex(C_BG2), 0); lv_obj_remove_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(ftr, LV_OBJ_FLAG_CLICKABLE);
  footLbl = makeLabel(ftr, "", &lv_font_weather_14, C_TEXT);      // kept for messages only
  lv_obj_add_flag(footLbl, LV_OBJ_FLAG_HIDDEN);
  // three clear stats: big number + small caption, separated by thin lines
  static const char *caps[3] = {"em solo", "at\xC3\xA9 200 m", "na regi\xC3\xA3o"};
  static const uint32_t col[3] = {0xF2B66D, 0x4ADE80, C_TEXT};      // same colours as the aircraft icons
  int x = 12;
  for (int k = 0; k < 3; ++k) {
    statNum[k] = makeLabel(ftr, "0", &lv_font_weather_20, col[k]);
    lv_obj_set_pos(statNum[k], x, 2);
    lv_obj_t *c = makeLabel(ftr, caps[k], &lv_font_weather_14, C_TEXT2);
    lv_obj_set_pos(c, x + 22, 7);
    lv_obj_update_layout(c);                                       // measure the caption
    x += 22 + lv_obj_get_width(c) + 18;
    if (k < 2) {
      lv_obj_t *sep = lv_obj_create(ftr);
      lv_obj_set_size(sep, 1, 16); lv_obj_set_pos(sep, x - 10, 7);
      lv_obj_set_style_bg_color(sep, lv_color_hex(C_RUNWAY_EDGE), 0); lv_obj_set_style_border_width(sep, 0, 0);
      lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    }
  }
  srcLbl = makeLabel(ftr, "", &lv_font_weather_14, 0x3C5266);
  lv_obj_align(srcLbl, LV_ALIGN_TOP_RIGHT, -10, 7);
  // ODbL attribution: bottom-right corner of the map (the footer is full of stats)
  attribLbl = makeLabel(airportView, "\xC2\xA9 OpenStreetMap contributors", &lv_font_weather_14, 0x2E4257);
  lv_obj_align(attribLbl, LV_ALIGN_TOP_RIGHT, -8, MAP_Y + MAP_H - 20);

  // no zoom buttons: pinch to zoom, drag to pan, double tap to recenter
  (void)zoomIn; (void)zoomOut; (void)roundBtn;

  // aircraft card (bottom of the map, only when selected)
  info = lv_obj_create(airportView);
  lv_obj_set_size(info, SCR_W - 16, 108); lv_obj_set_pos(info, 8, MAP_Y + MAP_H - 114);
  lv_obj_set_style_radius(info, 12, 0);
  lv_obj_set_style_bg_color(info, lv_color_hex(C_BG2), 0); lv_obj_set_style_bg_opa(info, LV_OPA_90, 0);
  lv_obj_set_style_border_width(info, 1, 0); lv_obj_set_style_border_color(info, lv_color_hex(C_RUNWAY_EDGE), 0);
  lv_obj_set_style_pad_all(info, 0, 0); lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(info, LV_OBJ_FLAG_CLICKABLE);
  infoCall = makeLabel(info, "", &lv_font_weather_20, C_SELECTED); lv_obj_set_pos(infoCall, 12, 6);
  infoState = makeLabel(info, "", &lv_font_weather_14, C_TEXT); lv_obj_align(infoState, LV_ALIGN_TOP_RIGHT, -12, 10);
  infoRoute = makeLabel(info, "", &lv_font_weather_14, C_TEXT); lv_obj_set_pos(infoRoute, 12, 32);
  lv_obj_set_width(infoRoute, SCR_W - 40); lv_label_set_long_mode(infoRoute, LV_LABEL_LONG_DOT);
  infoModel = makeLabel(info, "", &lv_font_weather_14, C_TEXT2); lv_obj_set_pos(infoModel, 12, 50);
  lv_obj_set_width(infoModel, SCR_W - 40); lv_label_set_long_mode(infoModel, LV_LABEL_LONG_DOT);
  infoLine1 = makeLabel(info, "", &lv_font_weather_14, C_TEXT); lv_obj_set_pos(infoLine1, 12, 68);
  infoLine2 = makeLabel(info, "", &lv_font_weather_14, C_TEXT2); lv_obj_set_pos(infoLine2, 12, 86);
  lv_obj_set_width(infoLine2, SCR_W - 40); lv_label_set_long_mode(infoLine2, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(info, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- lifecycle (UI thread)
void airportOnEnter() {
  if (!glideTimer) glideTimer = lv_timer_create(glideCb, 100, NULL);   // 10 fps, only small icons move
  lv_timer_resume(glideTimer);
  mapDrawn = false;
  renderedTrafficVersion = UINT32_MAX;
  selectedHex[0] = 0;
  lv_obj_add_flag(info, LV_OBJ_FLAG_HIDDEN);
}

void airportOnExit() {
  dragging = false; pinching = false;
  lv_obj_set_pos(canvas, 0, MAP_Y);
  if (glideTimer) lv_timer_pause(glideTimer);
}

void airportRender() {
  char code[5]; double lat, lon;
  currentAirport(code, lat, lon);
  char iata[5], icao[5], b[96];
  codePair(code, iata, icao);
  snprintf(b, sizeof b, "Aeroporto   %s / %s", iata, icao);
  setLabelIfChanged(title, b);

  const bool ready = mapReady && !strcmp(mapCode, code);
  static bool wasReady = false;
  if (ready && (!mapDrawn || !wasReady)) { fitView(); drawMap(); updateNorthArrow(); renderedTrafficVersion = UINT32_MAX; }
  static uint32_t drawnBase = 0;
  if (ready && mapDrawn && drawnBase != baseVersion) { drawnBase = baseVersion; drawMap(); renderedTrafficVersion = UINT32_MAX; }
  wasReady = ready;
  if (!ready) {
    setLabelIfChanged(msgLbl, mapMessage[0] ? mapMessage : "Carregando mapa do OpenStreetMap...");
    lv_obj_remove_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);
  } else if (!featCount) {
    setLabelIfChanged(msgLbl, "Mapa sem elementos no OpenStreetMap");
    lv_obj_remove_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(msgLbl, LV_OBJ_FLAG_HIDDEN);

  if (!traffic) return;
  static AirportTraffic t;
  xSemaphoreTake(mtx, portMAX_DELAY);
  const uint32_t ver = traffic->version;
  const bool changed = ver != renderedTrafficVersion;
  if (changed) t = *traffic;
  else { t.okMs = traffic->okMs; t.failed = traffic->failed; t.sourcesOk = traffic->sourcesOk; }
  xSemaphoreGive(mtx);

  // header status + footer (age refreshed every render, ~0.5 s)
  const uint32_t ageS = t.okMs ? (millis() - t.okMs) / 1000 : 0;
  const bool stale = t.okMs && ageS > 90;
  uint32_t dot = C_TEXT2;
  if (!t.okMs) snprintf(b, sizeof b, t.failed ? "DADOS INDISPON\xC3\x8DVEIS" : "conectando");
  else if (stale) { snprintf(b, sizeof b, "ANTIGO  h\xC3\xA1 %lus", (unsigned long)ageS); dot = C_ALERT; }
  else { snprintf(b, sizeof b, t.failed ? "falha, h\xC3\xA1 %lus" : "ao vivo  %lus", (unsigned long)ageS); dot = t.failed ? C_ALERT : 0x3FA56E; }
  setLabelIfChanged(statusLbl, b);
  lv_obj_set_style_bg_color(statusDot, lv_color_hex(dot), 0);
  int ground = 0, low = 0;
  { const float elev = fieldElevM(code);
    for (int i = 0; i < t.count; ++i) { if (t.memoS[i]) continue; ground += t.ac[i].onGround; const float h = aglM(t.ac[i], t.geomFt[i], elev); low += !t.ac[i].onGround && h <= SHOW_MAX_AGL_M; } }
  snprintf(b, sizeof b, "%d", ground); setLabelIfChanged(statNum[0], b);
  snprintf(b, sizeof b, "%d", low);    setLabelIfChanged(statNum[1], b);
  snprintf(b, sizeof b, "%u", t.count); setLabelIfChanged(statNum[2], b);
  { const int ns = !!(t.sourcesOk & SRC_ADSBLOL) + !!(t.sourcesOk & SRC_AIRPLANES) + !!(t.sourcesOk & SRC_ADSBFI) + !!(t.sourcesOk & SRC_ADSBONE);
    snprintf(b, sizeof b, "%d fonte%s", ns, ns == 1 ? "" : "s"); }
  setLabelIfChanged(srcLbl, b);

  if (!changed && !(stale)) { if (selectedHex[0]) renderInfo(t); return; }
  renderedTrafficVersion = ver;
  int shown = 0;
  static char keepHex[MAX_AC][8]; static uint8_t keepN = 0;   // on screen last draw
  char nowHex[MAX_AC][8]; uint8_t nowN = 0;
  if (ready) {
    const float elev = fieldElevM(code);
    // Hysteresis: an aircraft enters at <= 200 m AGL (negative included), but once on screen it stays until it
    // is clearly gone (> 300 m AGL). Altitude jitter between networks (GNSS vs barometric,
    // +/- tens of m) made aircraft on final blink out before touchdown.
    for (int i = 0; i < t.count && shown < MAX_AC; ++i) {
      const float agl = aglM(t.ac[i], t.geomFt[i], elev);
      bool kept = false;
      for (int k = 0; k < keepN; ++k) if (!strcmp(keepHex[k], t.ac[i].icao24)) { kept = true; break; }
      const bool show = t.ac[i].onGround || agl <= SHOW_MAX_AGL_M ||
                        (kept && (isnan(agl) || agl <= SHOW_KEEP_AGL_M));
      if (!show) {                                    // higher traffic: Flight Radar
        if (kept) LogV.printf("Airport: %s saiu (%.0f m AGL)\n", t.ac[i].icao24, agl);
        continue;
      }
      snprintf(nowHex[nowN++], 8, "%s", t.ac[i].icao24);
      float mx, my; proj(t.ac[i].latitude, t.ac[i].longitude, mx, my);
      const float x = sx(mx), y = sy(my);
      const bool sel = selectedHex[0] && !strcmp(selectedHex, t.ac[i].icao24);
      const bool outside = x < 6 || x > SCR_W - 6 || y < 6 || y > MAP_H - 6;
      if (outside) placeEdgeMarker(shown++, x, y, sel, stale || t.memoS[i]);   // outside: chevron on the edge
      else placeMarker(shown++, t.ac[i], sel, stale || t.memoS[i]);
    }
  }
  if (ready) {                                    // log aircraft that vanished from the data
    for (int k = 0; k < keepN; ++k) {
      bool still = false, inData = false;
      for (int j = 0; j < nowN; ++j) if (!strcmp(nowHex[j], keepHex[k])) { still = true; break; }
      for (int j = 0; j < t.count; ++j) if (!strcmp(t.ac[j].icao24, keepHex[k])) { inData = true; break; }
      if (!still && !inData) LogV.printf("Airport: %s sumiu dos dados (nenhuma rede informou)\n", keepHex[k]);
    }
    memcpy(keepHex, nowHex, sizeof nowHex[0] * nowN); keepN = nowN;
  }
  for (int i = shown; i < MAX_AC; ++i) { lv_obj_add_flag(marker[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(planeImg[i], LV_OBJ_FLAG_HIDDEN); }
  lv_obj_move_foreground(info);
  renderInfo(t);
}
