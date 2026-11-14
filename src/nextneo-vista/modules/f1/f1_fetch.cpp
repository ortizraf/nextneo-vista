// F1 network side (network task only): Jolpica calendar/standings, circuit outlines
// (bacinger/f1-circuits), OpenStreetMap surroundings, ArcGIS picture and the race weather.
// Results are cached on LittleFS and published to the screen through f1::shared.
#include "app_screens.h"
#include "modules/photos/photos.h"
#include "services/app_log.h"
#include "services/https_fetch.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <JPEGDEC.h>
#include <new>
#include "modules/f1/f1_internal.h"
#include "services/psram_alloc.h"

namespace f1 {

static const CircuitInfo kCircuits[] = {
  {"albert_park", "au-1953", "GP da Austr\xC3\xA1lia", "Austr\xC3\xA1lia"},
  {"shanghai", "cn-2004", "GP da China", "China"},
  {"suzuka", "jp-1962", "GP do Jap\xC3\xA3o", "Jap\xC3\xA3o"},
  {"bahrain", "bh-2002", "GP do Bahrein", "Bahrein"},
  {"jeddah", "sa-2021", "GP da Ar\xC3\xA1" "bia Saudita", "Ar\xC3\xA1" "bia Saudita"},
  {"miami", "us-2022", "GP de Miami", "EUA"},
  {"villeneuve", "ca-1978", "GP do Canad\xC3\xA1", "Canad\xC3\xA1"},
  {"monaco", "mc-1929", "GP de M\xC3\xB4naco", "M\xC3\xB4naco"},
  {"catalunya", "es-1991", "GP de Barcelona", "Espanha"},
  {"red_bull_ring", "at-1969", "GP da \xC3\x81ustria", "\xC3\x81ustria"},
  {"silverstone", "gb-1948", "GP da Inglaterra", "Reino Unido"},
  {"spa", "be-1925", "GP da B\xC3\xA9lgica", "B\xC3\xA9lgica"},
  {"hungaroring", "hu-1986", "GP da Hungria", "Hungria"},
  {"zandvoort", "nl-1948", "GP da Holanda", "Holanda"},
  {"monza", "it-1922", "GP da It\xC3\xA1lia", "It\xC3\xA1lia"},
  {"imola", "it-1953", "GP da Emilia-Romagna", "It\xC3\xA1lia"},
  {"madring", "es-2026", "GP da Espanha", "Espanha"},
  {"baku", "az-2016", "GP do Azerbaij\xC3\xA3o", "Azerbaij\xC3\xA3o"},
  {"sepang", "my-1999", "GP da Mal\xC3\xA1sia", "Mal\xC3\xA1sia"},
  {"marina_bay", "sg-2008", "GP de Singapura", "Singapura"},
  {"americas", "us-2012", "GP dos EUA", "EUA"},
  {"rodriguez", "mx-1962", "GP da Cidade do M\xC3\xA9xico", "M\xC3\xA9xico"},
  {"interlagos", "br-1940", "GP de S\xC3\xA3o Paulo", "Brasil"},
  {"vegas", "us-2023", "GP de Las Vegas", "EUA"},
  {"losail", "qa-2004", "GP do Catar", "Catar"},
  {"yas_marina", "ae-2009", "GP de Abu Dhabi", "Emirados \xC3\x81rabes"},
};
const CircuitInfo *circuitInfo(const char *cid) {
  for (const auto &c : kCircuits) if (!strcmp(c.cid, cid)) return &c;
  return nullptr;
}

static F1Data *net;                                       // built by the network task (PSRAM)
F1Data *shared;                                    // last complete copy, under `lock`
SemaphoreHandle_t lock;
volatile uint32_t mapVersion;                      // bumped when a circuit file is written
char mapWanted[20];                                // circuit the UI shows (network task fetches its map)

static time_t timegmSimple(int y, int mo, int d, int h, int mi, int s) {
  y -= mo <= 2; const int era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + (long)doe - 719468L;
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + s);
}
time_t parseUtc(const char *date, const char *tm) {    // "2026-03-08", "04:00:00Z"
  if (!date || strlen(date) < 10) return 0;
  int y, mo, d, h = 12, mi = 0, s = 0;
  if (sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3) return 0;
  if (tm) sscanf(tm, "%d:%d:%d", &h, &mi, &s);
  return timegmSimple(y, mo, d, h, mi, s);
}

