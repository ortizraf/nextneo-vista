// Airport data side (network task): OpenStreetMap airfield map (Overpass, cached on
// LittleFS), traffic from several ADS-B networks, details of the tapped aircraft
// (adsbdb), the Aena board (CGH) and the Flight Radar overlay.
#include "../../app_screens.h"
#include "modules/flight_airport/flight_airport.h"
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

#include "modules/flight_airport/airport_internal.h"

namespace airport {

// ---------------------------------------------------------------- map model (PSRAM)
Feature *feats;
int16_t *pts;                 // x,y pairs, local metres (east, north) around the airport centre
int featCount = 0, ptCount = 0;
char mapCode[5] = "";         // airport the map belongs to
double mapLat = 0, mapLon = 0;
float minX, maxX, minY, maxY;
volatile bool mapReady = false;
static uint32_t mapRetryAtMs = 0;
// Street basemap around the airport (same Esri source as the Flight Radar), north-up,
// re-tinted to the dark navy palette and sampled rotated under the vector map.
uint16_t *basemap;                                 // BASE_PX x BASE_PX RGB565 (PSRAM)
char baseCode[5] = "";
volatile bool baseReady = false;
volatile uint32_t baseVersion = 0;
static uint32_t baseRetryAtMs = 0;
char mapMessage[48] = "";

// ---------------------------------------------------------------- traffic
static uint8_t *fetchSrc;
static float *fetchGeom;                   // GNSS altitude (alt_geom, ft) or NAN: independent of QNH
static Aircraft *secBuf; static uint8_t *secSrc; static float *secGeom;   // cache of the secondary networks
static WiFiClientSecure lolClient;         // keep-alive session to ADSB.lol while the screen is open

// Only aircraft on the ground or up to this height above the field are drawn
// (touchdown / lift-off / runway); higher traffic belongs to the Flight Radar.
struct FieldElev { const char *code; float m; };
static const FieldElev kElev[] = {
  {"CGH",802},{"SBSP",802},{"GRU",750},{"SBGR",750},{"VCP",661},{"SBKP",661},{"SDU",3},{"SBRJ",3},{"GIG",9},{"SBGL",9},
  {"BSB",1066},{"SBBR",1066},{"CNF",827},{"SBCF",827},{"CWB",908},{"SBCT",908},{"POA",3},{"SBPA",3},{"FLN",5},{"SBFL",5},
  {"REC",10},{"SBRF",10},{"SSA",20},{"SBSV",20},{"FOR",25},{"SBFZ",25},{"BEL",16},{"SBBE",16},{"MAO",80},{"SBEG",80},
  {"NAT",83},{"SBSG",83},{"MCZ",118},{"SBMO",118},{"JPA",66},{"SBJP",66},{"IGU",240},{"SBFI",240},{"GYN",747},{"SBGO",747}};
float fieldElevM(const char *code) { for (auto &e : kElev) if (!strcmp(code, e.code)) return e.m; return NAN; }
// height above the field in metres (0 on the ground, NAN unknown)
float aglM(const Aircraft &a, float geomFt, float elevM) {
  if (a.onGround) return 0;
  if (isnan(elevM)) return NAN;
  if (!isnan(geomFt)) return geomFt * 0.3048f - elevM;           // GNSS: best
  if (a.hasAltitude) return a.altitudeFeet * 0.3048f - elevM;     // barometric (std 1013): +/- tens of m
  return NAN;
}
AirportTraffic *traffic;        // written by the network task under mtx
static Aircraft *fetchBuf;


// ---------------------------------------------------------------- helpers
struct AirportCodes { const char *iata, *icao; };
static const AirportCodes kCodes[] = {
  {"CGH","SBSP"},{"GRU","SBGR"},{"VCP","SBKP"},{"SDU","SBRJ"},{"GIG","SBGL"},{"BSB","SBBR"},{"CNF","SBCF"},
  {"CWB","SBCT"},{"POA","SBPA"},{"FLN","SBFL"},{"REC","SBRF"},{"SSA","SBSV"},{"FOR","SBFZ"},{"BEL","SBBE"},
  {"MAO","SBEG"},{"NAT","SBSG"},{"MCZ","SBMO"},{"JPA","SBJP"},{"IGU","SBFI"},{"GYN","SBGO"}};
void codePair(const char *code, char *iata, char *icao) {
  snprintf(iata, 5, "%s", code); snprintf(icao, 5, "%s", code);
  for (auto &c : kCodes) {
    if (!strcmp(code, c.iata)) { snprintf(icao, 5, "%s", c.icao); return; }
    if (!strcmp(code, c.icao)) { snprintf(iata, 5, "%s", c.iata); return; }
  }
}
// Map orientation: the whole picture (map + aircraft + headings) is turned clockwise
// on screen by mapRotDeg so that the longest runway lies horizontal (landscape screen).
float mapRotDeg = 0, rotC = 1, rotS = 0;
void setRotation(float deg) { mapRotDeg = deg; rotC = cosf(deg * (float)M_PI / 180); rotS = sinf(deg * (float)M_PI / 180); }
void currentAirport(char *code, double &lat, double &lon) {
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
  // Short timeouts: a slow/hung network must not freeze the live view (was 10 s + 8 s).
  if (!keep) { fresh.setInsecure(); fresh.setHandshakeTimeout(5); }    // public read-only data
  else if (!client.connected()) client.stop();          // server closed it: free the old context first
  const bool reused = keep && client.connected();            // diagnostic: did keep-alive work?
  const uint32_t t0 = millis();
  const int status = httpsFetch(client, url, body, 4500, keep != nullptr);
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

// ---------------------------------------------------------------- Flight Radar overlay
// The radar draws this airport (runways, taxiways, aprons) north-up INTO its map
// image, once per airport/radius change, from the network task. Nothing is added
// to LVGL, so it costs nothing per frame. Geometry comes from the same sources as
// this screen: built-in (CGH) -> flash cache -> Overpass (once, then cached).
// Network task only; the Airport screen is not active while the radar is.
}  // namespace airport
using namespace airport;

bool airportEnsureRadarGeometry(const char *code, double lat, double lon, bool allowDownload) {
  if (mapReady && !strcmp(mapCode, code) && fabs(mapLat - lat) < 1e-6 && fabs(mapLon - lon) < 1e-6) return true;
  if ((int32_t)(millis() - mapRetryAtMs) < 0) return false;
  if (!feats) {
    feats = (Feature *)psramCalloc(MAX_FEATURES, sizeof(Feature));
    pts = (int16_t *)psramMalloc(MAX_POINTS * 2 * sizeof(int16_t));
    if (!feats || !pts) return false;
  }
  if (loadBuiltinMap(code, lat, lon) || loadMapFile(code, lat, lon)) return true;
  if (allowDownload && fetchMap(code, lat, lon)) {
    Log.printf("Radar: mapa do aeroporto %s baixado (%d elementos)\n", code, featCount);
    return true;
  }
  mapRetryAtMs = millis() + 60000;
  return false;
}

namespace airport {

namespace {
struct OverlayRaster { uint16_t *buf; int size; float c, r2; };
inline void ovPlot(const OverlayRaster &R, int x, int y, uint16_t col) {
  if (x < 0 || y < 0 || x >= R.size || y >= R.size) return;
  const float dx = x + 0.5f - R.c, dy = y + 0.5f - R.c;
  if (dx * dx + dy * dy > R.r2) return;                 // stay inside the round scope
  R.buf[y * R.size + x] = col;
}
// Anti-aliased thick segment: per-pixel coverage from the distance to the segment,
// blended over what is already in the buffer (map or plain scope).
inline uint16_t ovBlend(uint16_t dst, uint16_t src, float a) {
  const int ia = (int)(a * 256 + 0.5f);
  const int r = (((src >> 11) * ia) + ((dst >> 11) * (256 - ia))) >> 8;
  const int g = ((((src >> 5) & 0x3F) * ia) + (((dst >> 5) & 0x3F) * (256 - ia))) >> 8;
  const int b = (((src & 0x1F) * ia) + ((dst & 0x1F) * (256 - ia))) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
inline void ovLineAA(const OverlayRaster &R, float x0, float y0, float x1, float y1, float widthPx, uint16_t col, float alpha = 1.0f) {
  const float hw = widthPx / 2;
  const int xa = max(0, (int)floorf(min(x0, x1) - hw - 1)), xb = min(R.size - 1, (int)ceilf(max(x0, x1) + hw + 1));
  const int ya = max(0, (int)floorf(min(y0, y1) - hw - 1)), yb = min(R.size - 1, (int)ceilf(max(y0, y1) + hw + 1));
  const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
  for (int y = ya; y <= yb; ++y)
    for (int x = xa; x <= xb; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float cx = px - R.c, cy = py - R.c;
      if (cx * cx + cy * cy > R.r2) continue;
      float t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
      t = t < 0 ? 0 : (t > 1 ? 1 : t);
      const float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
      const float cov = hw + 0.5f - sqrtf(ex * ex + ey * ey);     // 1 px soft edge
      if (cov <= 0) continue;
      uint16_t &d = R.buf[y * R.size + x];
      d = ovBlend(d, col, (cov > 1 ? 1 : cov) * alpha);
    }
}
constexpr uint16_t ov565(uint32_t c) {
  return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}
}  // namespace

// radiusKm = radar range; buf = size x size RGB565, centre = airport reference point.
}  // namespace airport
using namespace airport;

void airportDrawRadarOverlay(float radiusKm, uint16_t *buf, int size) {
  if (!mapReady || !buf || radiusKm <= 0) return;
  const uint32_t t0 = millis();
  const OverlayRaster R = {buf, size, size / 2.0f, (size / 2.0f - 1) * (size / 2.0f - 1)};
  const float pxPerM = (size / 2.0f) / (radiusKm * 1000.0f);
  // This screen projects with 111320/110540 m per degree; the radar uses a 6371 km
  // sphere (111195 m). Scale each axis so runways sit exactly under the aircraft.
  const float kE = 111194.9f / 111320.0f, kN = 111194.9f / 110540.0f;
  auto toPx = [&](uint32_t k, float &x, float &y) {
    const float rx = pts[k * 2], ry = pts[k * 2 + 1];
    const float e = rx * rotC - ry * rotS, n = rx * rotS + ry * rotC;   // undo the "runway horizontal" turn
    x = R.c + e * kE * pxPerM;
    y = R.c - n * kN * pxPerM;
  };
  const bool detail = radiusKm <= 5.5f, medium = radiusKm <= 10.5f;
  const uint16_t cApron = ov565(0x163F42), cTerm = ov565(0x1F5457), cTaxi = ov565(0x4FA095), cRunway = ov565(0xD8F5EE);

  // 1) aprons / terminals: scanline fill (even-odd), only when they are big enough to read
  if (medium) {
    static float xs[64];
    for (int i = 0; i < featCount; ++i) {
      const Feature &ft = feats[i];
      if (!ft.closed || ft.count < 3 || !(ft.type == F_APRON || (detail && ft.type == F_TERMINAL))) continue;
      const uint16_t col = ft.type == F_APRON ? cApron : cTerm;
      float minY = 1e9f, maxY = -1e9f;
      for (uint32_t k = ft.first; k < ft.first + ft.count; ++k) { float x, y; toPx(k, x, y); minY = min(minY, y); maxY = max(maxY, y); }
      const int ya = max(0, (int)floorf(minY)), yb = min(size - 1, (int)ceilf(maxY));
      for (int py = ya; py <= yb; ++py) {
        const float yc = py + 0.5f;
        int nx = 0;
        float px0, py0; toPx(ft.first + ft.count - 1, px0, py0);
        for (uint32_t k = ft.first; k < ft.first + ft.count && nx < 64; ++k) {
          float px1, py1; toPx(k, px1, py1);
          if ((py0 <= yc) != (py1 <= yc)) xs[nx++] = px0 + (yc - py0) * (px1 - px0) / (py1 - py0);
          px0 = px1; py0 = py1;
        }
        std::sort(xs, xs + nx);
        for (int j = 0; j + 1 < nx; j += 2)
          for (int px = (int)ceilf(xs[j] - 0.5f); px <= (int)floorf(xs[j + 1] - 0.5f); ++px) ovPlot(R, px, py, col);
      }
    }
  }
  // 2) taxiways, then 3) runways on top. Runways: dark halo first (separates parallel
  //    runways and lifts them off the street map), then an anti-aliased body.
  const uint16_t cHalo = ov565(0x031012);
  auto strokeFeature = [&](const Feature &ft, float w, uint16_t col, float alpha) {
    float x0, y0; toPx(ft.first, x0, y0);
    for (uint32_t k = ft.first + 1; k < ft.first + ft.count; ++k) {
      float x1, y1; toPx(k, x1, y1);
      ovLineAA(R, x0, y0, x1, y1, w, col, alpha);
      x0 = x1; y0 = y1;
    }
  };
  if (medium)
    for (int i = 0; i < featCount; ++i) {
      const Feature &ft = feats[i];
      if (ft.type != F_TAXIWAY || ft.count < 2) continue;
      const float w = detail ? max(1.0f, (ft.widthM > 0 ? ft.widthM : 23.0f) * pxPerM) : 1.0f;
      strokeFeature(ft, w, cTaxi, detail ? 0.85f : 0.6f);
    }
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < featCount; ++i) {
      const Feature &ft = feats[i];
      if (ft.type != F_RUNWAY || ft.count < 2) continue;
      const float w = max(medium ? 2.0f : 1.6f, (ft.widthM > 0 ? ft.widthM : 45.0f) * pxPerM);
      if (pass == 0) strokeFeature(ft, w + 2.0f, cHalo, 0.75f);
      else strokeFeature(ft, w, cRunway, 1.0f);
    }
  LogV.printf("Radar: aeroporto %s desenhado no mapa (%.0f km) em %lums\n", mapCode, radiusKm, (unsigned long)(millis() - t0));
}

namespace airport {

// ---------------------------------------------------------------- network: job entry
// ---------------------------------------------------------------- details of the selected aircraft
// Same source as the Flight Radar (adsbdb.com): model, owner, airline, route.
AptDetails det;                      // guarded by mtx

static void copyStr(JsonVariantConst v, char *out, size_t n) { snprintf(out, n, "%s", v.is<const char *>() ? v.as<const char *>() : ""); }

// ---------------------------------------------------------------- details on tap
// Route / airline / model (adsbdb) are fetched ONLY for the tapped aircraft, in the network
// task, once per aircraft per visit (kept in memory while the screen is open). The panel
// shows them for 5 s and closes by itself, so the screen goes back to positions only.
// (A parallel task was tried: the internal RAM never had room for a 2nd TLS context.)
AptDetails detCache[DET_CACHE];                    // guarded by mtx
static uint32_t detRetryAt[DET_CACHE];
static uint8_t detTries[DET_CACHE];
int detCacheN = 0;

int detFind(const char *hex) {                     // caller holds mtx
  for (int i = 0; i < detCacheN; ++i) if (!strcmp(detCache[i].hex, hex)) return i;
  return -1;
}

// One adsbdb request into `out` (hex/cs set by the caller). No lock held while fetching.
static void fetchDetailsInto(AptDetails &out) {
  char url[128];
  if (out.hex[0] == '~') snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", out.cs);   // parked (Aena): no ICAO hex
  else snprintf(url, sizeof url, "https://api.adsbdb.com/v0/aircraft/%s%s%s", out.hex, out.cs[0] ? "?callsign=" : "", out.cs);
  static PsramBuffer body(32 * 1024);
  body.clear();
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(5);
  const uint32_t t0 = millis();
  const int status = httpsFetch(client, url, body, 4500, false);
  client.stop();
  JsonDocument doc(SpiRamJsonAllocator::instance());
  const bool parsed = (status == 200 || status == 404) && !deserializeJson(doc, body.c_str(), body.length());
  JsonObjectConst ac = doc["response"]["aircraft"], fr = doc["response"]["flightroute"];
  if (!parsed) out.state = DET_FAILED;
  else if (ac.isNull() && fr.isNull()) out.state = DET_NOTFOUND;
  else {
    char man[24] = "", type[24] = "";
    copyStr(ac["manufacturer"], man, sizeof man); copyStr(ac["type"], type, sizeof type);
    snprintf(out.model, sizeof out.model, "%s%s%s", man, man[0] ? " " : "", type);
    copyStr(ac["registered_owner"], out.owner, sizeof out.owner);
    copyStr(fr["airline"]["name"], out.airline, sizeof out.airline);
    copyStr(fr["origin"]["iata_code"], out.org, sizeof out.org);
    copyStr(fr["destination"]["iata_code"], out.dst, sizeof out.dst);
    copyStr(fr["origin"]["municipality"], out.orgCity, sizeof out.orgCity);
    copyStr(fr["destination"]["municipality"], out.dstCity, sizeof out.dstCity);
    out.state = DET_READY;
  }
  LogV.printf("Airport: detalhes %s -> HTTP %d em %lums (%s)\n", out.hex, status, (unsigned long)(millis() - t0),
              out.state == DET_READY ? "ok" : out.state == DET_NOTFOUND ? "sem dados" : "falhou");
}

// Network task: fetch the details of the tapped aircraft (if still pending). ~1-2 s, only
// on a tap; the icons keep gliding by prediction meanwhile.
static void fetchTappedDetails() {
  AptDetails job = {};
  xSemaphoreTake(mtx, portMAX_DELAY);
  const bool pending = det.hex[0] && det.state == DET_PENDING;
  if (pending) { snprintf(job.hex, sizeof job.hex, "%s", det.hex); snprintf(job.cs, sizeof job.cs, "%s", det.cs); }
  xSemaphoreGive(mtx);
  if (!pending) return;
  lolClient.stop();                                       // one TLS context at a time
  fetchDetailsInto(job);
  xSemaphoreTake(mtx, portMAX_DELAY);
  int k = detFind(job.hex);
  if (k < 0 && detCacheN < DET_CACHE) { k = detCacheN++; detTries[k] = 0; detRetryAt[k] = 0; }
  if (k >= 0) { detCache[k] = job; if (job.state == DET_FAILED) ++detTries[k]; }
  if (!strcmp(det.hex, job.hex)) det = job;               // shown now (failure shows "—")
  xSemaphoreGive(mtx);
}

// ---------------------------------------------------------------- Aena board (CGH only)
// Aircraft parked at a stand are usually invisible to ADS-B (transponder off, buildings).
// The Aena Brasil flight page embeds the whole day's board as JSON (var departureFligth):
// flight, status, "box" (aircraft STAND = OSM parking_position ref) and "portal" (gate).
// Departures and arrivals are two pages; each is fetched every 3 min (alternating, one per
// cycle, AFTER the positions) only while this screen shows CGH. Parked aircraft are drawn
// at their stand (dimmed) and vanish as soon as the same flight shows up in ADS-B.
AenaStand aenaStands[AENA_MAX];                    // merged list, guarded by mtx (UI reads it)
int aenaN = 0;
static AenaStand aenaArr[AENA_MAX], aenaDep[AENA_MAX];    // network task only
static int aenaArrN = 0, aenaDepN = 0;
static uint32_t aenaArrMs = 0, aenaDepMs = 0;
// Departures already gone ("voo encerrado") per stand: an older arrival at that stand has left.
struct AenaClosed { char box[4]; int16_t mins; };
static AenaClosed aenaClosed[AENA_MAX * 2];
static int aenaClosedN = 0;

bool isCgh(const char *code) { return !strcmp(code, "CGH") || !strcmp(code, "SBSP"); }

// "11/11/2026 14:25" -> minutes from now (negative = past). false if no clock / bad text.
static bool aenaMinutes(const char *s, int &minutes, char *hhmm) {
  int d, mo, y, h, mi;
  if (sscanf(s, "%d/%d/%d %d:%d", &d, &mo, &y, &h, &mi) != 5) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  minutes = (int)((mktime(&t) - now) / 60);
  snprintf(hhmm, 6, "%02d:%02d", h, mi);
  return true;
}

// One board (departures or arrivals) -> list of aircraft that should be at a stand now.
static bool fetchAenaBoard(bool arrivals, AenaStand *out, int &outN) {
  static PsramBuffer body(768 * 1024);
  body.clear();
  const char *url = arrivals
      ? "https://www.aenabrasil.com.br/pt/aeroportos/aeroporto-de-congonhas/informacoes-de-voos.html?type=CHEGADAS&airportA=SBSP"
      : "https://www.aenabrasil.com.br/pt/aeroportos/aeroporto-de-congonhas/informacoes-de-voos.html";
  lolClient.stop();                                       // one TLS context at a time
  if (!tlsMemoryAvailable("aena")) return false;
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(6);
  HttpsFetchOptions opt;
  opt.userAgent = "Mozilla/5.0 (NextNeoVista; ESP32-S3 airport display)";
  const uint32_t t0 = millis();
  const int status = httpsFetch(client, url, body, 9000, false, nullptr, "application/json", opt);
  client.stop();
  if (status != 200 || body.overflowed()) { Log.printf("Airport: Aena %s HTTP %d\n", arrivals ? "chegadas" : "partidas", status); return false; }
  const char *html = body.c_str();
  const char *start = strstr(html, "departureFligth = '");
  if (!start) { Log.println("Airport: Aena sem dados de voos na página"); return false; }
  start += strlen("departureFligth = '");
  const char *end = strstr(start, "}';");
  if (!end) return false;
  ++end;                                                   // keep the closing brace
  JsonDocument filter(SpiRamJsonAllocator::instance());
  JsonObject f = filter["voos"]["voo"][0].to<JsonObject>();
  for (const char *k : {"numero", "situacao", "observacao", "box", "portal", "origem", "destino", "dataHoraEfetiva"}) f[k] = true;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (deserializeJson(doc, start, end - start, DeserializationOption::Filter(filter))) { Log.println("Airport: Aena JSON inválido"); return false; }
  outN = 0;
  int total = 0;
  if (!arrivals) aenaClosedN = 0;
  for (JsonObjectConst v : doc["voos"]["voo"].as<JsonArrayConst>()) {
    ++total;
    const char *sit = v["situacao"] | "";
    const char *box = v["box"] | "";
    if (!box[0] || !strcmp(box, "-")) continue;
    int mins; char hhmm[6];
    if (!aenaMinutes(v["dataHoraEfetiva"] | "", mins, hhmm)) continue;
    char boxRef[4];                                          // "07" -> "7" (OSM ref); "P01" kept as is
    if (isdigit((unsigned char)box[0])) snprintf(boxRef, sizeof boxRef, "%d", atoi(box)); else snprintf(boxRef, sizeof boxRef, "%s", box);
    if (!arrivals && !strcmp(sit, "FEC") && mins >= -120 && mins <= 0 && aenaClosedN < AENA_MAX * 2) {
      snprintf(aenaClosed[aenaClosedN].box, sizeof aenaClosed[0].box, "%s", boxRef);
      aenaClosed[aenaClosedN++].mins = (int16_t)mins;
    }
    bool keep;
    if (arrivals) keep = !strcmp(sit, "PSD") && mins >= -90 && mins <= 5;            // "aeronave no pátio", landed < 90 min ago
    else keep = (!strcmp(sit, "EMP") || !strcmp(sit, "EMI") || !strcmp(sit, "ULT") ||  // boarding soon / boarding / last call
                 !strcmp(sit, "CON") || !strcmp(sit, "ATR")) && mins >= -20 && mins <= 60;
    if (!keep || outN >= AENA_MAX) continue;
    AenaStand &a = out[outN++];
    memset(&a, 0, sizeof a);
    a.dep = !arrivals;
    snprintf(a.flight, sizeof a.flight, "%s", v["numero"] | "");
    snprintf(a.other, sizeof a.other, "%s", arrivals ? (v["origem"] | "") : (v["destino"] | ""));
    snprintf(a.gate, sizeof a.gate, "%s", v["portal"] | "");
    snprintf(a.box, sizeof a.box, "%s", boxRef);
    a.mins = (int16_t)mins;
    snprintf(a.status, sizeof a.status, "%s", v["observacao"] | "");
    snprintf(a.when, sizeof a.when, "%s", hhmm);
    snprintf(a.hex, sizeof a.hex, "~%s", a.box);          // synthetic id: never a real ICAO hex
  }
  Log.printf("Airport: Aena %s %d voos, %d em posição (%lums, %u bytes)\n", arrivals ? "chegadas" : "partidas",
             total, outN, (unsigned long)(millis() - t0), (unsigned)body.length());
  return true;
}

// Network task: refresh one board when due, then merge (one aircraft per stand; a boarding
// departure wins over the arrival that brought the aircraft there).
static void updateAena(const char *code) {
  if (!isCgh(code)) { if (aenaN) { xSemaphoreTake(mtx, portMAX_DELAY); aenaN = 0; xSemaphoreGive(mtx); } return; }
  const uint32_t now = millis();
  const bool depDue = !aenaDepMs || now - aenaDepMs >= 180000UL, arrDue = !aenaArrMs || now - aenaArrMs >= 180000UL;
  if (!depDue && !arrDue) return;
  const bool doArr = arrDue && (!depDue || aenaDepMs > aenaArrMs);   // alternate, never both in one cycle
  if (doArr) { aenaArrMs = now; if (!fetchAenaBoard(true, aenaArr, aenaArrN)) aenaArrN = 0; }
  else       { aenaDepMs = now; if (!fetchAenaBoard(false, aenaDep, aenaDepN)) aenaDepN = 0; }
  static AenaStand merged[AENA_MAX]; int m = 0;            // static: keep the network task stack small
  for (int i = 0; i < aenaDepN && m < AENA_MAX; ++i) merged[m++] = aenaDep[i];
  for (int i = 0; i < aenaArrN && m < AENA_MAX; ++i) {
    const AenaStand &a = aenaArr[i];
    bool skip = false;
    for (int k = 0; k < aenaArrN && !skip; ++k)            // a newer arrival at the same stand replaced it
      skip = k != i && !strcmp(aenaArr[k].box, a.box) && aenaArr[k].mins > a.mins;
    for (int k = 0; k < aenaClosedN && !skip; ++k)          // a departure from that stand closed after it landed
      skip = !strcmp(aenaClosed[k].box, a.box) && aenaClosed[k].mins > a.mins;
    for (int k = 0; k < m && !skip; ++k) skip = !strcmp(merged[k].box, a.box);   // boarding departure wins
    if (!skip) merged[m++] = a;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy(aenaStands, merged, m * sizeof(AenaStand)); aenaN = m;
  xSemaphoreGive(mtx);
}

// Stand position from the OSM map: parking_position line (nose towards the terminal) or gate node.
bool standPose(const char *box, double &lat, double &lon, float &heading, bool &hasHeading) {
  if (!mapReady) return false;
  int best = -1;
  for (int i = 0; i < featCount; ++i)
    if (feats[i].type == F_PARKING && !strcmp(feats[i].ref, box) && (best < 0 || feats[i].count > feats[best].count)) best = i;
  if (best < 0) return false;
  const Feature &ft = feats[best];
  auto unrot = [](float x, float y, float &e, float &n) { e = x * rotC - y * rotS; n = x * rotS + y * rotC; };
  float e0, n0; unrot(pts[ft.first * 2], pts[ft.first * 2 + 1], e0, n0);
  float e = e0, n = n0; hasHeading = false; heading = 0;
  if (ft.count >= 2) {
    const uint32_t last = ft.first + ft.count - 1;
    float e1, n1; unrot(pts[last * 2], pts[last * 2 + 1], e1, n1);
    // The nose is the end of the stand line closer to the terminal: the gate node with the
    // same ref when the map has one, else the terminal buildings' centre. (The direction
    // of the line itself varies in OSM: in CGH ~1/4 of the stands are drawn the other way.)
    float re = 0, rn = 0; int rc = 0;
    for (int i = 0; i < featCount; ++i)
      if (feats[i].type == F_PARKING && feats[i].count == 1 && !strcmp(feats[i].ref, box)) {
        unrot(pts[feats[i].first * 2], pts[feats[i].first * 2 + 1], re, rn); rc = 1; break;
      }
    if (!rc)
      for (int i = 0; i < featCount; ++i) if (feats[i].type == F_TERMINAL)
        for (uint32_t q = feats[i].first; q < feats[i].first + feats[i].count; ++q) {
          float te, tn; unrot(pts[q * 2], pts[q * 2 + 1], te, tn); re += te; rn += tn; ++rc;
        }
    if (rc) { re /= rc; rn /= rc; }
    float ne = e1, nn = n1, te = e0, tn = n0;              // nose / tail ends
    if (rc && (e0 - re) * (e0 - re) + (n0 - rn) * (n0 - rn) < (e1 - re) * (e1 - re) + (n1 - rn) * (n1 - rn)) {
      ne = e0; nn = n0; te = e1; tn = n1;
    }
    const float de = ne - te, dn = nn - tn, len = sqrtf(de * de + dn * dn);   // tail -> nose
    if (len > 1) {
      heading = fmodf(atan2f(de, dn) * 180 / (float)M_PI + 360, 360); hasHeading = true;
      const float k = len > 40 ? 20 / len : 0.5f;          // aircraft centre ~20 m behind the nose mark
      e = ne - de * k; n = nn - dn * k;
    }
  }
  lat = mapLat + n / 110540.0;
  lon = mapLon + e / (111320.0 * cos(mapLat * M_PI / 180.0));
  return true;
}

// Network task: close the keep-alive session when the Airport screen is not active.
static volatile bool lolOpen = false;
}  // namespace airport
using namespace airport;

void airportReleaseConnection() {
  if (!lolOpen) return;
  lolClient.stop();
  lolOpen = false;
  LogV.println("Airport: conexão ADSB.lol liberada");
}

namespace airport {

static int lastLolStatus = 0;     // last ADSB.lol HTTP status (diagnostics)
}  // namespace airport
using namespace airport;

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
        snprintf(baseCode, sizeof baseCode, "%s", code); baseReady = true; baseVersion = baseVersion + 1;
        LogV.printf("Airport: mapa de ruas %s carregado\n", code);
      } else { baseRetryAtMs = millis() + 120000; Log.printf("Airport: mapa de ruas falhou (%s)\n", err); }
    }
    if (!screenJobStillCurrent()) return false;
  }

  // Aircraft around the airfield from ADSB.lol (one network: coherent positions).
  const float radiusNm = 2.0f;                         // airfield + final approach / initial climb
  uint8_t n = 0, okMask = 0; char err[40] = "";
  char url[128];
  // Every poll (~10 s, only while this screen is shown): ADSB.lol and the other networks,
  // nothing cached between polls. Every request re-checks that the Airport screen is still
  // the active one, so leaving it stops the queries at once. One TLS context at a time.
  static bool lolConfigured = false;
  if (!lolConfigured) { lolClient.setInsecure(); lolClient.setHandshakeTimeout(5); lolConfigured = true; }
  snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.5f/%.5f/%u", lat, lon, (unsigned)ceilf(radiusNm));
  // ADSB.lol closes the connection after every answer (log: always "conexao nova"), so each
  // poll pays a ~2 s TLS handshake, and it answers 429 when polled faster than ~2-3 s.
  // Respect that: after a 429 wait 6 s between ADSB.lol polls for the next 60 s.
  if (screenJobStillCurrent()) fetchTappedDetails();     // only after a tap, once per aircraft
  // Two public networks with the same readsb format. The log showed ADSB.lol answering
  // 429 (rate limit) to this screen almost every time, while adsb.fi answered every
  // query in ~1 s. So adsb.fi is the main source here and ADSB.lol the fallback.
  // Only ONE network per cycle (positions from different networks are never mixed).
  // A network that answers 429 / fails rests 10 s, doubling up to 60 s; each success
  // halves it. Between queries the moving aircraft follow the prediction.
  // A tap queues an extra cycle for the details: it never forces an early query.
  struct NetState { uint32_t lastMs, backoffMs; };
  static NetState fi = {0, 0}, lol = {0, 0};
  static constexpr uint32_t FI_GAP = 3500, LOL_GAP = 4500;   // adsb.fi allows ~1 query/s per IP
  char lolUrl[128];
  snprintf(lolUrl, sizeof lolUrl, "%s", url);
  char fiUrl[128];
  snprintf(fiUrl, sizeof fiUrl, "https://opendata.adsb.fi/api/v2/lat/%.5f/lon/%.5f/dist/%u", lat, lon, (unsigned)ceilf(radiusNm));
  auto due = [](const NetState &st, uint32_t gap) {
    return !st.lastMs || millis() - st.lastMs >= (st.backoffMs ? st.backoffMs : gap);
  };
  auto rest = [](NetState &st, const char *name, int status) {
    st.backoffMs = st.backoffMs ? (st.backoffMs * 2 > 60000UL ? 60000UL : st.backoffMs * 2) : 10000UL;
    Log.printf("Airport: %s %s, em pausa por %lus\n", name, status == 429 ? "pediu calma (429)" : "falhou",
               (unsigned long)(st.backoffMs / 1000));
  };
  auto okAgain = [](NetState &st) { st.backoffMs = st.backoffMs / 2 < 4000UL ? 0 : st.backoffMs / 2; };
  bool tried = false, got429 = false;
  const bool fiResting = fi.backoffMs && !due(fi, FI_GAP);
  if (!fiResting && due(fi, FI_GAP)) {                      // main: adsb.fi
    tried = true; fi.lastMs = millis();
    lolClient.stop();                                       // one TLS context at a time
    if (fetchExtra(fiUrl, SRC_ADSBFI, lat, lon, radiusNm, fetchBuf, fetchSrc, fetchGeom, n)) { okMask |= SRC_ADSBFI; okAgain(fi); }
    else { got429 = lastFetchStatus == 429; rest(fi, "adsb.fi", lastFetchStatus); snprintf(err, sizeof err, "adsb.fi sem resposta"); }
  }
  // fallback: ADSB.lol, only while adsb.fi is resting (or just failed in this cycle)
  if (!okMask && (fi.backoffMs || got429) && due(lol, LOL_GAP) && screenJobStillCurrent()) {
    tried = true; lol.lastMs = millis(); lolOpen = true; n = 0;
    if (fetchExtra(lolUrl, SRC_ADSBLOL, lat, lon, radiusNm, fetchBuf, fetchSrc, fetchGeom, n, &lolClient)) { okMask |= SRC_ADSBLOL; okAgain(lol); }
    else { if (lastFetchStatus == 429) got429 = true; rest(lol, "ADSB.lol", lastFetchStatus); snprintf(err, sizeof err, "ADSB.lol sem resposta"); }
    lastLolStatus = lastFetchStatus;
  }
  if (!tried) return true;                                  // too soon: keep the current positions
  if (!okMask && got429) return true;                       // rate limited everywhere: keep positions

  // Single live source (same as the Flight Radar): ADSB.lol only, polled almost
  // continuously. Mixing networks (adsb.fi, airplanes.live, adsb.one) gave the same
  // aircraft positions with different ages from different receivers -> icons jumped.
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

  // Aena: aircraft at a stand that ADS-B does not see (CGH only). Fetched AFTER positions.
  if (ok && screenJobStillCurrent()) {
    updateAena(code);
    xSemaphoreTake(mtx, portMAX_DELAY);
    static AenaStand st[AENA_MAX]; const int sn = aenaN; memcpy(st, aenaStands, sn * sizeof(AenaStand));
    xSemaphoreGive(mtx);
    for (int k = 0; k < sn && n < MAX_AC; ++k) {
      bool live = false;                                   // already flying/taxiing in ADS-B: real position wins
      for (int i = 0; i < n; ++i) {
        char cs[9]; snprintf(cs, sizeof cs, "%s", fetchBuf[i].callsign);
        for (char *c = cs + strlen(cs); c > cs && c[-1] == ' '; --c) c[-1] = 0;
        if (!strcmp(cs, st[k].flight)) { live = true; break; }
      }
      double la, lo; float hd; bool hh;
      if (live || !standPose(st[k].box, la, lo, hd, hh)) continue;
      Aircraft a;
      snprintf(a.icao24, sizeof a.icao24, "%s", st[k].hex);
      snprintf(a.callsign, sizeof a.callsign, "%s", st[k].flight);
      a.latitude = la; a.longitude = lo; a.onGround = true;
      a.hasHeading = hh; a.headingDegrees = hd;
      fetchBuf[n] = a; fetchSrc[n] = SRC_AENA; fetchGeom[n] = NAN; memo[n] = 0; ++n;
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
  if (lastCycleStart && cycleStart - lastCycleStart > 8000)
    Log.printf("Airport: atualização lenta: intervalo %lums (ciclo %lums, fontes %c%c, ADSB.lol %d)\n",
               (unsigned long)(cycleStart - lastCycleStart), (unsigned long)(millis() - cycleStart),
               okMask & SRC_ADSBLOL ? 'L' : '-', okMask & SRC_ADSBFI ? 'F' : '-', lastLolStatus);
  LogV.printf("Airport: ciclo %lums, intervalo %lums, interno livre %u (maior bloco %u)\n", (unsigned long)(millis() - cycleStart),
             (unsigned long)(lastCycleStart ? cycleStart - lastCycleStart : 0), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  lastCycleStart = cycleStart;
  return ok;
}

namespace airport {


}  // namespace airport
