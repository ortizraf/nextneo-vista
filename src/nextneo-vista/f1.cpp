// Formula 1 screen ("Fórmula 1").
//
// Pages (tabs at the top, or swipe):  Próxima  |  Pilotos  |  Equipes
//  * Próxima: next session (TL1, TL2, TL3, Sprint quali, Sprint, Quali, Race) in the device's
//    time zone + countdown, and a big drawing of the circuit with its surroundings
//    (lakes, sea, parks, big buildings from OpenStreetMap) and famous landmarks / corners.
//    Tap the left side = full weekend programme; tap the map = corner names on/off.
//  * Pilotos / Equipes: championship standings (team colour bar, points, gap).
// Automatic mode: from the chequered flag (results published) until midnight it opens on the
// standings with the podium; after the last race of the season it shows the champions.
//
// Data (all free, no key):
//  - Jolpica-F1 (api.jolpi.ca, Ergast successor): calendar with every session, last race
//    podium, driver / constructor standings.
//  - bacinger/f1-circuits (GitHub, MIT): track outlines, downloaded once, cached in LittleFS.
//  - OpenStreetMap / Overpass: what is around each circuit, once per circuit, cached.
// Times come in UTC and are shown with cfg.utcOffset (from Open-Meteo, i.e. from lat/lon).
#include "app_screens.h"
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

lv_obj_t *f1View;

// ============================================================== static tables
struct CircuitInfo {
  const char *cid, *track, *gp, *country;   // Jolpica circuitId, bacinger id, GP name, country (pt)
};
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
static const CircuitInfo *circuitInfo(const char *cid) {
  for (const auto &c : kCircuits) if (!strcmp(c.cid, cid)) return &c;
  return nullptr;
}

// Famous corners, as a fraction of the lap from the start line (race direction).
struct Corner { const char *cid; float frac; const char *name; };
static const Corner kCorners[] = {
  {"interlagos", 0.05f, "S do Senna"}, {"interlagos", 0.30f, "Descida do Lago"}, {"interlagos", 0.43f, "Ferradura"},
  {"interlagos", 0.70f, "Mergulho"}, {"interlagos", 0.77f, "Jun\xC3\xA7\xC3\xA3o"}, {"interlagos", 0.88f, "Subida dos Boxes"},
  {"monaco", 0.05f, "Sainte-D\xC3\xA9vote"}, {"monaco", 0.22f, "Cassino"}, {"monaco", 0.33f, "Grand Hotel"},
  {"monaco", 0.45f, "T\xC3\xBAnel"}, {"monaco", 0.74f, "Piscina"}, {"monaco", 0.88f, "Rascasse"},
  {"spa", 0.03f, "La Source"}, {"spa", 0.08f, "Eau Rouge"}, {"spa", 0.28f, "Les Combes"}, {"spa", 0.50f, "Pouhon"},
  {"spa", 0.80f, "Blanchimont"}, {"spa", 0.94f, "Bus Stop"},
  {"monza", 0.11f, "Rettifilo"}, {"monza", 0.37f, "Lesmo"}, {"monza", 0.63f, "Ascari"}, {"monza", 0.88f, "Parabolica"},
  {"suzuka", 0.10f, "Esses"}, {"suzuka", 0.30f, "Degner"}, {"suzuka", 0.58f, "Spoon"}, {"suzuka", 0.80f, "130R"},
  {"silverstone", 0.08f, "Copse"}, {"silverstone", 0.18f, "Maggotts/Becketts"}, {"silverstone", 0.42f, "Stowe"},
  {"silverstone", 0.80f, "Luffield"},
  {"zandvoort", 0.05f, "Tarzan"}, {"zandvoort", 0.20f, "Hugenholtz"}, {"zandvoort", 0.93f, "Arie Luyendyk"},
  {"villeneuve", 0.97f, "Muro dos Campe\xC3\xB5" "es"},
  {"baku", 0.45f, "Cidade Velha"},
  {"rodriguez", 0.86f, "Foro Sol"},
  {"red_bull_ring", 0.20f, "Remus"},
  {"americas", 0.05f, "Curva 1"},
};

// Landmarks that are not shapes on the map: drawn as small icons at their coordinates.
enum { LM_WHEEL, LM_SPHERE, LM_STADIUM, LM_TOWER, LM_BOATS, LM_BUILDING };
struct Landmark { const char *cid; float lat, lon; uint8_t icon; const char *name; };
static const Landmark kLandmarks[] = {
  {"vegas", 36.1208f, -115.1617f, LM_SPHERE, "Sphere"},
  {"marina_bay", 1.2893f, 103.8631f, LM_WHEEL, "Roda-gigante"},
  {"marina_bay", 1.2834f, 103.8607f, LM_BUILDING, "Marina Bay"},
  {"suzuka", 34.8459f, 136.5376f, LM_WHEEL, "Roda-gigante"},
  {"monaco", 43.7395f, 7.4278f, LM_BUILDING, "Cassino"},
  {"monaco", 43.7353f, 7.4232f, LM_BOATS, "Porto"},
  {"yas_marina", 24.4676f, 54.6036f, LM_BUILDING, "Hotel"},
  {"yas_marina", 24.4650f, 54.6080f, LM_BOATS, "Marina"},
  {"rodriguez", 19.4050f, -99.0894f, LM_STADIUM, "Est\xC3\xA1" "dio"},
  {"miami", 25.9580f, -80.2389f, LM_STADIUM, "Est\xC3\xA1" "dio"},
  {"baku", 40.3665f, 49.8355f, LM_TOWER, "Cidade Velha"},
  {"silverstone", 52.0716f, -1.0162f, LM_BUILDING, "Wing"},
  {"albert_park", -37.8440f, 144.9700f, LM_BOATS, "Lago"},
};

// Kerb colours = colours of the host country's flag
struct KerbColors { const char *cid; uint32_t col[3]; };
static const KerbColors kKerbs[] = {
  {"interlagos", {0x009C3B, 0xFFDF00, 0}}, {"monaco", {0xE1251B, 0xFFFFFF, 0}}, {"monza", {0x009246, 0xFFFFFF, 0xCE2B37}},
  {"imola", {0x009246, 0xFFFFFF, 0xCE2B37}}, {"spa", {0x1A1A1A, 0xFDDA24, 0xEF3340}}, {"silverstone", {0xC8102E, 0xFFFFFF, 0x2B4EA2}},
  {"zandvoort", {0xAE1C28, 0xFFFFFF, 0x21468B}}, {"catalunya", {0xAA151B, 0xF1BF00, 0}}, {"madring", {0xAA151B, 0xF1BF00, 0}},
  {"red_bull_ring", {0xC8102E, 0xFFFFFF, 0}}, {"hungaroring", {0xCE2939, 0xFFFFFF, 0x477050}}, {"villeneuve", {0xD52B1E, 0xFFFFFF, 0}},
  {"americas", {0xB22234, 0xFFFFFF, 0x3C3B6E}}, {"miami", {0xB22234, 0xFFFFFF, 0x3C3B6E}}, {"vegas", {0xB22234, 0xFFFFFF, 0x3C3B6E}},
  {"rodriguez", {0x006847, 0xFFFFFF, 0xCE1126}}, {"albert_park", {0x2B4EA2, 0xFFFFFF, 0xE4002B}}, {"shanghai", {0xEE1C25, 0xFFDE00, 0}},
  {"suzuka", {0xBC002D, 0xFFFFFF, 0}}, {"baku", {0x00B5E2, 0xEF3340, 0x509E2F}}, {"marina_bay", {0xEF3340, 0xFFFFFF, 0}},
  {"sepang", {0xCC0001, 0xFFFFFF, 0x2B3A9C}}, {"losail", {0x8A1538, 0xFFFFFF, 0}}, {"yas_marina", {0x00732F, 0xFFFFFF, 0xEF3340}},
  {"bahrain", {0xCE1126, 0xFFFFFF, 0}}, {"jeddah", {0x006C35, 0xFFFFFF, 0}},
};

// Start / finish line positions known to be on the main straight
struct StartLine { const char *cid; float lat, lon; };
static const StartLine kStartLine[] = {
  {"vegas", 36.10794f, -115.16650f},        // south straight (pit building)
  {"monaco", 43.73470f, 7.42056f},          // Boulevard Albert 1er
  {"interlagos", -23.70360f, -46.69970f},   // main straight
};

struct TeamColor { const char *id; uint32_t col; };
static const TeamColor kTeams[] = {
  {"mclaren", 0xFF8000}, {"ferrari", 0xE8002D}, {"red_bull", 0x3671C6}, {"mercedes", 0x27F4D2},
  {"aston_martin", 0x229971}, {"alpine", 0x00A1E8}, {"williams", 0x64C4FF}, {"rb", 0x6692FF},
  {"sauber", 0x52E252}, {"audi", 0xBB0A30}, {"haas", 0xB6BABD}, {"cadillac", 0xD4AF37},
};
static uint32_t teamColor(const char *id) {
  for (const auto &t : kTeams) if (!strcmp(t.id, id)) return t.col;
  return 0x8090A0;
}

// ============================================================== data model
enum { S_FP1, S_FP2, S_FP3, S_SQ, S_SPRINT, S_QUALI, S_RACE, S_COUNT };
static const char *kSession[S_COUNT] = {"TL1", "TL2", "TL3", "Classif. Sprint", "Sprint", "Classifica\xC3\xA7\xC3\xA3o", "CORRIDA"};
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
static F1Data *net;                                       // built by the network task (PSRAM)
static F1Data *shared;                                    // last complete copy, under `lock`
static F1Data *ui;                                        // UI thread copy
static SemaphoreHandle_t lock;
static volatile uint32_t mapVersion;                      // bumped when a circuit file is written
static char mapWanted[20];                                // circuit the UI shows (network task fetches its map)

static time_t timegmSimple(int y, int mo, int d, int h, int mi, int s) {
  y -= mo <= 2; const int era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + (long)doe - 719468L;
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + s);
}
static time_t parseUtc(const char *date, const char *tm) {    // "2026-03-08", "04:00:00Z"
  if (!date || strlen(date) < 10) return 0;
  int y, mo, d, h = 12, mi = 0, s = 0;
  if (sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3) return 0;
  if (tm) sscanf(tm, "%d:%d:%d", &h, &mi, &s);
  return timegmSimple(y, mo, d, h, mi, s);
}
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
// Big work arrays live in PSRAM: internal RAM must stay free for the TLS handshakes
// (mbedTLS needs one ~40-60 KB contiguous block of internal RAM).
template <class T> static T *psram(T *&p, size_t n) {
  if (!p) p = (T *)heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p;
}
static void copyStr(const char *s, char *out, size_t n) { snprintf(out, n, "%s", s ? s : ""); }

// ============================================================== network task
static char netMsg[72];                                   // last network problem, shown while nothing is loaded
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