// ============================================================== network task
char netMsg[72];                                   // last network problem, shown while nothing is loaded
static int fetchJson(const char *url, JsonDocument &doc, PsramBuffer &body, const char *postBody = nullptr) {
  if (!tlsMemoryAvailable("f1")) { snprintf(netMsg, sizeof netMsg, "mem\xC3\xB3ria baixa p/ conex\xC3\xA3o, tentando de novo"); return -1; }
  body.clear();
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
  HttpsFetchOptions opt; opt.userAgent = "NextNeoVista/1.0 (ESP32-S3 F1 display)";
  const uint32_t t0 = millis();
  const int st = httpsFetch(client, url, body, 25000, false, postBody,
                            postBody ? "application/x-www-form-urlencoded" : "application/json", opt);
  client.stop();
  LogV.printf("F1: %.60s -> HTTP %d, %u bytes, %lums\n", url + 8, st, (unsigned)body.length(), (unsigned long)(millis() - t0));
  if (st != 200 || body.overflowed()) { snprintf(netMsg, sizeof netMsg, "servidor respondeu %d, tentando de novo", st); return st == 200 ? -2 : st; }
  if (deserializeJson(doc, body.c_str(), body.length())) { snprintf(netMsg, sizeof netMsg, "resposta inv\xC3\xA1lida, tentando de novo"); return -3; }
  netMsg[0] = 0;
  return 200;
}

static bool fetchSchedule(PsramBuffer &body) {
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (fetchJson("https://api.jolpi.ca/ergast/f1/current.json?limit=40", doc, body) != 200) return false;
  JsonArrayConst races = doc["MRData"]["RaceTable"]["Races"];
  if (races.isNull()) return false;
  static const char *kKey[S_COUNT] = {"FirstPractice", "SecondPractice", "ThirdPractice", "SprintQualifying", "Sprint", "Qualifying", nullptr};
  F1Data &d = *net;
  int n = 0;
  for (JsonObjectConst r : races) {
    if (n >= 30) break;
    Race &x = d.races[n++];
    memset(&x, 0, sizeof x);
    x.round = atoi(r["round"] | "0");
    copyStr(r["Circuit"]["circuitId"], x.cid, sizeof x.cid);
    copyStr(r["raceName"], x.name, sizeof x.name);
    copyStr(r["Circuit"]["Location"]["locality"], x.locality, sizeof x.locality);
    copyStr(r["Circuit"]["Location"]["country"], x.country, sizeof x.country);
    x.lat = atof(r["Circuit"]["Location"]["lat"] | "0"); x.lon = atof(r["Circuit"]["Location"]["long"] | "0");
    for (int s = 0; s < S_COUNT; ++s) {
      if (s == S_RACE) x.t[s] = parseUtc(r["date"], r["time"]);
      else if (!r[kKey[s]].isNull()) x.t[s] = parseUtc(r[kKey[s]]["date"], r[kKey[s]]["time"]);
    }
  }
  d.nRaces = n;
  d.season = atoi(doc["MRData"]["RaceTable"]["season"] | "0");
  d.schedMs = millis();
  Log.printf("F1: calend\xC3\xA1rio %d, %d corridas\n", d.season, n);
  return n > 0;
}

static bool fetchStandings(PsramBuffer &body) {
  F1Data &d = *net;
  bool ok = true;
  {
    JsonDocument doc(SpiRamJsonAllocator::instance());
    if (fetchJson("https://api.jolpi.ca/ergast/f1/current/last/results.json?limit=3", doc, body) == 200) {
      JsonObjectConst r = doc["MRData"]["RaceTable"]["Races"][0];
      if (!r.isNull()) {
        d.resultRound = atoi(r["round"] | "0");
        const CircuitInfo *ci = circuitInfo(r["Circuit"]["circuitId"] | "");
        copyStr(ci ? ci->gp : (const char *)(r["raceName"] | ""), d.resultRace, sizeof d.resultRace);
        int k = 0;
        for (JsonObjectConst x : r["Results"].as<JsonArrayConst>()) {
          if (k >= 3) break;
          copyStr(x["Driver"]["familyName"], d.podium[k++], sizeof d.podium[0]);
        }
      }
    } else ok = false;
  }
  if (!screenJobStillCurrent()) return false;
  {
    JsonDocument doc(SpiRamJsonAllocator::instance());
    if (fetchJson("https://api.jolpi.ca/ergast/f1/current/driverStandings.json?limit=30", doc, body) == 200) {
      int n = 0;
      for (JsonObjectConst x : doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"].as<JsonArrayConst>()) {
        if (n >= 24) break;
        Standing &s = d.drivers[n++];
        s.pos = n;
        if (!x["position"].isNull()) s.pos = atoi(x["position"] | "0");
        char given[2] = {(char)(((const char *)(x["Driver"]["givenName"] | "?"))[0]), 0};
        snprintf(s.name, sizeof s.name, "%s. %s", given, (const char *)(x["Driver"]["familyName"] | ""));
        copyStr(x["Constructors"][0]["name"], s.team, sizeof s.team);
        copyStr(x["Constructors"][0]["constructorId"], s.teamId, sizeof s.teamId);
        s.pts = atof(x["points"] | "0"); s.wins = atoi(x["wins"] | "0");
      }
      d.nDrivers = n;
    } else ok = false;
  }
  if (!screenJobStillCurrent()) return false;
  {
    JsonDocument doc(SpiRamJsonAllocator::instance());
    if (fetchJson("https://api.jolpi.ca/ergast/f1/current/constructorStandings.json?limit=20", doc, body) == 200) {
      int n = 0;
      for (JsonObjectConst x : doc["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"].as<JsonArrayConst>()) {
        if (n >= 12) break;
        Standing &s = d.teams[n++];
        s.pos = n;
        if (!x["position"].isNull()) s.pos = atoi(x["position"] | "0");
        copyStr(x["Constructor"]["name"], s.name, sizeof s.name);
        copyStr(x["Constructor"]["constructorId"], s.teamId, sizeof s.teamId);
        s.team[0] = 0;
        s.pts = atof(x["points"] | "0"); s.wins = atoi(x["wins"] | "0");
      }
      d.nTeams = n;
    } else ok = false;
  }
  if (ok) d.standMs = millis();
  Log.printf("F1: campeonato %d pilotos, %d equipes, resultado rodada %d\n", d.nDrivers, d.nTeams, d.resultRound);
  return ok;
}


bool fsOk() { if (!photosFsReady()) return false; if (!LittleFS.exists("/f1")) LittleFS.mkdir("/f1"); return true; }
void pathFor(char *out, size_t n, char kind, const char *cid) { snprintf(out, n, "/f1/%c_%s.bin", kind, cid); }

static bool fetchTracks(PsramBuffer &body) {      // one download (~120 KB) -> every circuit of the table
  JsonDocument filter;
  filter["features"][0]["properties"]["id"] = true;
  filter["features"][0]["properties"]["length"] = true;
  filter["features"][0]["geometry"]["coordinates"] = true;
  if (!tlsMemoryAvailable("f1-tracks")) return false;
  body.clear();
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
  const int st = httpsFetch(client, "https://raw.githubusercontent.com/bacinger/f1-circuits/master/f1-circuits.geojson", body, 25000, false);
  client.stop();
  if (st != 200 || body.overflowed()) { Log.printf("F1: tra\xC3\xA7" "ados HTTP %d\n", st); return false; }
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) return false;
  int saved = 0;
  for (JsonObjectConst f : doc["features"].as<JsonArrayConst>()) {
    const char *id = f["properties"]["id"] | "";
    const CircuitInfo *ci = nullptr;
    for (const auto &c : kCircuits) if (!strcmp(c.track, id)) ci = &c;
    if (!ci) continue;
    JsonArrayConst co = f["geometry"]["coordinates"];
    const int n = co.size();
    if (n < 10 || n > 2000) continue;
    double sLat = 0, sLon = 0;
    for (JsonArrayConst p : co) { sLon += p[0].as<double>(); sLat += p[1].as<double>(); }
    TrackHdr h = {T_MAGIC, (float)(sLat / n), (float)(sLon / n), (uint16_t)(f["properties"]["length"] | 0), (uint16_t)n};
    const double kx = cos(h.lat0 * M_PI / 180) * 111320.0, ky = 110540.0;
    char path[40]; pathFor(path, sizeof path, 't', ci->cid);
    File fl = LittleFS.open(path, "w");
    if (!fl) continue;
    fl.write((const uint8_t *)&h, sizeof h);
    for (JsonArrayConst p : co) {
      int16_t xy[2] = {(int16_t)lround((p[0].as<double>() - h.lon0) * kx), (int16_t)lround((p[1].as<double>() - h.lat0) * ky)};
      fl.write((const uint8_t *)xy, 4);
    }
    fl.close(); ++saved;
  }
  Log.printf("F1: %d tra\xC3\xA7" "ados salvos\n", saved);
  return saved > 0;
}