// ---- circuit files (LittleFS):  /f1/t_<cid>.bin track,  /f1/m_<cid>.bin surroundings
// Coordinates are metres (int16) east/north of the track centre (lat0, lon0).
struct TrackHdr { uint32_t magic; float lat0, lon0; uint16_t lengthM, n; };
static constexpr uint32_t T_MAGIC = 0x31543146, M_MAGIC = 0x314D3146;   // "F1T1", "F1M1"
enum { K_GREEN, K_SAND, K_WATER, K_BUILD, K_STAND, K_WLINE, K_PIT, K_CORNER, K_COAST, K_ROAD, K_STREET, K_ALT, K_PATH, K_PARKING, K_COUNT };
// K_ALT: other tracks (kart, old layouts) in grey; K_PATH: footpaths / service roads; K_PARKING: car parks
// K_CORNER: named piece of track; K_COAST: sea shore (water on its right); K_ROAD/K_STREET: big / small streets

static bool fsOk() { if (!photosFsReady()) return false; if (!LittleFS.exists("/f1")) LittleFS.mkdir("/f1"); return true; }
static void pathFor(char *out, size_t n, char kind, const char *cid) { snprintf(out, n, "/f1/%c_%s.bin", kind, cid); }

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

static bool loadTrackHdr(const char *cid, TrackHdr &h, int16_t *pts, int maxPts) {
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
enum { ST_STREET, ST_DRAWN, ST_COUNT, ST_SAT = 9 };      // button cycles streets / drawing (satellite kept in code, unused)
static volatile uint8_t mapStyle = ST_STREET;
static constexpr int MAP_W = 284, MAP_H = 274;
static void imagePath(char *out, size_t n, uint8_t style, const char *cid) { pathFor(out, n, style == ST_SAT ? 's' : 'r', cid); }

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

// ============================================================== UI: raster helpers (RGB565 map buffer)
static uint16_t *mapBuf;
static inline uint16_t c565(uint32_t c) { return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3)); }
static inline void blend(int x, int y, uint32_t col, float a) {
  if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H || a <= 0) return;
  uint16_t &d = mapBuf[y * MAP_W + x];
  const int ia = (int)((a > 1 ? 1 : a) * 256);
  const int sr = (col >> 16) & 0xFF, sg = (col >> 8) & 0xFF, sb = col & 0xFF;
  const int dr = (d >> 11) << 3, dg = ((d >> 5) & 0x3F) << 2, db = (d & 0x1F) << 3;
  const int r = (sr * ia + dr * (256 - ia)) >> 8, g = (sg * ia + dg * (256 - ia)) >> 8, b = (sb * ia + db * (256 - ia)) >> 8;
  d = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void disc(float cx, float cy, float r, uint32_t col, float alpha) {
  for (int y = (int)floorf(cy - r - 1); y <= (int)ceilf(cy + r + 1); ++y)
    for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
      if (cov > 0) blend(x, y, col, (cov > 1 ? 1 : cov) * alpha);
    }
}
static void glow(float cx, float cy, float R, uint32_t col, float alpha) {
  for (int y = (int)(cy - R); y <= (int)(cy + R); ++y)
    for (int x = (int)(cx - R); x <= (int)(cx + R); ++x) {
      const float t = 1 - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / R;
      if (t > 0) blend(x, y, col, t * t * alpha);
    }
}
static void line(float x0, float y0, float x1, float y1, uint32_t col, float alpha) {   // 1 px anti-aliased
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) return;
  const int steps = (int)(len * 1.5f) + 1;
  for (int i = 0; i <= steps; ++i) {
    const float x = x0 + dx * i / steps, y = y0 + dy * i / steps;
    const int xi = (int)floorf(x), yi = (int)floorf(y); const float fx = x - xi, fy = y - yi, a = alpha / 1.5f;
    blend(xi, yi, col, (1 - fx) * (1 - fy) * a); blend(xi + 1, yi, col, fx * (1 - fy) * a);
    blend(xi, yi + 1, col, (1 - fx) * fy * a); blend(xi + 1, yi + 1, col, fx * fy * a);
  }
}
// thick anti-aliased stroke (distance to the segment)
static void thick(float x0, float y0, float x1, float y1, float r, uint32_t col, float alpha) {
  const float dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
  const int xa = (int)floorf(fminf(x0, x1) - r - 1), xb = (int)ceilf(fmaxf(x0, x1) + r + 1);
  const int ya = (int)floorf(fminf(y0, y1) - r - 1), yb = (int)ceilf(fmaxf(y0, y1) + r + 1);
  for (int y = ya; y <= yb; ++y)
    for (int x = xa; x <= xb; ++x) {
      const float px = x + 0.5f - x0, py = y + 0.5f - y0;
      float t = L2 > 0 ? (px * dx + py * dy) / L2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
      const float ex = px - t * dx, ey = py - t * dy, cov = r + 0.5f - sqrtf(ex * ex + ey * ey);
      if (cov > 0) blend(x, y, col, (cov > 1 ? 1 : cov) * alpha);
    }
}
static void rect(int x0, int y0, int x1, int y1, uint32_t col, float a) {
  for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) blend(x, y, col, a);
}

// ============================================================== UI: circuit drawing
static float mScale = 1, mCx, mCy;                       // metres -> map pixels
static inline float mx(float x) { return MAP_W / 2.0f + (x - mCx) * mScale; }
static inline float my(float y) { return MAP_H / 2.0f - (y - mCy) * mScale; }

static uint8_t *kindMask;                                  // MAP_W x MAP_H: which kind of area each pixel is (0xFF = none)
static uint8_t maskKind = 0xFF;                            // kind written by fillRings (0xFF = do not write)
static void fillRings(const int16_t *const *rings, const uint16_t *ns, int nr, uint32_t col, float alpha) {
  float ya = 1e9, yb = -1e9;
  for (int r = 0; r < nr; ++r) for (int i = 0; i < ns[r]; ++i) { const float y = my(rings[r][2 * i + 1]); ya = fminf(ya, y); yb = fmaxf(yb, y); }
  const int y0 = imax(0, (int)ya), y1 = imin(MAP_H - 1, (int)yb + 1);
  static float *xs; if (!psram(xs, 512)) return;
  for (int y = y0; y <= y1; ++y) {                       // even-odd scanline fill
    const float sy = y + 0.5f; int k = 0;
    for (int r = 0; r < nr; ++r)
      for (int i = 0, j = ns[r] - 1; i < ns[r]; j = i++) {
        const float yi = my(rings[r][2 * i + 1]), yj = my(rings[r][2 * j + 1]);
        if ((yi > sy) != (yj > sy) && k < 512) {
          const float xi = mx(rings[r][2 * i]), xj = mx(rings[r][2 * j]);
          xs[k++] = xi + (sy - yi) / (yj - yi) * (xj - xi);
        }
      }
    for (int a = 1; a < k; ++a) { const float v = xs[a]; int b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; --b; } xs[b + 1] = v; }
    for (int a = 0; a + 1 < k; a += 2) {
      const int xa = imax(0, (int)ceilf(xs[a] - 0.5f)), xb = imin(MAP_W - 1, (int)floorf(xs[a + 1] - 0.5f));
      for (int x = xa; x <= xb; ++x) { blend(x, y, col, alpha); if (kindMask && maskKind != 0xFF) kindMask[y * MAP_W + x] = maskKind; }
    }
  }
  for (int r = 0; r < nr; ++r)                          // soft edge
    for (int i = 0, j = ns[r] - 1; i < ns[r]; j = i++)
      line(mx(rings[r][2 * j]), my(rings[r][2 * j + 1]), mx(rings[r][2 * i]), my(rings[r][2 * i + 1]), col, alpha * 0.6f);
}

// corner names that OpenStreetMap gives for this circuit (named pieces of highway=raceway)
static char osmCornerName[20][42]; static float osmCornerX[20], osmCornerY[20]; static int osmCornerN;
static float pitX, pitY; static bool havePit;

// Illustrated "site plan" look (light): cream ground, soft green parks with trees, blue water
// (sea filled from the shore line), white streets with grey casing, white buildings with a
// shadow (a little 3D), blue grandstands. Everything from OpenStreetMap.
static void floodSea(int sx, int sy, uint16_t col) {      // fill the sea from a seed, stopping at the shore line
  if (sx < 0 || sy < 0 || sx >= MAP_W || sy >= MAP_H || kindMask[sy * MAP_W + sx] != 0xFF) return;
  static int16_t *stk; static constexpr int STK = 60000;
  if (!stk) stk = (int16_t *)heap_caps_malloc(STK * 4, MALLOC_CAP_SPIRAM);
  if (!stk) return;
  int n = 0; stk[0] = sx; stk[1] = sy; n = 1;
  while (n) {
    --n; int x = stk[2 * n], y = stk[2 * n + 1];
    while (x > 0 && kindMask[y * MAP_W + x - 1] == 0xFF) --x;
    bool up = false, dn = false;
    for (; x < MAP_W && kindMask[y * MAP_W + x] == 0xFF; ++x) {
      kindMask[y * MAP_W + x] = K_WATER; mapBuf[y * MAP_W + x] = col;
      if (y > 0) { const bool f = kindMask[(y - 1) * MAP_W + x] == 0xFF; if (f && !up && n < STK) { stk[2 * n] = x; stk[2 * n + 1] = y - 1; ++n; } up = f; }
      if (y < MAP_H - 1) { const bool f = kindMask[(y + 1) * MAP_W + x] == 0xFF; if (f && !dn && n < STK) { stk[2 * n] = x; stk[2 * n + 1] = y + 1; ++n; } dn = f; }
    }
  }
}