bool loadTrackHdr(const char *cid, TrackHdr &h, int16_t *pts, int maxPts) {
  char path[40]; pathFor(path, sizeof path, 't', cid);
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f || f.read((uint8_t *)&h, sizeof h) != sizeof h || h.magic != T_MAGIC) return false;
  if (pts) { const int n = h.n < maxPts ? h.n : maxPts; f.read((uint8_t *)pts, n * 4); h.n = n; }
  return true;
}

static void urlEncode(const char *s, String &out) {
  static const char *hex = "0123456789ABCDEF";
  for (; *s; ++s) {
    const uint8_t c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
}

static int classify(JsonObjectConst tags) {
  const char *hw = tags["highway"] | "";
  if (!strcmp(hw, "motorway") || !strcmp(hw, "trunk") || !strcmp(hw, "primary") || !strcmp(hw, "secondary")) return K_ROAD;
  if (!strcmp(hw, "tertiary") || !strcmp(hw, "residential") || !strcmp(hw, "unclassified") || !strcmp(hw, "pedestrian") ||
      !strcmp(hw, "living_street")) return K_STREET;
  if (!strcmp(hw, "footway") || !strcmp(hw, "path") || !strcmp(hw, "cycleway") || !strcmp(hw, "track") || !strcmp(hw, "service"))
    return K_PATH;
  if (hw[0] && strcmp(hw, "raceway")) return -1;
  if (!strcmp(hw, "raceway")) {                          // pit lane, or a named piece of the track (corner names)
    const char *nm = tags["name"] | "";
    if (strstr(nm, "Pit") || strstr(nm, "pit") || strstr(nm, "Box") || !tags["service"].isNull() ||
        !strcmp(tags["raceway"] | "", "pit_lane")) return K_PIT;
    if (!strcmp(tags["sport"] | "motor", "karting")) return K_ALT;
    return nm[0] ? K_CORNER : K_ALT;                    // unnamed pieces: other layouts, drawn in grey
  }
  if (!strcmp(tags["amenity"] | "", "parking")) return K_PARKING;
  const char *bd = tags["building"] | "";
  if (!strcmp(bd, "grandstand") || !strcmp(tags["leisure"] | "", "bleachers")) return K_STAND;
  const char *nat = tags["natural"] | "", *ww = tags["waterway"] | "", *lu = tags["landuse"] | "", *le = tags["leisure"] | "";
  if (!strcmp(nat, "coastline")) return K_COAST;
  if (!strcmp(ww, "river") || !strcmp(ww, "canal")) return K_WLINE;
  if (!strcmp(nat, "water") || !strcmp(nat, "bay") || !strcmp(ww, "riverbank") || !strcmp(ww, "dock") ||
      !strcmp(lu, "reservoir") || !strcmp(lu, "basin") || !strcmp(le, "marina") || !strcmp(le, "swimming_pool") ||
      !strcmp(le, "water_park")) return K_WATER;
  if (!strcmp(nat, "beach") || !strcmp(nat, "sand")) return K_SAND;
  if (!tags["building"].isNull() || !strcmp(le, "stadium")) return K_BUILD;
  return K_GREEN;
}

// Overpass: water, green, sand and big buildings around the track; simplified and saved.
static bool fetchSurroundings(const char *cid, PsramBuffer &body) {
  TrackHdr h; static int16_t *pts; if (!psram(pts, 2 * 2000)) return false;
  if (!loadTrackHdr(cid, h, pts, 2000)) return false;
  int minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
  for (int i = 0; i < h.n; ++i) {
    minX = imin(minX, pts[2 * i]); maxX = imax(maxX, pts[2 * i]);
    minY = imin(minY, pts[2 * i + 1]); maxY = imax(maxY, pts[2 * i + 1]);
  }
  const double span = imax(maxX - minX, maxY - minY) * 0.75 + 300.0;            // a margin around the track
  const double cx = (minX + maxX) / 2.0, cy = (minY + maxY) / 2.0;
  const double kx = cos(h.lat0 * M_PI / 180) * 111320.0, ky = 110540.0;
  const double s = h.lat0 + (cy - span) / ky, n = h.lat0 + (cy + span) / ky, w = h.lon0 + (cx - span) / kx, e = h.lon0 + (cx + span) / kx;
  char bb[96]; snprintf(bb, sizeof bb, "(%.5f,%.5f,%.5f,%.5f)", s, w, n, e);
  char q[1900];
  snprintf(q, sizeof q,
           "[out:json][timeout:25];("
           "way[\"natural\"~\"^(water|bay|wood|scrub|grassland|beach|sand|coastline)$\"]%s;"
           "way[\"waterway\"~\"^(riverbank|dock|river|canal)$\"]%s;"
           "way[\"landuse\"~\"^(forest|grass|meadow|recreation_ground|village_green|reservoir|basin)$\"]%s;"
           "way[\"leisure\"~\"^(park|garden|golf_course|marina|stadium)$\"]%s;"
           "way[\"building\"](if:length()>240)%s;way[\"building\"=\"grandstand\"]%s;way[\"leisure\"=\"bleachers\"]%s;"
           "way[\"highway\"=\"raceway\"]%s;"
           "way[\"highway\"~\"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|pedestrian|living_street|footway|path|cycleway|track)$\"]%s;"
           "way[\"amenity\"=\"parking\"]%s;way[\"leisure\"~\"^(swimming_pool|water_park)$\"]%s;"
           "rel[\"natural\"~\"^(water|wood)$\"]%s;rel[\"landuse\"=\"forest\"]%s;rel[\"leisure\"=\"park\"]%s;"
           ");out geom qt;", bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb);
  String post = "data="; urlEncode(q, post);
  // overpass-api.de answers 406 to some clients: start with the mirrors, try all three in one go
  static const char *kEndpoints[] = {"https://overpass.private.coffee/api/interpreter",
                                     "https://maps.mail.ru/osm/tools/overpass/api/interpreter", "https://overpass-api.de/api/interpreter"};
  JsonDocument filter;
  JsonObject fe = filter["elements"].add<JsonObject>();
  fe["type"] = true;
  for (const char *t : {"natural", "waterway", "landuse", "leisure", "building", "highway", "name", "name:pt", "service", "raceway", "sport", "amenity"})
    fe["tags"][t] = true;
  fe["geometry"][0]["lat"] = true; fe["geometry"][0]["lon"] = true;
  fe["members"][0]["role"] = true; fe["members"][0]["geometry"][0]["lat"] = true; fe["members"][0]["geometry"][0]["lon"] = true;
  bool got = false;
  for (int k = 0; k < 3 && !got; ++k) {
    if (!tlsMemoryAvailable("f1-osm") || !screenJobStillCurrent()) return false;
    body.clear();
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(15);
    HttpsFetchOptions opt; opt.userAgent = "NextNeoVista/1.0 (ESP32-S3 F1 circuit map)";
    const uint32_t t0 = millis();
    const int st = httpsFetch(client, kEndpoints[k], body, 30000, false, post.c_str(), "application/x-www-form-urlencoded", opt);
    client.stop();
    Log.printf("F1: OSM %s via %.24s HTTP %d, %u bytes, %lums\n", cid, kEndpoints[k] + 8, st, (unsigned)body.length(), (unsigned long)(millis() - t0));
    got = st == 200 && !body.overflowed() && body.length() && body.c_str()[0] == '{';
  }
  if (!got) return false;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) return false;
  // file: magic, count, then per shape: kind, nrings, per ring: n, int16 pts (built in PSRAM first)
  static constexpr size_t OB_MAX = 512 * 1024;
  uint8_t *ob = (uint8_t *)heap_caps_malloc(OB_MAX, MALLOC_CAP_SPIRAM);
  if (!ob) return false;
  size_t olen = 6; uint16_t count = 0;
  auto put = [&](const void *src, size_t k) { if (olen + k <= OB_MAX) { memcpy(ob + olen, src, k); olen += k; } };
  const double step = span / 400.0;                              // ~0.7 px on the map
  static int16_t *ring; if (!psram(ring, 2 * 1500)) { heap_caps_free(ob); return false; }
  auto writeRing = [&](JsonArrayConst g, bool line) -> bool {
    int m = 0; double lx = 1e9, ly = 1e9;
    for (JsonObjectConst p : g) {
      if (p.isNull() || m >= 1500) continue;
      const double x = (p["lon"].as<double>() - h.lon0) * kx, y = (p["lat"].as<double>() - h.lat0) * ky;
      if (m && fabs(x - lx) + fabs(y - ly) < step) continue;
      ring[2 * m] = (int16_t)constrain(lround(x), -32000L, 32000L); ring[2 * m + 1] = (int16_t)constrain(lround(y), -32000L, 32000L);
      lx = x; ly = y; ++m;
    }
    if (m < (line ? 2 : 3) || olen + 2 + m * 4 + 64 > OB_MAX) return false;
    const uint16_t mm = m; put(&mm, 2); put(ring, m * 4);
    return true;
  };
  for (JsonObjectConst el : doc["elements"].as<JsonArrayConst>()) {
    const int kind = classify(el["tags"]);
    if (kind < 0) continue;
    const bool line = kind == K_WLINE || kind == K_PIT || kind == K_CORNER || kind == K_COAST || kind == K_ROAD || kind == K_STREET || kind == K_ALT || kind == K_PATH;
    const size_t at = olen;
    olen += 2;
    uint8_t rings = 0;
    if (!el["geometry"].isNull()) { if (writeRing(el["geometry"], line)) rings = 1; }
    else for (JsonObjectConst mb : el["members"].as<JsonArrayConst>()) {
      if (rings >= 60 || mb["geometry"].isNull()) continue;
      if (writeRing(mb["geometry"], line)) ++rings;
    }
    if (!rings || olen > OB_MAX - 64) { olen = at; continue; }   // nothing usable: roll back
    if (kind == K_CORNER) {                                       // + name (len, bytes, padded to 2)
      const char *nm = el["tags"]["name:pt"] | (const char *)(el["tags"]["name"] | "");
      uint8_t len = strlen(nm) > 40 ? 40 : strlen(nm);
      put(&len, 1); put(nm, len); if ((len + 1) & 1) { const uint8_t z = 0; put(&z, 1); }
    }
    ob[at] = kind; ob[at + 1] = rings;
    ++count;
  }
  const uint32_t mg = M_MAGIC;
  memcpy(ob, &mg, 4); memcpy(ob + 4, &count, 2);
  char path[40]; pathFor(path, sizeof path, 'v', cid);
  File fl = LittleFS.open(path, "w");
  const bool wrote = fl && fl.write(ob, olen) == olen;
  if (fl) fl.close();
  heap_caps_free(ob);
  if (!wrote) { LittleFS.remove(path); return false; }
  const size_t total = olen;
  Log.printf("F1: entorno de %s salvo (%u formas, %u bytes)\n", cid, count, (unsigned)total);
  return true;
}

// ---- background picture of the circuit area (ArcGIS export, like the radar map):
//      satellite photo (real run-off areas, grandstands, lakes) or street map.
volatile uint8_t mapStyle = ST_STREET;
void imagePath(char *out, size_t n, uint8_t style, const char *cid) { pathFor(out, n, style == ST_SAT ? 's' : 'r', cid); }

// metres -> map pixels for a circuit (same formula as renderMap, so the picture lines up)
static bool viewFor(const char *cid, TrackHdr &h, float &cx, float &cy, float &sc) {
  static int16_t *pts; if (!psram(pts, 2 * 2000)) return false;
  if (!loadTrackHdr(cid, h, pts, 2000)) return false;
  int n = h.n;
  if (n > 2 && pts[0] == pts[2 * (n - 1)] && pts[1] == pts[2 * n - 1]) --n;
  int minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
  for (int i = 0; i < n; ++i) { minX = imin(minX, pts[2 * i]); maxX = imax(maxX, pts[2 * i]); minY = imin(minY, pts[2 * i + 1]); maxY = imax(maxY, pts[2 * i + 1]); }
  cx = (minX + maxX) / 2.0f; cy = (minY + maxY) / 2.0f;
  sc = fminf((MAP_W - 26) / (float)imax(1, maxX - minX), (MAP_H - 26) / (float)imax(1, maxY - minY));
  return true;
}