static void drawSurroundings(const char *cid, bool pitPass, bool paint = true) {
  char path[40]; pathFor(path, sizeof path, 'v', cid);
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  const size_t sz = f.size();
  uint8_t *b = (uint8_t *)heap_caps_malloc(sz + 4, MALLOC_CAP_SPIRAM);
  if (!b) { f.close(); return; }
  f.read(b, sz); f.close();
  uint32_t mg; uint16_t count; memcpy(&mg, b, 4); memcpy(&count, b + 4, 2);
  if (mg != M_MAGIC) { heap_caps_free(b); return; }
  if (!kindMask) kindMask = (uint8_t *)heap_caps_malloc(MAP_W * MAP_H, MALLOC_CAP_SPIRAM);
  if (!pitPass) { osmCornerN = 0; havePit = false; if (kindMask) memset(kindMask, 0xFF, MAP_W * MAP_H); }
  static const uint32_t C_GREEN = 0xBFD8A0, C_SAND = 0xEAD9A6, C_WATER = 0x9CCFEA, C_WATER_EDGE = 0x5FA7CF,
                        C_BUILD = 0xFBFBF8, C_BUILD_EDGE = 0xA9ADB3, C_SHADOW = 0x8A8F86, C_STAND = 0x6F9FD8,
                        C_ROAD = 0xFFFFFF, C_CASING = 0xB9BCC0;
  // passes: 0 green, 1 sand, 2 water, 3 sea, 4 streets casing, 5 streets, 6 rivers, 7 building shadows,
  //         8 buildings, 9 stands  |  pit pass: pit lane
  const int firstPass = pitPass ? 20 : 0, lastPass = pitPass ? 20 : 9;
  for (int pass = firstPass; pass <= lastPass; ++pass) {
    size_t o = 6;
    for (int sIdx = 0; sIdx < count && o + 2 <= sz; ++sIdx) {
      const uint8_t kind = b[o], nr = b[o + 1]; o += 2;
      const int16_t *rings[60]; uint16_t ns[60]; int k = 0;
      for (int r = 0; r < nr && o + 2 <= sz; ++r) {
        uint16_t n; memcpy(&n, b + o, 2); o += 2;
        if (k < 60) { rings[k] = (const int16_t *)(b + o); ns[k] = n; ++k; }
        o += n * 4;
      }
      const char *name = nullptr; uint8_t nlen = 0;
      if (kind == K_CORNER && o < sz) { nlen = b[o]; name = (const char *)(b + o + 1); o += 1 + nlen + (((nlen + 1) & 1) ? 1 : 0); }
      if (kind == K_CORNER && !pitPass && pass == 0 && k && osmCornerN < 20 && nlen) {
        const int m = ns[0] / 2;                          // middle of that piece of track
        memcpy(osmCornerName[osmCornerN], name, nlen); osmCornerName[osmCornerN][nlen] = 0;
        osmCornerX[osmCornerN] = rings[0][2 * m]; osmCornerY[osmCornerN] = rings[0][2 * m + 1]; ++osmCornerN;
      }
      if (kind == K_PIT && !havePit && k) { const int m = ns[0] / 2; pitX = rings[0][2 * m]; pitY = rings[0][2 * m + 1]; havePit = true; }
      if (!paint) continue;
      auto polyline = [&](float r, uint32_t col, float a) {
        for (int q = 0; q < k; ++q) for (int i = 1; i < ns[q]; ++i)
          thick(mx(rings[q][2 * i - 2]), my(rings[q][2 * i - 1]), mx(rings[q][2 * i]), my(rings[q][2 * i + 1]), r, col, a);
      };
      auto outline = [&](uint32_t col, float a, float ox = 0, float oy = 0) {
        for (int q = 0; q < k; ++q) for (int i = 0, j = ns[q] - 1; i < ns[q]; j = i++)
          line(mx(rings[q][2 * j]) + ox, my(rings[q][2 * j + 1]) + oy, mx(rings[q][2 * i]) + ox, my(rings[q][2 * i + 1]) + oy, col, a);
      };
      switch (pass) {
        case 0: if (kind == K_GREEN) { maskKind = K_GREEN; fillRings(rings, ns, k, C_GREEN, 1); maskKind = 0xFF; } break;
        case 1:
          if (kind == K_SAND) fillRings(rings, ns, k, C_SAND, 1);
          else if (kind == K_PARKING) { fillRings(rings, ns, k, 0xE4E5E7, 1); outline(0xC8CBCF, 0.8f); }
          break;
        case 2: if (kind == K_WATER) { maskKind = K_WATER; fillRings(rings, ns, k, C_WATER, 1); maskKind = 0xFF; outline(C_WATER_EDGE, 0.8f); } break;
        case 3:
          if (kind == K_COAST && kindMask) {
            // mark the shore in the mask, then flood the sea from points just to its right (OSM: water on the right)
            for (int q = 0; q < k; ++q) for (int i = 1; i < ns[q]; ++i) {
              const float x0 = mx(rings[q][2 * i - 2]), y0 = my(rings[q][2 * i - 1]), x1 = mx(rings[q][2 * i]), y1 = my(rings[q][2 * i + 1]);
              const int st = (int)(hypotf(x1 - x0, y1 - y0) * 2) + 1;
              for (int t = 0; t <= st; ++t) {
                const int x = (int)(x0 + (x1 - x0) * t / st), y = (int)(y0 + (y1 - y0) * t / st);
                if (x >= 0 && y >= 0 && x < MAP_W && y < MAP_H) kindMask[y * MAP_W + x] = K_COAST;
              }
            }
            for (int q = 0; q < k; ++q) for (int i = 1; i < ns[q]; ++i) {
              const float x0 = mx(rings[q][2 * i - 2]), y0 = my(rings[q][2 * i - 1]), x1 = mx(rings[q][2 * i]), y1 = my(rings[q][2 * i + 1]);
              const float L = hypotf(x1 - x0, y1 - y0);
              if (L < 2) continue;
              // screen y points down, so "right of the direction" in map terms = (-dy, dx) on screen
              const float rx = -(y1 - y0) / L, ry = (x1 - x0) / L;
              floodSea((int)((x0 + x1) / 2 + rx * 3), (int)((y0 + y1) / 2 + ry * 3), c565(C_WATER));
            }
            polyline(0.8f, C_WATER_EDGE, 1);
          }
          break;
        case 4: if (kind == K_ROAD) polyline(3.2f, C_CASING, 1); else if (kind == K_STREET) polyline(2.0f, C_CASING, 1); break;
        case 5:
          if (kind == K_ROAD) polyline(2.3f, C_ROAD, 1);
          else if (kind == K_STREET) polyline(1.2f, C_ROAD, 1);
          else if (kind == K_PATH) polyline(0.6f, 0xFFFFFF, 0.85f);                       // footpaths in the parks
          else if (kind == K_ALT) { polyline(2.4f, 0x8E9298, 1); polyline(1.6f, 0xB5B9BE, 1); }   // kart track, old layouts
          break;
        case 6: if (kind == K_WLINE) polyline(1.4f, C_WATER_EDGE, 1); break;
        case 7: if (kind == K_BUILD) fillRings(rings, ns, k, C_SHADOW, 0.35f); break;     // shadow drawn at the same place,
        case 8:                                            // building raised 2 px up-left = a little 3D
          if (kind == K_BUILD) {
            for (int q = 0; q < k; ++q) {
              static int16_t *tmp; if (!psram(tmp, 2 * 1500)) continue;
              const int n = ns[q] < 1500 ? ns[q] : 1500;
              const int dx = (int)lroundf(-2 / mScale), dy = (int)lroundf(2 / mScale);
              for (int i = 0; i < n; ++i) { tmp[2 * i] = rings[q][2 * i] + dx; tmp[2 * i + 1] = rings[q][2 * i + 1] + dy; }
              const int16_t *rr[1] = {tmp}; const uint16_t nn[1] = {(uint16_t)n};
              // roofs in soft varied colours (like an illustrated map), picked from the building position
              static const uint32_t kRoof[6] = {C_BUILD, 0xF3E3CC, 0xEBC7A4, 0xD8E1EA, 0xF0DBAE, 0xE4D2DF};
              static const uint32_t kEdge[6] = {C_BUILD_EDGE, 0xC2AE90, 0xB98E6C, 0x9EA9B5, 0xBFA374, 0xAF98AA};
              const uint32_t hsh = (uint32_t)(tmp[0] * 2654435761u) ^ (uint32_t)(tmp[1] * 40503u);
              const int c = (hsh >> 7) % 6;
              maskKind = K_BUILD; fillRings(rr, nn, 1, kRoof[c], 1); maskKind = 0xFF;
              for (int i = 0, j = n - 1; i < n; j = i++) line(mx(tmp[2 * j]), my(tmp[2 * j + 1]), mx(tmp[2 * i]), my(tmp[2 * i + 1]), kEdge[c], 0.9f);
            }
          }
          break;
        case 9: if (kind == K_STAND) { fillRings(rings, ns, k, C_STAND, 1); outline(0xFFFFFF, 0.9f); } break;
        case 20:
          if (kind == K_PIT) { polyline(2.6f, 0x8C9096, 1); polyline(1.8f, 0x5A5E66, 1); }
          break;
      }
    }
    if (pass == 0 && kindMask) {                         // trees on the green areas (deterministic scatter)
      for (int y = 3; y < MAP_H - 3; y += 6)
        for (int x = 3 + (y / 6 % 2) * 3; x < MAP_W - 3; x += 6) {
          const uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u);
          const int tx = x + (int)(hsh % 3) - 1, ty = y + (int)(hsh / 3 % 3) - 1;
          if (kindMask[ty * MAP_W + tx] != K_GREEN || (hsh >> 8) % 3 == 0) continue;
          disc(tx + 0.8f, ty + 0.8f, 2.0f, 0x5E7F4A, 0.45f);            // shadow
          disc(tx, ty, 2.0f, 0x6FA05A, 1);                              // crown
          disc(tx - 0.6f, ty - 0.6f, 0.9f, 0x9CC77E, 0.9f);             // light
        }
    }
  }
  heap_caps_free(b);
}

static void drawIcon(float x, float y, uint8_t icon) {
  switch (icon) {
    case LM_WHEEL:
      for (int a = 0; a < 360; a += 8) { const float r = a * 0.01745f; blend(x + 7 * cosf(r), y + 7 * sinf(r), 0xE8EEF8, 1); }
      for (int a = 0; a < 180; a += 30) { const float r = a * 0.01745f; line(x - 7 * cosf(r), y - 7 * sinf(r), x + 7 * cosf(r), y + 7 * sinf(r), 0xB8C4D8, 0.8f); }
      line(x, y, x - 4, y + 11, 0xB8C4D8, 1); line(x, y, x + 4, y + 11, 0xB8C4D8, 1);
      break;
    case LM_SPHERE:
      glow(x, y, 14, 0x6FA8FF, 0.5f); disc(x, y, 6.5f, 0x3C7BE0, 1); disc(x - 2, y - 2, 2.5f, 0xBFD8FF, 0.8f); break;
    case LM_STADIUM:
      for (int a = 0; a < 360; a += 4) { const float r = a * 0.01745f; thick(x + 8 * cosf(r), y + 5 * sinf(r), x + 8 * cosf(r + 0.07f), y + 5 * sinf(r + 0.07f), 1.2f, 0xD0D6E0, 1); }
      disc(x, y, 3, 0x2E7D32, 1); break;
    case LM_TOWER:
      rect((int)x - 3, (int)y - 9, (int)x + 3, (int)y + 6, 0xC9B48A, 1);
      for (int k = -3; k < 3; k += 2) rect((int)x + k, (int)y - 11, (int)x + k + 1, (int)y - 9, 0xC9B48A, 1); break;
    case LM_BOATS:
      for (int k = 0; k < 3; ++k) { const float bx = x - 7 + k * 6, by = y + (k % 2) * 3; line(bx - 2, by, bx + 2, by, 0xFFFFFF, 1); line(bx, by, bx, by - 5, 0xFFFFFF, 0.9f); line(bx, by - 5, bx + 2, by - 1, 0xFFFFFF, 0.7f); }
      break;
    default:
      rect((int)x - 5, (int)y - 8, (int)x + 5, (int)y + 6, 0xC8CED8, 1);
      for (int r = -6; r < 5; r += 3) rect((int)x - 4, (int)y + r, (int)x + 4, (int)y + r + 1, 0x7A8496, 1);
  }
}

// Track points rotated so that index 0 = start line (closest point to the Jolpica location).
static int16_t *trk; static int trkN; static float trkLen;
static bool showCorners;
struct MapLabel { int x, y; const char *t; uint32_t col; };
static constexpr int MAX_LABELS = 48;
static MapLabel mapLabels[MAX_LABELS]; static int mapLabelN;