static bool fetchImage(const char *cid, uint8_t style, PsramBuffer &body) {
  TrackHdr h; float cx, cy, sc;
  if (!viewFor(cid, h, cx, cy, sc)) return false;
  const double kx = cos(h.lat0 * M_PI / 180) * 111320.0, ky = 110540.0, R = 6378137.0, rad = M_PI / 180;
  const double lonL = h.lon0 + (cx - MAP_W / 2.0 / sc) / kx, lonR = h.lon0 + (cx + MAP_W / 2.0 / sc) / kx;
  const double latB = h.lat0 + (cy - MAP_H / 2.0 / sc) / ky, latT = h.lat0 + (cy + MAP_H / 2.0 / sc) / ky;
  auto merY = [&](double lat) { return R * log(tan(M_PI / 4 + lat * rad / 2)); };
  char url[360];
  snprintf(url, sizeof url, "https://server.arcgisonline.com/arcgis/rest/services/%s/MapServer/export"
           "?bbox=%.1f,%.1f,%.1f,%.1f&bboxSR=3857&imageSR=3857&size=%d,%d&format=jpg&f=image",
           style == ST_SAT ? "World_Imagery" : "World_Street_Map", R * lonL * rad, merY(latB), R * lonR * rad, merY(latT), MAP_W, MAP_H);
  if (!tlsMemoryAvailable("f1-img")) return false;
  body.clear();
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(12);
  const uint32_t t0 = millis();
  const int st = httpsFetch(client, url, body, 20000, false);
  client.stop();
  Log.printf("F1: imagem %s (%s) HTTP %d, %u bytes, %lums\n", cid, style == ST_SAT ? "sat\xC3\xA9lite" : "ruas", st,
             (unsigned)body.length(), (unsigned long)(millis() - t0));
  if (st != 200 || body.overflowed() || body.length() < 1000 || (uint8_t)body.c_str()[0] != 0xFF) return false;
  char path[40]; imagePath(path, sizeof path, style, cid);
  File f = LittleFS.open(path, "w");
  const bool ok = f && f.write(body.data(), body.length()) == body.length();
  if (f) f.close();
  if (!ok) LittleFS.remove(path);
  return ok;
}

// Forecast at the circuit (local time of the circuit) for every session of race r.
static bool fetchRaceWeather(F1Data &d, const Race &r, PsramBuffer &body) {
  char url[300];
  snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&hourly=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=16", r.lat, r.lon);
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (fetchJson(url, doc, body) != 200) return false;
  const long off = doc["utc_offset_seconds"] | 0;
  const char *h0 = doc["hourly"]["time"][0] | "", *d0 = doc["daily"]["time"][0] | "";
  if (strlen(h0) < 16 || strlen(d0) < 10) return false;
  const time_t hour0 = parseUtc(h0, h0 + 11), day0 = parseUtc(d0, "00:00:00");   // local clock of the circuit
  JsonArrayConst ht = doc["hourly"]["temperature_2m"], hc = doc["hourly"]["weather_code"], hd = doc["hourly"]["is_day"];
  JsonArrayConst dmax = doc["daily"]["temperature_2m_max"], dmin = doc["daily"]["temperature_2m_min"];
  for (int s = 0; s < S_COUNT; ++s) {
    d.wxHave[s] = false;
    if (!r.t[s]) continue;
    const long hi = (long)((r.t[s] + off - hour0 + 1800) / 3600), di = (long)((r.t[s] + off - day0) / 86400);
    if (hi < 0 || hi >= (long)ht.size() || di < 0 || di >= (long)dmax.size() || ht[hi].isNull()) continue;
    d.wxTemp[s] = ht[hi].as<float>(); d.wxCode[s] = hc[hi] | 0; d.wxNight[s] = (hd[hi] | 1) == 0;
    d.wxMax[s] = dmax[di].as<float>(); d.wxMin[s] = dmin[di].as<float>();
    d.wxHave[s] = true;
  }
  copyStr(r.cid, d.wxCid, sizeof d.wxCid);
  d.wxOff = off;
  d.wxMs = millis();
  Log.printf("F1: previs\xC3\xA3o do tempo em %s carregada\n", r.cid);
  return true;
}