static void trackPoint(float frac, float &x, float &y, float &ux, float &uy) {   // position at a fraction of the lap
  float total = 0;
  for (int i = 0; i < trkN; ++i) { const int j = (i + 1) % trkN; total += hypotf(trk[2 * j] - trk[2 * i], trk[2 * j + 1] - trk[2 * i + 1]); }
  float want = frac * total, acc = 0;
  for (int i = 0; i < trkN; ++i) {
    const int j = (i + 1) % trkN;
    const float sx = trk[2 * j] - trk[2 * i], sy = trk[2 * j + 1] - trk[2 * i + 1], L = hypotf(sx, sy);
    if (acc + L >= want && L > 0) {
      const float t = (want - acc) / L;
      x = mx(trk[2 * i] + sx * t); y = my(trk[2 * i + 1] + sy * t); ux = sx / L; uy = -sy / L; return;
    }
    acc += L;
  }
  x = mx(trk[0]); y = my(trk[1]); ux = 1; uy = 0;
}

// middle of the (longest) pit lane, in metres: the start / finish line is beside it
static bool pitMiddle(const char *cid, float &x, float &y) {
  char path[40]; pathFor(path, sizeof path, 'v', cid);
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  const size_t sz = f.size();
  uint8_t *b = (uint8_t *)heap_caps_malloc(sz + 4, MALLOC_CAP_SPIRAM);
  if (!b) { f.close(); return false; }
  f.read(b, sz); f.close();
  uint16_t count; memcpy(&count, b + 4, 2);
  size_t o = 6; int bestN = 0;
  for (int sIdx = 0; sIdx < count && o + 2 <= sz; ++sIdx) {
    const uint8_t kind = b[o], nr = b[o + 1]; o += 2;
    for (int r = 0; r < nr && o + 2 <= sz; ++r) {
      uint16_t n; memcpy(&n, b + o, 2); o += 2;
      if (kind == K_PIT && n > bestN && o + n * 4 <= sz) {
        bestN = n; int16_t xy[2]; memcpy(xy, b + o + (n / 2) * 4, 4); x = xy[0]; y = xy[1];
      }
      o += n * 4;
    }
    if (kind == K_CORNER && o < sz) { const uint8_t l = b[o]; o += 1 + l + (((l + 1) & 1) ? 1 : 0); }
  }
  heap_caps_free(b);
  return bestN > 0;
}