// Network task (CMD_F1). Decides what is stale; quick no-op when everything is fresh.
bool f1Fetch() {
  if (!lock || !shared) return false;                   // screen not entered yet
  if (!net) {
    net = (F1Data *)heap_caps_calloc(1, sizeof(F1Data), MALLOC_CAP_SPIRAM);
    if (!net) return false;
  }
  static PsramBuffer body(1536 * 1024);
  F1Data &d = *net;
  const time_t now = time(nullptr);
  bool ok = true, changed = false;
  // each piece is shown as soon as it arrives: calendar first, then standings, forecast, circuit
  auto publish = [&]() {
    ++d.version;
    xSemaphoreTake(lock, portMAX_DELAY); memcpy(shared, &d, sizeof d); xSemaphoreGive(lock);
    changed = false;
  };
  if (!d.schedMs || millis() - d.schedMs > 24UL * 3600UL * 1000UL) {
    if (fetchSchedule(body)) publish(); else return false;          // no calendar: nothing else matters yet
  }
  if (!screenJobStillCurrent()) return false;
  // standings: every 6 h, or every run (15 min) while a finished race has no published result yet
  int lastRound = 0; time_t lastT = 0;
  for (int i = 0; i < d.nRaces; ++i) if (d.races[i].t[S_RACE] && d.races[i].t[S_RACE] + 6000 < now) { lastRound = d.races[i].round; lastT = d.races[i].t[S_RACE]; }
  const bool waiting = lastRound > d.resultRound && now - lastT < 3 * 86400;
  if (!d.standMs || millis() - d.standMs > 6UL * 3600UL * 1000UL || waiting) { if (fetchStandings(body)) publish(); else ok = false; }
  // forecast for the next weekend: once it is within 15 days, refreshed every 3 h
  for (int i = 0; i < d.nRaces; ++i) {
    const Race &r = d.races[i];
    if (r.t[S_RACE] + 7200 <= now) continue;
    if (r.t[S_RACE] - now < 15L * 86400 && (strcmp(d.wxCid, r.cid) || millis() - d.wxMs > 3UL * 3600UL * 1000UL) &&
        screenJobStillCurrent()) {
      if (fetchRaceWeather(d, r, body)) changed = true;
    }
    break;
  }
  if (changed) publish();
  // circuit drawing for the circuit on screen
  char cid[20] = "";
  xSemaphoreTake(lock, portMAX_DELAY); copyStr(mapWanted, cid, sizeof cid); xSemaphoreGive(lock);   // what the screen shows
  for (int i = 0; i < d.nRaces && !cid[0]; ++i) if (d.races[i].t[S_RACE] + 7200 > now) copyStr(d.races[i].cid, cid, sizeof cid);
  if (!cid[0] && d.nRaces) copyStr(d.races[d.nRaces - 1].cid, cid, sizeof cid);   // season over: last circuit
  if (!cid[0]) { xSemaphoreTake(lock, portMAX_DELAY); copyStr(mapWanted, cid, sizeof cid); xSemaphoreGive(lock); }
  LogV.printf("F1: circuito na tela: %s\n", cid[0] ? cid : "-");
  if (cid[0] && fsOk() && screenJobStillCurrent()) {
    char path[40]; pathFor(path, sizeof path, 't', cid);
    static uint32_t tracksTriedMs;
    if (!LittleFS.exists(path) && (!tracksTriedMs || millis() - tracksTriedMs > 120000UL)) {
      tracksTriedMs = millis();
      if (fetchTracks(body)) ++mapVersion; else ok = false;
    }
    // street map picture (only for the "Ruas" style; "Pista" is the track alone on black)
    char tp[40]; pathFor(tp, sizeof tp, 't', cid);
    const uint8_t style = mapStyle;
    char ip[40]; imagePath(ip, sizeof ip, style, cid);
    if (style == ST_STREET && LittleFS.exists(tp) && !LittleFS.exists(ip) && screenJobStillCurrent()) {
      if (fetchImage(cid, style, body)) ++mapVersion; else ok = false;
    }
  }
  return ok;
}

}  // namespace f1