struct ImgTarget { uint16_t *out; uint8_t style; };
static int imgBlock(JPEGDRAW *d) {
  const ImgTarget *t = (const ImgTarget *)d->pUser;
  for (int row = 0; row < d->iHeight; ++row) {
    const int y = d->y + row;
    if (y < 0 || y >= MAP_H) continue;
    const uint16_t *src = d->pPixels + row * d->iWidth;
    for (int col = 0; col < d->iWidth; ++col) {
      const int x = d->x + col;
      if (x < 0 || x >= MAP_W) continue;
      const uint16_t p = src[col];
      int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
      if (t->style == ST_SAT) { r = r * 13 / 16; g = g * 13 / 16; b = b * 13 / 16; }   // a bit darker: overlays stand out
      else {                                              // street map in a dark slate tone
        const int v = 255 - ((r * 77 + g * 150 + b * 29) >> 8);
        r = 16 + v * 120 / 255; g = 20 + v * 130 / 255; b = 30 + v * 150 / 255;
      }
      t->out[y * MAP_W + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
  return 1;
}
static bool drawImage(const char *cid, uint8_t style) {
  if (style == ST_DRAWN) return false;
  char path[40]; imagePath(path, sizeof path, style, cid);
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  const size_t sz = f.size();
  uint8_t *jpg = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  void *mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
  bool ok = false;
  if (jpg && mem && f.read(jpg, sz) == sz) {
    JPEGDEC *dec = new (mem) JPEGDEC();
    ImgTarget t = {mapBuf, style};
    if (dec->openRAM(jpg, (int)sz, imgBlock)) {
      dec->setPixelType(RGB565_LITTLE_ENDIAN); dec->setUserPointer(&t);
      ok = dec->decode(0, 0, 0) == 1;
      dec->close();
    }
    dec->~JPEGDEC();
  }
  f.close();
  if (jpg) heap_caps_free(jpg);
  if (mem) heap_caps_free(mem);
  return ok;
}

static void renderMap(const Race *race) {
  mapLabelN = 0;
  // ground that matches the place: grass (parkland circuits), sand (desert), city blocks
  // (street circuits) or the city at night (Singapore, Las Vegas)
  enum { G_GRASS, G_DESERT, G_CITY, G_NIGHT };
  int ground = G_GRASS;
  if (race) {
    static const char *kDesert[] = {"bahrain", "jeddah", "losail", "yas_marina"};
    static const char *kCity[] = {"monaco", "baku", "miami", "madring"};
    static const char *kNight[] = {"marina_bay", "vegas"};
    for (const char *c : kDesert) if (!strcmp(c, race->cid)) ground = G_DESERT;
    for (const char *c : kCity) if (!strcmp(c, race->cid)) ground = G_CITY;
    for (const char *c : kNight) if (!strcmp(c, race->cid)) ground = G_NIGHT;
  }
  for (int i = 0; i < MAP_W * MAP_H; ++i) mapBuf[i] = 0;    // black until the circuit is loaded
  if (!race) return;
  TrackHdr h;
  if (!trk) trk = (int16_t *)heap_caps_malloc(2 * 2000 * 2, MALLOC_CAP_SPIRAM);
  if (!trk || !fsOk() || !loadTrackHdr(race->cid, h, trk, 2000)) { trkN = 0; return; }
  for (int i = 0; i < MAP_W * MAP_H; ++i) mapBuf[i] = 0;    // black: only the track
  trkN = h.n; trkLen = h.lengthM;
  if (trkN > 2 && trk[0] == trk[2 * (trkN - 1)] && trk[1] == trk[2 * trkN - 1]) --trkN;   // closed: drop the repeat
  // start line: a known position (table), else the track point beside the middle of the pit
  // lane (OpenStreetMap), else the outline's first point (the data set starts on the main straight)
  const double kx = cos(h.lat0 * M_PI / 180) * 111320.0, ky = 110540.0;
  float sx = 0, sy = 0, lim = 0;
  for (const auto &st : kStartLine)
    if (!strcmp(st.cid, race->cid)) { sx = (st.lon - h.lon0) * kx; sy = (st.lat - h.lat0) * ky; lim = 400; }
  if (!lim && pitMiddle(race->cid, sx, sy)) lim = 300;
  int best = 0; float bd = 1e18f;
  for (int i = 0; i < trkN; ++i) { const float d = hypotf(trk[2 * i] - sx, trk[2 * i + 1] - sy); if (d < bd) { bd = d; best = i; } }
  if (best && bd < lim) {                               // rotate the ring (only if that point is really on the track)
    int16_t *tmp = (int16_t *)heap_caps_malloc(trkN * 4, MALLOC_CAP_SPIRAM);
    if (tmp) { for (int i = 0; i < trkN; ++i) { tmp[2 * i] = trk[2 * ((i + best) % trkN)]; tmp[2 * i + 1] = trk[2 * ((i + best) % trkN) + 1]; }
               memcpy(trk, tmp, trkN * 4); heap_caps_free(tmp); }
  }
  int minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
  for (int i = 0; i < trkN; ++i) { minX = imin(minX, trk[2 * i]); maxX = imax(maxX, trk[2 * i]); minY = imin(minY, trk[2 * i + 1]); maxY = imax(maxY, trk[2 * i + 1]); }
  mCx = (minX + maxX) / 2.0f; mCy = (minY + maxY) / 2.0f;
  mScale = fminf((MAP_W - 26) / (float)imax(1, maxX - minX), (MAP_H - 26) / (float)imax(1, maxY - minY));
  // background: photo / street map when downloaded, otherwise our own drawing of the area
  const bool picture = drawImage(race->cid, mapStyle);   // "Ruas": street map; "Pista": black, track only
  const bool lightMap = false;                           // light labels on black
  osmCornerN = 0; havePit = false;
  // corners: walk the lap in 2 px steps; where the heading turns fast it is a corner. The outside
  // edge gets a run-off area (gravel, or asphalt on street/desert circuits) under the track and
  // kerbs in the colours of the host country's flag on top of it.
  struct CPt { float x, y, nx, ny; bool right, start; int k; };
  static CPt *cp; int cpN = 0;
  if (!psram(cp, 900)) return;
  {
    float lapPx = 0;
    for (int i = 0; i < trkN; ++i) { const int j = (i + 1) % trkN; lapPx += hypotf(mx(trk[2 * j]) - mx(trk[2 * i]), my(trk[2 * j + 1]) - my(trk[2 * i + 1])); }
    const int steps = imin(900, (int)(lapPx / 2.0f));
    int lastK = -100; bool lastRight = false;
    for (int k = 0; k < steps; ++k) {
      float xa, ya, uxa, uya, xb, yb, uxb, uyb, x, y, ux, uy;
      trackPoint((k - 3) / (float)steps + (k < 3 ? 1 : 0), xa, ya, uxa, uya);
      trackPoint((k + 3) % steps / (float)steps, xb, yb, uxb, uyb);
      trackPoint(k / (float)steps, x, y, ux, uy);
      const float turn = uxa * uyb - uya * uxb;                // sin of the heading change over ~12 px
      if (fabsf(turn) < 0.45f || k < 4 || k > steps - 4) continue;   // never on the start line
      const bool right = turn > 0;                           // screen y down: right-hand corner -> outside = left
      // a new corner only after a real straight bit (or when the turn changes side): chicanes
      // and long curves stay one corner instead of many tiny ones
      const bool start = k - lastK > 9 || right != lastRight;
      cp[cpN++] = {x, y, right ? uy : -uy, right ? -ux : ux, right, start, k};
      lastK = k; lastRight = right;
    }
  }
  const bool paved = ground != G_GRASS;
  for (int i = 0; i < cpN && false; ++i)                      // (no run-off on the black map)
    disc(cp[i].x + cp[i].nx * 7, cp[i].y + cp[i].ny * 7, 6.5f, paved ? 0xC4C8CE : 0xE2D3A0, 0.85f);
  // Night race (at the circuit's local time): the drawing goes dark blue and lights come on:
  // lit windows on the buildings, street lamps along the roads, floodlights along the track.
  bool night = false;
  if (false) {                                          // (night lights need the surroundings: off on the black map)
    // the race start at the circuit's local time: 17:00 - 06:00 = night race
    const long off = (ui && !strcmp(ui->wxCid, race->cid)) ? ui->wxOff : lroundf(race->lon / 15) * 3600L;
    const long lh = (((long)(race->t[S_RACE] % 86400) + off) % 86400 + 86400) % 86400 / 3600;
    night = lh < 6 || lh >= 17;
  }
  if (night) {
    for (int i = 0; i < MAP_W * MAP_H; ++i) {
      const uint16_t p = mapBuf[i];
      const bool road = p == 0xFFFF;                     // white street (before darkening)
      int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
      r = 6 + r * 22 / 100; g = 10 + g * 24 / 100; b = 26 + b * 30 / 100;
      if (road) { r = 58; g = 58; b = 66; }
      mapBuf[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      if (road && kindMask) kindMask[i] = K_ROAD;        // remember the streets for the lamps
    }
    if (kindMask)
      for (int y = 1; y < MAP_H - 1; ++y)
        for (int x = 1; x < MAP_W - 1; ++x) {
          const uint8_t k = kindMask[y * MAP_W + x];
          const uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u);
          if (k == K_BUILD && hsh % 13 == 0) blend(x, y, (hsh >> 5) & 1 ? 0xFFD27A : 0xFFF1C4, 0.9f);   // windows
          else if (k == K_ROAD && hsh % 41 == 0) { glow(x, y, 4, 0xFFB347, 0.45f); blend(x, y, 0xFFE2A8, 1); }   // lamps
        }
    // neon: on street circuits some buildings get glowing coloured outlines (pink, cyan, violet, green)
    if (kindMask && (ground == G_CITY || ground == G_NIGHT)) {
      static const uint32_t kNeon[4] = {0xFF3FB4, 0x35E8FF, 0xB36BFF, 0x5CFF8A};
      for (int y = 1; y < MAP_H - 1; ++y)
        for (int x = 1; x < MAP_W - 1; ++x) {
          if (kindMask[y * MAP_W + x] != K_BUILD) continue;
          const bool edge = kindMask[y * MAP_W + x - 1] != K_BUILD || kindMask[y * MAP_W + x + 1] != K_BUILD ||
                            kindMask[(y - 1) * MAP_W + x] != K_BUILD || kindMask[(y + 1) * MAP_W + x] != K_BUILD;
          if (!edge) continue;
          const uint32_t cell = (uint32_t)((x / 22) * 92837111u ^ (y / 22) * 689287499u);
          if (cell % 3) continue;                          // about one block in three is neon
          const uint32_t col = kNeon[(cell >> 4) % 4];
          blend(x, y, col, 1);
          blend(x - 1, y, col, 0.25f); blend(x + 1, y, col, 0.25f); blend(x, y - 1, col, 0.25f); blend(x, y + 1, col, 0.25f);
        }
    }
    for (int i = 0; i < trkN; i += 2)                   // floodlights along the track
      glow(mx(trk[2 * i]), my(trk[2 * i + 1]), 11, 0xFFF4D6, 0.10f);
  }
  // Track drawn like the real thing: white edge lines, grey asphalt, red/white kerbs on the
  // outside of the corners. The later pass at a crossing is redrawn on top (Suzuka's bridge).
  auto seg = [&](int i, float r, uint32_t col, float a) { const int j = (i + 1) % trkN;
    thick(mx(trk[2 * i]), my(trk[2 * i + 1]), mx(trk[2 * j]), my(trk[2 * j + 1]), r, col, a); };
  // over the map picture: a solid black band (like a circuit plan); on our drawing: asphalt with white edges
  const uint32_t cEdge = 0xEDEFF2, cAsph = 0x30343B;
  for (int i = 0; i < trkN; ++i) seg(i, 6.0f, 0x000000, picture ? 0.18f : 0.6f);   // shadow / dark outline
  for (int i = 0; i < trkN; ++i) seg(i, 4.6f, cEdge, 1);
  for (int i = 0; i < trkN; ++i) seg(i, 3.7f, cAsph, 1);
  for (int i = 0; i + 1 < trkN; ++i)
    for (int j = i + 3; j + 1 < trkN && j < i + trkN - 3; ++j) {
      const float ax = trk[2 * i], ay = trk[2 * i + 1], bx = trk[2 * i + 2], by = trk[2 * i + 3];
      const float cx = trk[2 * j], cy = trk[2 * j + 1], dx = trk[2 * j + 2], dy = trk[2 * j + 3];
      const float den = (bx - ax) * (dy - cy) - (by - ay) * (dx - cx);
      if (fabsf(den) < 1e-6f) continue;
      const float t = ((cx - ax) * (dy - cy) - (cy - ay) * (dx - cx)) / den, u = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)) / den;
      if (t < 0 || t > 1 || u < 0 || u > 1) continue;
      for (int k = imax(0, j - 2); k <= imin(trkN - 2, j + 2); ++k) seg(k, 6.0f, 0x000000, 0.35f);
      for (int k = imax(0, j - 2); k <= imin(trkN - 2, j + 2); ++k) seg(k, 4.6f, cEdge, 1);   // bridge edge
      for (int k = imax(0, j - 2); k <= imin(trkN - 2, j + 2); ++k) seg(k, 3.7f, cAsph, 1);
    }
  {                                                           // kerbs
    uint32_t kerb[3] = {0xE1251B, 0xFFFFFF, 0}; int nKerb = 2;   // default red / white
    for (const auto &kc : kKerbs) if (!strcmp(kc.cid, race->cid)) { memcpy(kerb, kc.col, sizeof kerb); nKerb = kc.col[2] ? 3 : 2; }
    int stripe = 0;
    for (int i = 1; i < cpN; ++i) {
      if (cp[i].start || cp[i].right != cp[i - 1].right || cp[i].k - cp[i - 1].k != 1) { stripe = 0; continue; }
      thick(cp[i - 1].x + cp[i - 1].nx * 4.8f, cp[i - 1].y + cp[i - 1].ny * 4.8f, cp[i].x + cp[i].nx * 4.8f, cp[i].y + cp[i].ny * 4.8f,
            1.1f, kerb[(stripe++ / 2) % nKerb], 1);
    }
  }
  // timing lines: a thin line across the track where sectors 2 and 3 begin, with a small label
  for (int k = 1; k < 3; ++k) {
    float x, y, ux, uy; trackPoint(k / 3.0f, x, y, ux, uy);
    const float qx = -uy, qy = ux;                           // across the track
    line(x - qx * 6, y - qy * 6, x + qx * 6, y + qy * 6, 0xFFD60A, 1);
    line(x - qx * 6 + ux * 0.8f, y - qy * 6 + uy * 0.8f, x + qx * 6 + ux * 0.8f, y + qy * 6 + uy * 0.8f, 0xFFD60A, 0.7f);
    static const char *kS[3] = {"S1", "S2", "S3"};
    if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)(x + qx * 10) - 6, (int)(y + qy * 10) - 9, kS[k], lightMap ? 0xB86E00u : 0xFFD60Au};
  }
  // start line (chequered, across the track) + direction arrow a little further
  float x, y, ux, uy; trackPoint(0, x, y, ux, uy);
  const float qx = -uy, qy = ux;
  for (int k = -4; k < 4; ++k) for (int l = -1; l < 2; ++l) {  // chequered band across the track (8 x 3 squares)
    const float cx = x + qx * (k + 0.5f) * 1.25f + ux * l * 1.25f, cy = y + qy * (k + 0.5f) * 1.25f + uy * l * 1.25f;
    thick(cx - ux * 0.3f, cy - uy * 0.3f, cx + ux * 0.3f, cy + uy * 0.3f, 0.45f, ((k + l) & 1) ? 0x101010 : 0xFFFFFF, 1);
  }
  if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)(x - qx * 11) - 22, (int)(y - qy * 11) - 9, "Largada", lightMap ? 0x111111u : 0xFFFFFFu};
  trackPoint(0.035f, x, y, ux, uy);
  const float ax = x + qx * 9, ay = y + qy * 9;          // beside the track
  line(ax - ux * 5, ay - uy * 5, ax + ux * 5, ay + uy * 5, 0xFFFFFF, 1);
  line(ax + ux * 5, ay + uy * 5, ax + ux * 1 + qx * 3, ay + uy * 1 + qy * 3, 0xFFFFFF, 1);
  line(ax + ux * 5, ay + uy * 5, ax + ux * 1 - qx * 3, ay + uy * 1 - qy * 3, 0xFFFFFF, 1);
  // landmarks
  for (const auto &l : kLandmarks) {
    if (strcmp(l.cid, race->cid)) continue;
    const float lx = mx((l.lon - h.lon0) * kx), ly = my((l.lat - h.lat0) * ky);
    if (lx < 6 || lx > MAP_W - 6 || ly < 8 || ly > MAP_H - 8) continue;
    drawIcon(lx, ly, l.icon);
    if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)lx + 10, (int)ly - 8, l.name, lightMap ? 0x33404Fu : 0xB8C6DCu};
  }
  // famous corners (tap the map to show / hide)
  if (havePit && mapLabelN < MAX_LABELS) {
    const float px = mx(pitX), py = my(pitY);
    if (px > 4 && px < MAP_W - 30 && py > 6 && py < MAP_H - 6) mapLabels[mapLabelN++] = {(int)px + 6, (int)py - 8, "Boxes", lightMap ? 0x33404Fu : 0xF0F0F0u};
  }
  if (showCorners && osmCornerN)
    for (int i = 0; i < osmCornerN; ++i) {
      const float cx = mx(osmCornerX[i]), cy = my(osmCornerY[i]);
      disc(cx, cy, 2.6f, 0xFFD54A, 1);
      if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)cx + 5, (int)cy - 16, osmCornerName[i], lightMap ? 0x7A4E00u : 0xFFD54Au};
    }
  bool named = osmCornerN > 0;
  for (const auto &c : kCorners) if (!strcmp(c.cid, race->cid)) named = true;
  if (showCorners && !osmCornerN && named)
    for (const auto &c : kCorners) {
      if (strcmp(c.cid, race->cid)) continue;
      trackPoint(c.frac, x, y, ux, uy);
      disc(x, y, 2.6f, 0xFFD54A, 1);
      if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)x + 5, (int)y - 16, c.name, lightMap ? 0x7A4E00u : 0xFFD54Au};
    }
  if (showCorners && !named) {                            // no names known: number the corners, like the TV maps
    static char num[30][4];
    int n = 0;
    for (int i = 0; i < cpN && n < 30; ++i) {
      if (!cp[i].start) continue;
      int j = i; while (j + 1 < cpN && !cp[j + 1].start) ++j;
      const CPt &m = cp[(i + j) / 2];                       // middle of the corner, badge on the outside
      const float bx = m.x + m.nx * 13, by = m.y + m.ny * 13;
      disc(bx, by, 6.5f, 0xFFFFFF, 1); disc(bx, by, 5.4f, 0x14171C, 1);
      snprintf(num[n], sizeof num[0], "%d", n + 1);
      if (mapLabelN < MAX_LABELS) mapLabels[mapLabelN++] = {(int)bx - (n + 1 >= 10 ? 8 : 4), (int)by - 9, num[n], 0xFFFFFF};
      ++n;
    }
  }
}

// ============================================================== UI: screen
static lv_obj_t *tabBtn[3], *pages[3];
static lv_obj_t *lblGp, *lblPlace, *lblRound, *lblNextKind, *f1LblDate, *lblTime, *lblCount, *lblHint;
static lv_obj_t *mapCanvas, *lists[2];
static lv_obj_t *progPanel, *progTitle, *progSub, *progBox[S_COUNT], *progDot[S_COUNT], *progRow[S_COUNT], *progDay[S_COUNT],
                *progTime[S_COUNT], *progFoot, *progRaceWx;
static lv_timer_t *progTimer;
static int f1Page = 0;
static uint32_t shownVersion = 0xFFFFFFFF, shownMapVersion = 0xFFFFFFFF;
static char shownMapCid[20] = "?";
static bool shownCorners;
static int autoPageDone = -1;                             // version for which the start f1Page was chosen

static const uint32_t C_RED = 0xE10600, C_BG = 0x0B0F17, C_TXT = 0xECF0F6, C_DIM = 0x8C97AA;

static void localTm(time_t t, struct tm &out) { const time_t l = t + cfg.utcOffset; gmtime_r(&l, &out); }
static const char *kWd[7] = {"dom", "seg", "ter", "qua", "qui", "sex", "s\xC3\xA1" "b"};

// race shown on the "next" f1Page: the first one not finished yet (start + 2 h)
static int previewIdx = -1;                                // >= 0: browsing other circuits (arrows on the map)
static uint32_t previewMs;
static const Race *displayRace(time_t now) {
  if (previewIdx >= 0 && previewIdx < ui->nRaces) return &ui->races[previewIdx];
  for (int i = 0; i < ui->nRaces; ++i) if (ui->races[i].t[S_RACE] && ui->races[i].t[S_RACE] + 7200 > now) return &ui->races[i];
  return nullptr;
}
static const Race *lastRace(time_t now) {
  const Race *r = nullptr;
  for (int i = 0; i < ui->nRaces; ++i) if (ui->races[i].t[S_RACE] && ui->races[i].t[S_RACE] + 7200 <= now) r = &ui->races[i];
  return r;
}
// results mode: chequered flag seen (result published) and still the same local day
static bool resultsMode(time_t now) {
  const Race *r = lastRace(now);
  if (!r || ui->resultRound != r->round) return false;
  struct tm a, b; localTm(r->t[S_RACE] + 7200, a); localTm(now, b);
  return a.tm_yday == b.tm_yday && a.tm_year == b.tm_year;
}

static void setPage(int p) {
  f1Page = p;
  for (int i = 0; i < 3; ++i) {
    if (i == p) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(tabBtn[i], i == p ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(tabBtn[i], 0), lv_color_hex(i == p ? 0xFFFFFF : C_DIM), 0);
  }
}
static void tabEvent(lv_event_t *e) { setPage((int)(intptr_t)lv_event_get_user_data(e)); }
static void gestureEvent(lv_event_t *) {
  const lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
  if (d == LV_DIR_LEFT && f1Page < 2) setPage(f1Page + 1);
  else if (d == LV_DIR_RIGHT && f1Page > 0) setPage(f1Page - 1);
}

static void fillStandings(int which, time_t now) {
  lv_obj_t *list = lists[which];
  lv_obj_clean(list);
  auto row = [&](int h) { lv_obj_t *r = lv_obj_create(list); lv_obj_set_size(r, 456, h); lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
                          lv_obj_set_style_border_width(r, 0, 0); lv_obj_set_style_pad_all(r, 0, 0); lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
                          lv_obj_add_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE); return r; };
  // banner: podium after the chequered flag, champions at the end of the season
  const bool over = ui->nRaces && !displayRace(now);
  if ((resultsMode(now) || over) && (which == 0 ? ui->nDrivers : ui->nTeams)) {
    lv_obj_t *b = row(46);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1A2232), 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0); lv_obj_set_style_radius(b, 10, 0);
    lv_obj_t *bar = lv_obj_create(b); lv_obj_set_size(bar, 4, 34); lv_obj_set_pos(bar, 8, 6);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_RED), 0); lv_obj_set_style_border_width(bar, 0, 0); lv_obj_set_style_radius(bar, 2, 0);
    char t1[64], t2[96];
    if (over) {
      snprintf(t1, sizeof t1, "CAMPE\xC3\x95" "ES %d", ui->season);
      snprintf(t2, sizeof t2, "%s  \xC2\xB7  %s", ui->drivers[0].name, ui->teams[0].name);
    } else {
      snprintf(t1, sizeof t1, "RESULTADO  \xC2\xB7  %s", ui->resultRace);
      snprintf(t2, sizeof t2, "1\xC2\xBA %s    2\xC2\xBA %s    3\xC2\xBA %s", ui->podium[0], ui->podium[1], ui->podium[2]);
    }
    lv_obj_t *l1 = makeLabel(b, t1, &lv_font_weather_14, 0xFFD54A); lv_obj_set_pos(l1, 20, 4);
    lv_obj_t *l2 = makeLabel(b, t2, &lv_font_weather_14, C_TXT); lv_obj_set_pos(l2, 20, 24);
  }
  const int n = which == 0 ? ui->nDrivers : ui->nTeams;
  if (!n) { lv_obj_t *r = row(40); makeLabel(r, ui->standMs ? "O campeonato ainda n\xC3\xA3o come\xC3\xA7ou" : "carregando...", &lv_font_weather_14, C_DIM); return; }
  const Standing *s = which == 0 ? ui->drivers : ui->teams;
  for (int i = 0; i < n; ++i) {
    lv_obj_t *r = row(25);
    char b[24];
    snprintf(b, sizeof b, "%d", s[i].pos);
    lv_obj_t *p = makeLabel(r, b, &lv_font_weather_14, i < 3 ? 0xFFD54A : C_DIM); lv_obj_set_width(p, 26);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(p, 0, 4);
    lv_obj_t *bar = lv_obj_create(r); lv_obj_set_size(bar, 4, 17); lv_obj_set_pos(bar, 34, 4);
    lv_obj_set_style_bg_color(bar, lv_color_hex(teamColor(s[i].teamId)), 0); lv_obj_set_style_border_width(bar, 0, 0); lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_t *nm = makeLabel(r, s[i].name, &lv_font_weather_14, C_TXT); lv_obj_set_pos(nm, 46, 4);
    lv_obj_set_width(nm, which == 0 ? 160 : 250); lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    if (which == 0) { lv_obj_t *tm = makeLabel(r, s[i].team, &lv_font_weather_14, C_DIM); lv_obj_set_pos(tm, 212, 4);
                      lv_obj_set_width(tm, 130); lv_label_set_long_mode(tm, LV_LABEL_LONG_DOT); }
    snprintf(b, sizeof b, "%g", s[i].pts);
    lv_obj_t *pt = makeLabel(r, b, &lv_font_weather_14, C_TXT); lv_obj_set_width(pt, 52);
    lv_obj_set_style_text_align(pt, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(pt, 344, 4);
    if (i) { snprintf(b, sizeof b, "-%g", s[0].pts - s[i].pts);
             lv_obj_t *gp = makeLabel(r, b, &lv_font_weather_14, 0x5F6B7E); lv_obj_set_width(gp, 52);
             lv_obj_set_style_text_align(gp, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(gp, 400, 4); }
  }
}

// next session of the weekend (or the one running now)
static int nextSession(const Race *r, time_t now, bool &live) {
  live = false;
  for (int s = 0; s < S_COUNT; ++s) {
    if (!r->t[s]) continue;
    const int dur = s == S_RACE ? 7200 : 3600;
    if (now < r->t[s]) return s;
    if (now < r->t[s] + dur) { live = true; return s; }
  }
  return S_RACE;
}

static const char *shortSky(int c, bool night) {       // short sky text for the programme
  if (c == 0) return night ? "C\xC3\xA9u limpo" : "Sol";
  if (c == 1) return night ? "Poucas nuvens" : "Sol e nuvens";
  if (c == 2) return "Parc. nublado";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if (c >= 51 && c <= 57) return "Garoa";
  if (c >= 61 && c <= 67) return "Chuva";
  if (c >= 71 && c <= 77) return "Neve";
  if (c >= 80 && c <= 82) return "Pancadas";
  if (c >= 95) return "Trovoada";
  return "";
}
static void closeProg() { lv_obj_add_flag(progPanel, LV_OBJ_FLAG_HIDDEN); lv_timer_pause(progTimer); }
// Fills the programme overlay from `ui`. Called when it opens and again by f1Render()
// while it is open whenever new data arrives (the race forecast is fetched after the
// calendar, so it often lands a few seconds after the panel was opened).
static uint32_t progVersion = 0;
static bool fillProg() {
  if (!ui) return false;
  const time_t now = time(nullptr);
  const Race *r = displayRace(now);
  if (!r) return false;
  progVersion = ui->version;
  const CircuitInfo *ci = circuitInfo(r->cid);
  lv_label_set_text(progTitle, ci ? ci->gp : r->name);
  static char sub[64]; snprintf(sub, sizeof sub, "Programa\xC3\xA7\xC3\xA3o  \xC2\xB7  seu hor\xC3\xA1rio (GMT%+d)", cfg.utcOffset / 3600);
  lv_label_set_text(progSub, sub);
  bool live; const int nx = nextSession(r, now, live);
  int k = 0;
  for (int s = 0; s < S_COUNT; ++s) {
    if (!r->t[s]) continue;
    struct tm t; localTm(r->t[s], t);
    const bool past = now >= r->t[s] + (s == S_RACE ? 7200 : 3600), next = s == nx && !past;
    // colours: done = grey, next = red row, race = yellow, others = white
    const uint32_t txt = past ? 0x66728A : s == S_RACE ? 0xFFD54A : C_TXT;
    const uint32_t dot = past ? 0x4A5466 : next ? 0xFF3B30 : s == S_RACE ? 0xFFD54A : 0xC9D2E0;
    lv_obj_set_style_bg_color(progBox[k], lv_color_hex(next ? 0x3A1518 : 0x1A2232), 0);
    lv_obj_set_style_bg_opa(progBox[k], next || (k & 1) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(progDot[k], lv_color_hex(dot), 0);
    char b[24];
    lv_label_set_text(progRow[k], kSession[s]);
    snprintf(b, sizeof b, "%s %02d/%02d", kWd[t.tm_wday], t.tm_mday, t.tm_mon + 1); lv_label_set_text(progDay[k], b);
    if (next && live) snprintf(b, sizeof b, "AO VIVO"); else snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(progTime[k], b);
    for (lv_obj_t *o : {progRow[k], progDay[k], progTime[k]}) lv_obj_set_style_text_color(o, lv_color_hex(next ? 0xFFFFFF : txt), 0);
    lv_obj_remove_flag(progBox[k], LV_OBJ_FLAG_HIDDEN);
    ++k;
  }
  for (; k < S_COUNT; ++k) lv_obj_add_flag(progBox[k], LV_OBJ_FLAG_HIDDEN);
  // forecast only for the race of the coming GP (not in the preview of other circuits)
  if (previewIdx < 0 && ui->wxHave[S_RACE] && !strcmp(ui->wxCid, r->cid)) {
    static char w[96];
    // main race only (not the sprint): day min/max at the circuit + the sky at the start time
    snprintf(w, sizeof w, "Previs\xC3\xA3o p/ a corrida:  m\xC3\xADn %.0f\xC2\xB0  \xC2\xB7  m\xC3\xA1x %.0f\xC2\xB0  \xC2\xB7  %s",
             ui->wxMin[S_RACE], ui->wxMax[S_RACE],
             shortSky(ui->wxCode[S_RACE], ui->wxNight[S_RACE]));
    lv_label_set_text(progRaceWx, w); lv_obj_remove_flag(progRaceWx, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(progRaceWx, LV_OBJ_FLAG_HIDDEN);
  static char foot[96];
  char len[16] = "";
  TrackHdr h;
  if (fsOk() && loadTrackHdr(r->cid, h, nullptr, 0) && h.lengthM) { snprintf(len, sizeof len, "%.3f km", h.lengthM / 1000.0f); for (char *q = len; *q; ++q) if (*q == '.') *q = ','; }
  snprintf(foot, sizeof foot, "%s%s%s, %s", len[0] ? "Circuito " : "", len, len[0] ? "  \xC2\xB7  " : "", r->locality, ci ? ci->country : r->country);
  lv_label_set_text(progFoot, foot);
  return true;
}
static void openProg(lv_event_t *) {
  if (!fillProg()) return;
  lv_obj_remove_flag(progPanel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(progPanel);
  lv_timer_reset(progTimer); lv_timer_resume(progTimer);
}
static void mapTap(lv_event_t *) { showCorners = !showCorners; }
static void previewStep(lv_event_t *e) {                 // < > : show another circuit of the calendar
  if (!ui || !ui->nRaces) return;
  const int step = (int)(intptr_t)lv_event_get_user_data(e);
  if (previewIdx < 0) {                                   // start from the race on screen
    const Race *r = displayRace(time(nullptr));
    previewIdx = r ? (int)(r - ui->races) : ui->nRaces - 1;
  }
  previewIdx = (previewIdx + step + ui->nRaces) % ui->nRaces;
  previewMs = millis();
  postScreenJob(CMD_F1);                                  // download that circuit if needed
}
static lv_obj_t *styleBtn, *styleIcon[2];
static const char *kStyleName[ST_COUNT] = {"ruas", "pista"};
static void mapLong(lv_event_t *) {                         // style button: streets -> drawing -> satellite
  mapStyle = (mapStyle + 1) % ST_COUNT;
  for (int i = 0; i < 2; ++i) {
    if (i == mapStyle) lv_obj_remove_flag(styleIcon[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(styleIcon[i], LV_OBJ_FLAG_HIDDEN);
  }
  LogV.printf("F1: mapa em modo %s\n", kStyleName[mapStyle]);
  strcpy(shownMapCid, "?");                                 // redraw now (with or without the picture)
  postScreenJob(CMD_F1);                                    // download the picture for this style if needed
}

static lv_obj_t *plainBox(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_style_radius(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return o;
}

void buildF1Screen(lv_obj_t *scr) {
  lock = xSemaphoreCreateMutex();
  f1View = lv_obj_create(scr);
  lv_obj_set_size(f1View, SCR_W, SCR_H); lv_obj_set_pos(f1View, 0, 0);
  lv_obj_set_style_radius(f1View, 0, 0); lv_obj_set_style_border_width(f1View, 0, 0); lv_obj_set_style_pad_all(f1View, 0, 0);
  lv_obj_set_style_bg_color(f1View, lv_color_hex(C_BG), 0); lv_obj_set_style_bg_opa(f1View, LV_OPA_COVER, 0);
  lv_obj_remove_flag(f1View, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(f1View, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(f1View, gestureEvent, LV_EVENT_GESTURE, NULL);
  // header: red slash + "FÓRMULA 1" + chequered strip
  lv_obj_t *slash = lv_obj_create(f1View); lv_obj_set_size(slash, 6, 22); lv_obj_set_pos(slash, 52, 10);
  lv_obj_set_style_bg_color(slash, lv_color_hex(C_RED), 0); lv_obj_set_style_border_width(slash, 0, 0); lv_obj_set_style_radius(slash, 1, 0);
  lv_obj_t *t = makeLabel(f1View, "F\xC3\x93RMULA 1", &lv_font_weather_20, 0xFFFFFF); lv_obj_set_pos(t, 64, 10);
  for (int i = 0; i < 6; ++i) for (int j = 0; j < 2; ++j) {
    lv_obj_t *q = lv_obj_create(f1View); lv_obj_set_size(q, 5, 5); lv_obj_set_pos(q, 180 + i * 5, 14 + j * 5);
    lv_obj_set_style_bg_color(q, lv_color_hex((i + j) & 1 ? 0x101010 : 0xF0F0F0), 0);
    lv_obj_set_style_border_width(q, 0, 0); lv_obj_set_style_radius(q, 0, 0);
  }
  static const char *kTab[3] = {"GP", "Pilotos", "Equipes"};
  for (int i = 0; i < 3; ++i) {
    tabBtn[i] = lv_button_create(f1View);
    lv_obj_set_size(tabBtn[i], 74, 28); lv_obj_set_pos(tabBtn[i], 248 + i * 76, 7);
    lv_obj_set_style_radius(tabBtn[i], 14, 0); lv_obj_set_style_shadow_width(tabBtn[i], 0, 0);
    lv_obj_set_style_bg_color(tabBtn[i], lv_color_hex(C_RED), 0);
    lv_obj_t *l = makeLabel(tabBtn[i], kTab[i], &lv_font_weather_14, C_DIM); lv_obj_center(l);
    lv_obj_add_event_cb(tabBtn[i], tabEvent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  for (int i = 0; i < 3; ++i) pages[i] = plainBox(f1View, 0, 44, SCR_W, SCR_H - 44);
  // f1Page 0: next session + circuit
  lv_obj_t *p = pages[0];
  lv_obj_t *left = plainBox(p, 0, 0, 192, SCR_H - 44);
  lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(left, openProg, LV_EVENT_CLICKED, NULL);
  lblGp = makeLabel(left, "", &lv_font_weather_20, C_TXT); lv_obj_set_pos(lblGp, 12, 8);
  lv_obj_set_width(lblGp, 180); lv_label_set_long_mode(lblGp, LV_LABEL_LONG_DOT);
  lblPlace = makeLabel(left, "", &lv_font_weather_14, C_DIM); lv_obj_set_pos(lblPlace, 12, 34);
  lv_obj_set_width(lblPlace, 180); lv_label_set_long_mode(lblPlace, LV_LABEL_LONG_DOT);
  lblRound = makeLabel(left, "", &lv_font_weather_14, 0x5F6B7E); lv_obj_set_pos(lblRound, 12, 52);
  lblNextKind = makeLabel(left, "", &lv_font_weather_14, 0xFF5A4E); lv_obj_set_pos(lblNextKind, 12, 86);
  f1LblDate = makeLabel(left, "", &lv_font_weather_20, C_TXT); lv_obj_set_pos(f1LblDate, 12, 106);
  lblTime = makeLabel(left, "", &lv_font_montserrat_48, 0xFFFFFF); lv_obj_set_pos(lblTime, 8, 130);
  lblCount = makeLabel(left, "", &lv_font_weather_14, 0xFFD54A); lv_obj_set_pos(lblCount, 12, 188);
  lblHint = makeLabel(left, "toque: programa\xC3\xA7\xC3\xA3o >", &lv_font_weather_14, 0x4E5A6C); lv_obj_set_pos(lblHint, 12, 248);
  for (lv_obj_t *o : {lblGp, lblPlace, lblRound, lblNextKind, f1LblDate, lblTime, lblCount, lblHint}) lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  mapCanvas = lv_canvas_create(p);
  lv_obj_set_pos(mapCanvas, SCR_W - MAP_W - 2, 0);
  lv_obj_add_flag(mapCanvas, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(mapCanvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(mapCanvas, mapTap, LV_EVENT_CLICKED, NULL);
  // map style button (top-right corner of the map): one tap = next style
  styleBtn = lv_button_create(p);
  lv_obj_set_size(styleBtn, 32, 32); lv_obj_set_pos(styleBtn, SCR_W - 40, 6);
  lv_obj_set_style_radius(styleBtn, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_pad_all(styleBtn, 0, 0); lv_obj_set_style_shadow_width(styleBtn, 0, 0);
  lv_obj_set_style_bg_color(styleBtn, lv_color_hex(0x0A1220), 0); lv_obj_set_style_bg_opa(styleBtn, LV_OPA_70, 0);
  lv_obj_set_style_bg_color(styleBtn, lv_color_hex(C_RED), LV_STATE_PRESSED); lv_obj_set_style_bg_opa(styleBtn, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(styleBtn, lv_color_hex(0xFFFFFF), 0); lv_obj_set_style_border_opa(styleBtn, LV_OPA_30, 0);
  lv_obj_set_style_border_width(styleBtn, 1, 0);
  lv_obj_set_ext_click_area(styleBtn, 10);                   // easier to hit
  // drawn icons (20x20): street grid for "Ruas", a small circuit for "Desenho"
  static const lv_point_precise_t kGrid[][2] = {{{2, 6}, {18, 6}}, {{2, 14}, {18, 14}}, {{7, 2}, {7, 18}}, {{14, 2}, {14, 18}}};
  static const lv_point_precise_t kLoop[] = {{4, 15}, {4, 7}, {7, 4}, {11, 4}, {12, 8}, {16, 9}, {17, 13}, {14, 16}, {8, 16}, {4, 15}};
  for (int i = 0; i < 2; ++i) {
    styleIcon[i] = lv_obj_create(styleBtn);
    lv_obj_set_size(styleIcon[i], 20, 20); lv_obj_center(styleIcon[i]);
    lv_obj_set_style_bg_opa(styleIcon[i], LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(styleIcon[i], 0, 0);
    lv_obj_set_style_pad_all(styleIcon[i], 0, 0);
    lv_obj_remove_flag(styleIcon[i], LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(styleIcon[i], LV_OBJ_FLAG_SCROLLABLE);
  }
  for (const auto &g : kGrid) {
    lv_obj_t *l = lv_line_create(styleIcon[0]); lv_line_set_points(l, g, 2);
    lv_obj_set_style_line_width(l, 2, 0); lv_obj_set_style_line_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
  }
  { lv_obj_t *l = lv_line_create(styleIcon[1]); lv_line_set_points(l, kLoop, 10);
    lv_obj_set_style_line_width(l, 3, 0); lv_obj_set_style_line_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_t *f = lv_obj_create(styleIcon[1]); lv_obj_set_size(f, 4, 4); lv_obj_set_pos(f, 2, 9);   // red start mark
    lv_obj_set_style_bg_color(f, lv_color_hex(C_RED), 0); lv_obj_set_style_border_width(f, 0, 0); lv_obj_set_style_radius(f, 1, 0); }
  lv_obj_add_flag(styleIcon[1], LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(styleBtn, mapLong, LV_EVENT_CLICKED, NULL);
  for (int i = 0; i < 2; ++i) {                           // browse circuits: < (bottom-left of the map)  > (bottom-right)
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, 32, 32);
    lv_obj_set_pos(b, i ? SCR_W - 40 : SCR_W - MAP_W + 4, SCR_H - 44 - 38);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_shadow_width(b, 0, 0); lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A1220), 0); lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_RED), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0); lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = makeLabel(b, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, &lv_font_weather_14, 0xFFFFFF); lv_obj_center(l);
    lv_obj_add_event_cb(b, previewStep, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 1 : -1));
  }
  // pages 1 and 2: scrollable standings
  for (int i = 0; i < 2; ++i) {
    lists[i] = lv_obj_create(pages[i + 1]);
    lv_obj_set_pos(lists[i], 8, 2); lv_obj_set_size(lists[i], 464, SCR_H - 50);
    lv_obj_set_style_bg_opa(lists[i], LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(lists[i], 0, 0);
    lv_obj_set_style_pad_all(lists[i], 0, 0); lv_obj_set_style_pad_row(lists[i], 2, 0);
    lv_obj_set_flex_flow(lists[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(lists[i], LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lists[i], LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(lists[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
  }
  // weekend programme overlay
  progPanel = lv_obj_create(f1View);
  lv_obj_set_size(progPanel, 464, 308); lv_obj_align(progPanel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(progPanel, lv_color_hex(0x10151F), 0); lv_obj_set_style_bg_opa(progPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(progPanel, lv_color_hex(0x2C3850), 0); lv_obj_set_style_border_width(progPanel, 1, 0);
  lv_obj_set_style_radius(progPanel, 16, 0); lv_obj_set_style_pad_all(progPanel, 0, 0);
  lv_obj_remove_flag(progPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(progPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(progPanel, [](lv_event_t *) { closeProg(); }, LV_EVENT_CLICKED, NULL);
  { lv_obj_t *bar = lv_obj_create(progPanel);              // red accent on the left of the title
    lv_obj_set_size(bar, 4, 20); lv_obj_set_pos(bar, 16, 12);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_RED), 0); lv_obj_set_style_border_width(bar, 0, 0); lv_obj_set_style_radius(bar, 2, 0); }
  progTitle = makeLabel(progPanel, "", &lv_font_weather_20, 0xFFFFFF); lv_obj_set_pos(progTitle, 26, 10);
  progSub = makeLabel(progPanel, "", &lv_font_weather_14, C_DIM); lv_obj_set_pos(progSub, 26, 34);
  { lv_obj_t *sep = lv_obj_create(progPanel);
    lv_obj_set_size(sep, 432, 1); lv_obj_set_pos(sep, 16, 56);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x2C3850), 0); lv_obj_set_style_border_width(sep, 0, 0); lv_obj_set_style_radius(sep, 0, 0); }
  // table: dot | session | day | time  (one font, fixed columns)
  for (int i = 0; i < S_COUNT; ++i) {
    progBox[i] = lv_obj_create(progPanel);
    lv_obj_set_size(progBox[i], 440, 26); lv_obj_set_pos(progBox[i], 12, 62 + i * 27);
    lv_obj_set_style_radius(progBox[i], 6, 0); lv_obj_set_style_border_width(progBox[i], 0, 0); lv_obj_set_style_pad_all(progBox[i], 0, 0);
    lv_obj_remove_flag(progBox[i], LV_OBJ_FLAG_SCROLLABLE); lv_obj_remove_flag(progBox[i], LV_OBJ_FLAG_CLICKABLE);
    progDot[i] = lv_obj_create(progBox[i]);
    lv_obj_set_size(progDot[i], 10, 10); lv_obj_set_pos(progDot[i], 10, 8);
    lv_obj_set_style_radius(progDot[i], LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(progDot[i], 0, 0);
    progRow[i] = makeLabel(progBox[i], "", &lv_font_weather_20, C_TXT); lv_obj_set_pos(progRow[i], 30, 2);
    progDay[i] = makeLabel(progBox[i], "", &lv_font_weather_20, C_TXT); lv_obj_set_pos(progDay[i], 226, 2);
    progTime[i] = makeLabel(progBox[i], "", &lv_font_weather_20, C_TXT);
    lv_obj_set_width(progTime[i], 90); lv_obj_set_style_text_align(progTime[i], LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(progTime[i], 340, 2);
  }
  progRaceWx = makeLabel(progPanel, "", &lv_font_weather_14, 0xFFD54A); lv_obj_set_pos(progRaceWx, 26, 256);
  progFoot = makeLabel(progPanel, "", &lv_font_weather_14, C_DIM); lv_obj_set_pos(progFoot, 26, 280);
  lv_obj_add_flag(progPanel, LV_OBJ_FLAG_HIDDEN);
  progTimer = lv_timer_create([](lv_timer_t *) { closeProg(); }, 20000, NULL);
  lv_timer_pause(progTimer);
  addBackButton(f1View);
  setPage(0);
}

void f1OnEnter() {
  if (!ui) ui = (F1Data *)heap_caps_calloc(1, sizeof(F1Data), MALLOC_CAP_SPIRAM);
  if (!shared) shared = (F1Data *)heap_caps_calloc(1, sizeof(F1Data), MALLOC_CAP_SPIRAM);
  if (!mapBuf) {
    mapBuf = (uint16_t *)heap_caps_malloc(MAP_W * MAP_H * 2, MALLOC_CAP_SPIRAM);
    if (mapBuf) lv_canvas_set_buffer(mapCanvas, mapBuf, MAP_W, MAP_H, LV_COLOR_FORMAT_RGB565);
  }
  shownVersion = 0xFFFFFFFF; autoPageDone = -1; strcpy(shownMapCid, "?");
  f1Render();
}
void f1OnExit() { closeProg(); previewIdx = -1; }

static void countdown(char *b, size_t n, long s) {
  if (s <= 0) { snprintf(b, n, "agora"); return; }
  const long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
  if (d) snprintf(b, n, "em %ldd %02ldh %02ldmin", d, h, m);
  else if (h) snprintf(b, n, "em %ldh %02ldmin", h, m);
  else snprintf(b, n, "em %ldmin %02lds", m, s % 60);
}

// UI tick (500 ms while shown)
void f1Render() {
  if (!ui || !shared || !f1View || lv_obj_has_flag(f1View, LV_OBJ_FLAG_HIDDEN)) return;
  if (xSemaphoreTake(lock, 0) == pdTRUE) {
    if (shared->version != ui->version) memcpy(ui, shared, sizeof *ui);
    xSemaphoreGive(lock);
  }
  const time_t now = time(nullptr);
  if (now < 1700000000) return;
  const bool fresh = ui->version != shownVersion;
  if (fresh) {
    shownVersion = ui->version;
    fillStandings(0, now); fillStandings(1, now);
  }
  if (ui->version && (int)ui->version != autoPageDone) {          // start f1Page: results / champions
    autoPageDone = ui->version;
    setPage(resultsMode(now) || (ui->nRaces && !displayRace(now)) ? 1 : 0);
  }
  // programme open while new data (e.g. the race forecast) arrived: refresh it in place
  if (progPanel && !lv_obj_has_flag(progPanel, LV_OBJ_FLAG_HIDDEN) && ui->version != progVersion) fillProg();
  if (previewIdx >= 0 && millis() - previewMs > 120000UL) previewIdx = -1;   // back to the real next race
  const Race *r = displayRace(now);
  const Race *mapRace = r ? r : lastRace(now);
  // f1Page 0 texts
  char b[64];
  if (!ui->nRaces) {
    setLabelIfChanged(lblGp, "F\xC3\xB3rmula 1");
    char m[96]; snprintf(m, sizeof m, "carregando calend\xC3\xA1rio...%s%s", netMsg[0] ? "\n" : "", netMsg);
    setLabelIfChanged(lblPlace, ui->schedMs ? "" : m);
  } else if (!r) {                                       // season over
    snprintf(b, sizeof b, "Temporada %d encerrada", ui->season); setLabelIfChanged(lblGp, b);
    setLabelIfChanged(lblPlace, "Campe\xC3\xA3o de pilotos:");
    setLabelIfChanged(lblRound, "");
    setLabelIfChanged(lblNextKind, "");
    setLabelIfChanged(f1LblDate, ui->nDrivers ? ui->drivers[0].name : "");
    setLabelIfChanged(lblTime, "");
    snprintf(b, sizeof b, "%s", ui->nTeams ? ui->teams[0].name : ""); setLabelIfChanged(lblCount, b);
  } else {
    const CircuitInfo *ci = circuitInfo(r->cid);
    setLabelIfChanged(lblGp, ci ? ci->gp : r->name);
    snprintf(b, sizeof b, "%s  \xC2\xB7  %s", r->locality, ci ? ci->country : r->country); setLabelIfChanged(lblPlace, b);
    snprintf(b, sizeof b, "Rodada %d de %d", r->round, ui->nRaces); setLabelIfChanged(lblRound, b);
    bool live; const int s = nextSession(r, now, live);
    snprintf(b, sizeof b, "%s  \xC2\xB7  %s", previewIdx >= 0 ? "PR\xC3\x89VIA" : live ? "AO VIVO" : "PR\xC3\x93XIMA", kSession[s]);
    setLabelIfChanged(lblNextKind, b);
    struct tm t; localTm(r->t[s], t);
    snprintf(b, sizeof b, "%s %02d/%02d", kWd[t.tm_wday], t.tm_mday, t.tm_mon + 1); setLabelIfChanged(f1LblDate, b);
    snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min); setLabelIfChanged(lblTime, b);
    if (live) snprintf(b, sizeof b, "come\xC3\xA7ou h\xC3\xA1 %ld min", (long)(now - r->t[s]) / 60);
    else if (previewIdx >= 0 && r->t[s] <= now) b[0] = 0;          // preview of a race already run: no countdown
    else countdown(b, sizeof b, (long)(r->t[s] - now));
    setLabelIfChanged(lblCount, b);
  }
  // circuit drawing: on a new circuit, new files from the network task, or corners toggled
  if (mapRace) {
    xSemaphoreTake(lock, portMAX_DELAY); copyStr(mapRace->cid, mapWanted, sizeof mapWanted); xSemaphoreGive(lock);
  }
  if (mapBuf && (strcmp(shownMapCid, mapRace ? mapRace->cid : "") || shownMapVersion != mapVersion || shownCorners != showCorners)) {
    copyStr(mapRace ? mapRace->cid : "", shownMapCid, sizeof shownMapCid);
    shownMapVersion = mapVersion; shownCorners = showCorners;
    const uint32_t t0 = millis();
    renderMap(mapRace);
    lv_layer_t layer; lv_canvas_init_layer(mapCanvas, &layer);
    lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld); ld.font = &lv_font_weather_14;
    for (int i = 0; i < mapLabelN; ++i) {
      ld.color = lv_color_hex(mapLabels[i].col); ld.text = mapLabels[i].t;
      lv_area_t a = {mapLabels[i].x, mapLabels[i].y, mapLabels[i].x + 120, mapLabels[i].y + 18};
      lv_draw_label(&layer, &ld, &a);
    }
    if (!trkN) {
      ld.color = lv_color_hex(0x5F6B7E); ld.align = LV_TEXT_ALIGN_CENTER;
      ld.text = mapRace ? "baixando o tra\xC3\xA7" "ado..." : "";
      lv_area_t a = {0, MAP_H / 2 - 9, MAP_W, MAP_H / 2 + 9}; lv_draw_label(&layer, &ld, &a);
    }
    lv_canvas_finish_layer(mapCanvas, &layer);
    lv_obj_invalidate(mapCanvas);
    LogV.printf("F1: mapa %s desenhado em %lums\n", shownMapCid, (unsigned long)(millis() - t0));
  }
}
