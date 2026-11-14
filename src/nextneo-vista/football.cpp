// Futebol screen: live scores and results from ESPN's public site API (no key).
// Competitions: Champions League, Libertadores, Mundial de Clubes and Copa do Mundo.
// Tabs: Ao vivo · Champions · Libertadores · Mundial.
//
// API rule of the app: ESPN is only called by this screen's job (CMD_FOOTBALL), only while
// the screen is active. The job runs every FOOTBALL_UPDATE_INTERVAL_MS and decides itself
// what is stale: leagues with a live game every ~40 s, the others every 10 min. Team crests are the logos ESPN sends (team.logo); they are downloaded after
// the scores, a few per job, saved on LittleFS (/fl/<team id>.bin) and never delay scores.
//
// Threads: footballFetch() runs on the network task and fills its own copy (net); it
// publishes to `shared` under `lock`; the UI copies it to `ui` when the version changes.
#include "app_screens.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                     const unsigned char *in, size_t insize);

// LVGL 9's copy of lodepng does NOT return raw RGBA: its "out" is an lv_draw_buf_t* holding
// ARGB8888 pixels (byte order B,G,R,A, with a row stride). Reading that pointer as RGBA is what
// drew the blue scribbles. This returns a plain RGBA array (lv_malloc) for either variant.
static unsigned decodePng(const uint8_t *png, size_t len, unsigned char **rgbaOut, unsigned *w, unsigned *h) {
  unsigned char *raw = nullptr;
  *rgbaOut = nullptr;
  const unsigned err = lodepng_decode32(&raw, w, h, png, len);
  if (err || !raw) return err ? err : 1;
  const lv_draw_buf_t *db = (const lv_draw_buf_t *)raw;
  const bool isDrawBuf = db->header.magic == LV_IMAGE_HEADER_MAGIC && db->header.w == *w && db->header.h == *h;
  if (!isDrawBuf) { *rgbaOut = raw; return 0; }            // plain lodepng: already RGBA
  unsigned char *rgba = (unsigned char *)lv_malloc((size_t)*w * *h * 4);
  if (!rgba) { lv_draw_buf_destroy((lv_draw_buf_t *)db); return 2; }
  const uint32_t stride = db->header.stride ? db->header.stride : *w * 4;
  for (unsigned y = 0; y < *h; ++y) {
    const uint8_t *src = db->data + y * stride;
    unsigned char *dst = rgba + y * *w * 4;
    memcpy(dst, src, (size_t)*w * 4);                         // lodepng writes RGBA (LVGL swaps later, not here)
  }
  lv_draw_buf_destroy((lv_draw_buf_t *)db);
  *rgbaOut = rgba;
  return 0;
}   // LVGL's lodepng (LV_USE_LODEPNG)

lv_obj_t *footballView;

// ================================================================ data
enum League : uint8_t { L_CL, L_LIB, L_CWC, L_WC, L_SUL, L_UEL, L_INT, L_COUNT };   // L_INT has no tab: shown under MUN
static const char *const kSlug[L_COUNT] = {"uefa.champions", "conmebol.libertadores", "fifa.cwc", "fifa.world", "conmebol.sudamericana", "uefa.europa", "fifa.intercontinental_cup"};
static const char *const kLeagueName[L_COUNT] = {"CHAMPIONS LEAGUE", "LIBERTADORES", "MUNDIAL DE CLUBES", "COPA DO MUNDO", "SUL-AMERICANA", "EUROPA LEAGUE", "INTERCONTINENTAL"};
static const char *const kLeagueShort[L_COUNT] = {"UCL", "LIB", "MUN", "COPA", "SUL", "UEL", "INT"};   // until the logo arrives
// importance in "Ao vivo": UCL / Copa / LIB in a tier of their own (a phase bonus of up to +40 never lifts
// another competition above them); favourites (+1000) still come first
static const uint8_t kLeagueWeight[L_COUNT] = {220, 210, 60, 230, 40, 40, 60};
static constexpr int MAX_M = 24, CREST = 40, LEAGUE_LOGO = 30;
static inline uint32_t leagueImgId(int L) { return 0x80000200u + (uint32_t)L; }   // new base: old ids meant other leagues   // crest cache/file key
static constexpr uint32_t LIVE_TTL_MS = 40000, IDLE_TTL_MS = 10UL * 60 * 1000;
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

static SemaphoreHandle_t lock;
static FData *net, *shared, *ui;
static char favorites[64] = "";            // "FLA,PAL" (ESPN abbreviations), prefs "futFav"

// ================================================================ helpers
static void copyStr(const char *s, char *out, size_t n) { snprintf(out, n, "%s", s ? s : ""); }
static uint32_t hexColor(const char *s, uint32_t def) {
  if (!s || strlen(s) < 6) return def;
  return (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFF;
}
static time_t utcFromIso(const char *s) {               // "2026-11-14T22:00Z" / "...:00Z"
  int Y, M, D, h = 0, m = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) < 3) return 0;
  // days from civil (Howard Hinnant)
  Y -= M <= 2; const int era = (Y >= 0 ? Y : Y - 399) / 400; const unsigned yoe = (unsigned)(Y - era * 400);
  const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + (long)doe - 719468L;
  return (time_t)(days * 86400L + h * 3600L + m * 60L);
}
// ESPN phase notes are English: translate the usual words
static void phasePt(const char *in, char *out, size_t n) {
  static const char *const kMap[][2] = {
    {"Quarterfinals", "Quartas de final"}, {"Quarterfinal", "Quartas de final"}, {"Semifinals", "Semifinal"},
    {"Round of 16", "Oitavas de final"}, {"Round of 32", "16 avos"}, {"Knockout Round Playoffs", "Playoffs"},
    {"League Phase", "Fase de liga"}, {"Group Stage", "Fase de grupos"}, {"1st Leg", "ida"}, {"2nd Leg", "volta"},
    {"Leg 1", "ida"}, {"Leg 2", "volta"}, {"Aggregate", "agreg."}, {"Final", "Final"}, {" - ", " \xC2\xB7 "}};
  String s = in ? in : "";
  for (auto &p : kMap) s.replace(p[0], p[1]);
  copyStr(s.c_str(), out, n);
}
static bool isFavorite(const char *abbr) {
  if (!abbr[0] || !favorites[0]) return false;
  char key[8]; snprintf(key, sizeof key, "%s", abbr);
  const char *p = favorites;
  const size_t k = strlen(key);
  while ((p = strstr(p, key))) {
    if ((p == favorites || p[-1] == ',') && (p[k] == 0 || p[k] == ',')) return true;
    p += k;
  }
  return false;
}
static int importance(const FMatch &m) {
  int s = kLeagueWeight[m.league];
  if (isFavorite(m.home.abbr) || isFavorite(m.away.abbr)) s += 1000;
  if (strstr(m.phase, "Final") && !strstr(m.phase, "Quartas") && !strstr(m.phase, "Oitavas")) s += strstr(m.phase, "Semi") ? 25 : 40;
  else if (strstr(m.phase, "Quartas")) s += 15;
  else if (strstr(m.phase, "Oitavas")) s += 8;
  return s;
}

// ================================================================ network task
static WiFiClientSecure espn;                            // keep-alive to site.api.espn.com while the screen is open
static bool espnOpen = false;
// Download buffer (PSRAM, grows up to 2 MB for big scoreboards). Only used by this screen's
// job; given back as soon as the user leaves the screen.
static PsramBuffer body(2048 * 1024);
static void closeEspn() {
  if (!espnOpen) return;
  espn.stop(); espnOpen = false;
}
void footballReleaseConnection() {                       // worker loop, when Futebol is not the active screen
  const bool had = espnOpen || body.data();
  closeEspn();
  body.release();
  if (had) Log.println("Futebol: conex\xC3\xA3o TLS e buffer liberados");
}

static int fetchJson(WiFiClientSecure &client, bool keep, const char *url, PsramBuffer &body,
                     JsonDocument &doc, JsonDocument &filter) {
  if (!tlsMemoryAvailable("futebol")) { snprintf(net->msg, sizeof net->msg, "mem\xC3\xB3ria baixa p/ conex\xC3\xA3o"); return -1; }
  static bool cfg = false;
  if (!cfg) { espn.setInsecure(); espn.setHandshakeTimeout(10); cfg = true; }
  if (!client.connected()) client.stop();
  body.clear();
  if (keep) espnOpen = true;
  const uint32_t t0 = millis();
  HttpsFetchOptions opt; opt.userAgent = "Mozilla/5.0 (NextNeoVista; ESP32-S3)"; opt.followRedirects = true;
  const int st = httpsFetch(client, url, body, 20000, keep, nullptr, "application/json", opt);
  LogV.printf("Futebol: %.70s -> HTTP %d, %u bytes, %lums\n", url + 8, st, (unsigned)body.length(), (unsigned long)(millis() - t0));
  if (st != 200 || body.overflowed()) {
    client.stop(); if (keep) espnOpen = false;
    snprintf(net->msg, sizeof net->msg, "ESPN respondeu %d", st);
    Log.printf("Futebol: %.90s -> HTTP %d%s (%u bytes)\n", url + 8, st, body.overflowed() ? " estourou o buffer" : "",
               (unsigned)body.length());
    return st == 200 ? -2 : st;
  }
  // ESPN nests deeper than ArduinoJson's default limit of 10 (events > competitions >
  // competitors > team > links > rel ...): "TooDeep" even with a filter.
  const DeserializationError je = deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter),
                                                  DeserializationOption::NestingLimit(40));
  if (je) {
    snprintf(net->msg, sizeof net->msg, "resposta inv\xC3\xA1lida da ESPN");
    Log.printf("Futebol: %.90s -> JSON %s (%u bytes)\n", url + 8, je.c_str(), (unsigned)body.length());
    return -3;
  }
  net->msg[0] = 0;
  return 200;
}

static void readTeam(JsonVariantConst t, FTeam &out) {
  out.id = (uint32_t)atol(t["id"] | "0");
  copyStr(t["abbreviation"] | "", out.abbr, sizeof out.abbr);
  copyStr(t["shortDisplayName"] | (t["displayName"] | ""), out.name, sizeof out.name);
  out.color = hexColor(t["color"] | "", 0x3A4558);
  out.alt = hexColor(t["alternateColor"] | "", 0xFFFFFF);
  const char *logo = t["logo"] | "";
  if (!*logo) logo = t["logos"][0]["href"] | "";
  const char *path = strstr(logo, ".com/");                // keep only the path: /i/teamlogos/soccer/500/819.png
  copyStr(path ? path + 4 : "", out.logo, sizeof out.logo);
}

// Plain scoreboard per competition (current round / nearest games). The date-range form
// (?dates=AAAAMMDD-AAAAMMDD) answers HTTP 400 "Failed to get events endpoint" for soccer.
static bool fetchLeague(uint8_t L, PsramBuffer &body) {
  char url[160];
  snprintf(url, sizeof url, "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/scoreboard", kSlug[L]);
  JsonDocument filter(SpiRamJsonAllocator::instance());
  JsonObject lg = filter["leagues"].add<JsonObject>()["logos"].add<JsonObject>();
  lg["href"] = true; lg["rel"] = true;
  JsonObject e = filter["events"].add<JsonObject>();
  e["id"] = true; e["date"] = true;
  e["status"]["displayClock"] = true;
  e["status"]["type"]["state"] = true; e["status"]["type"]["shortDetail"] = true;
  JsonObject c = e["competitions"].add<JsonObject>();
  c["notes"].add<JsonObject>()["headline"] = true;
  c["altGameNote"] = true;                                  // "UEFA Champions League, League Phase"
  JsonObject cp = c["competitors"].add<JsonObject>();
  cp["homeAway"] = true; cp["score"] = true;
  JsonObject tm = cp["team"].to<JsonObject>();
  tm["id"] = true; tm["abbreviation"] = true; tm["shortDisplayName"] = true; tm["displayName"] = true;
  tm["color"] = true; tm["alternateColor"] = true; tm["logo"] = true;
  JsonDocument doc(SpiRamJsonAllocator::instance());
  if (fetchJson(espn, true, url, body, doc, filter) != 200) return false;
  FData &d = *net;
  // competition logo: prefer the "dark" variant (made for dark backgrounds)
  {
    const char *best = "";
    for (JsonVariantConst l : doc["leagues"][0]["logos"].as<JsonArrayConst>()) {
      const char *href = l["href"] | "";
      bool dark = false;
      for (JsonVariantConst r : l["rel"].as<JsonArrayConst>()) if (!strcmp(r | "", "dark")) dark = true;
      if (!*best || dark) best = href;
      if (dark) break;
    }
    const char *path = strstr(best, ".com/");
    if (path) copyStr(path + 4, d.leagueLogo[L], sizeof d.leagueLogo[L]);
  }
  uint8_t n = 0; bool live = false;
  for (JsonVariantConst ev : doc["events"].as<JsonArrayConst>()) {
    if (n >= MAX_M) break;
    FMatch &m = d.m[L][n];
    memset(&m, 0, sizeof m);
    m.league = L;
    m.id = (uint32_t)atol(ev["id"] | "0");
    m.start = (uint32_t)utcFromIso(ev["date"] | "");
    const char *st = ev["status"]["type"]["state"] | "pre";
    m.state = !strcmp(st, "in") ? ST_IN : !strcmp(st, "post") ? ST_POST : ST_PRE;
    const char *detail = ev["status"]["type"]["shortDetail"] | "";
    if (m.state == ST_IN) {
      if (!strcmp(detail, "HT") || strstr(detail, "Half")) copyStr("Intervalo", m.clock, sizeof m.clock);
      else copyStr(ev["status"]["displayClock"] | detail, m.clock, sizeof m.clock);
      live = true;
    } else if (m.state == ST_POST) copyStr(strstr(detail, "Pen") ? "P\xC3\xAAnaltis" : "Encerrado", m.clock, sizeof m.clock);
    JsonVariantConst comp = ev["competitions"][0];
    // phase: notes[0].headline (knockout legs); usually empty, then the part of
    // altGameNote after the competition name ("UEFA Champions League, League Phase")
    const char *note = comp["notes"][0]["headline"] | "";
    if (!*note) { const char *alt = comp["altGameNote"] | ""; const char *cm = strstr(alt, ", "); note = cm ? cm + 2 : ""; }
    phasePt(note, m.phase, sizeof m.phase);
    for (JsonVariantConst k : comp["competitors"].as<JsonArrayConst>()) {
      FTeam &t = !strcmp(k["homeAway"] | "", "away") ? m.away : m.home;
      readTeam(k["team"], t);
      const char *sc = k["score"] | "";
      t.score = (int8_t)(*sc ? atoi(sc) : -1);
    }
    ++n;
  }
  // order: live, then upcoming (soonest first), then finished (latest first)
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0; --j) {
      FMatch &a = d.m[L][j - 1], &b = d.m[L][j];
      auto rank = [](const FMatch &x) { return x.state == ST_IN ? 0 : x.state == ST_PRE ? 1 : 2; };
      const int ra = rank(a), rb = rank(b);
      const bool swap = ra > rb || (ra == rb && (ra == 2 ? a.start < b.start : a.start > b.start));
      if (!swap) break;
      FMatch tmp = a; a = b; b = tmp;
    }
  d.n[L] = n; d.live[L] = live; d.fetchedMs[L] = millis();
  return true;
}

// ---- crests: ESPN logo (resized by ESPN's image service) -> RGB565A8 file on LittleFS
// remove every file of a folder (flat), then the folder; keepId() true = keep that file
template <typename Keep> static int cleanDir(const char *dir, bool rmdirToo, Keep keepId) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return 0;
  static char names[160][24]; int n = 0;
  for (File f = d.openNextFile(); f && n < 160; f = d.openNextFile()) {
    const char *nm = f.name(); const char *base = strrchr(nm, '/'); base = base ? base + 1 : nm;
    if (!keepId((uint32_t)strtoul(base, nullptr, 10))) snprintf(names[n++], 24, "%s/%s", dir, base);
    f.close();
  }
  d.close();
  for (int i = 0; i < n; ++i) LittleFS.remove(names[i]);
  if (rmdirToo) LittleFS.rmdir(dir);
  return n;
}
static bool fsReady() {
  if (!photosFsReady()) { Log.println("Futebol: LittleFS indispon\xC3\xADvel (escudos)"); return false; }
  static bool cleaned = false;
  if (!cleaned) {                                           // old folders (wrong colours) only waste space
    cleaned = true;
    const int k = cleanDir("/fl", true, [](uint32_t) { return false; }) + cleanDir("/fl2", true, [](uint32_t) { return false; });
    Log.printf("Futebol: LittleFS %u/%u KB usados%s\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024),
               k ? " (escudos antigos apagados)" : "");
  }
  if (!LittleFS.exists("/fl3")) LittleFS.mkdir("/fl3");
  return true;
}
// a saved crest is valid only if its size matches its header (a write cut short by a full
// LittleFS leaves a half file that would never load and never be downloaded again)
static bool crestFileOk(const char *path) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  uint8_t h[4] = {0};
  const bool ok = f && f.read(h, 4) == 4 && (h[0] | h[1] << 8) && (h[2] | h[3] << 8) &&
                  f.size() == 4 + (size_t)(h[0] | h[1] << 8) * (h[2] | h[3] << 8) * 3;
  if (f) f.close();
  if (!ok) LittleFS.remove(path);
  return ok;
}
// LittleFS full: drop saved crests of teams that are not in the current games
static int pruneCrests() {
  const int k = cleanDir("/fl3", false, [](uint32_t id) {
    if (id >= leagueImgId(0) && id < leagueImgId(L_COUNT)) return true;
    for (int L = 0; L < L_COUNT; ++L)
      for (int i = 0; i < net->n[L]; ++i) if (net->m[L][i].home.id == id || net->m[L][i].away.id == id) return true;
    return false;
  });
  Log.printf("Futebol: LittleFS cheio, %d escudo(s) sem jogo apagado(s) (%u/%u KB)\n", k,
             (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  return k;
}
// /fl3: correct colour order (old /fl and /fl2 files had wrong colours)
static void crestPath(uint32_t id, char *out, size_t n) { snprintf(out, n, "/fl3/%lu.bin", (unsigned long)id); }

static int crestErrors = 0;                                 // logged (not verbose) for the first few failures
static void crestLog(const FTeam &t, const char *what, int code) {
  if (crestErrors++ < 6) Log.printf("Futebol: escudo %s (%lu) falhou: %s %d\n", t.abbr, (unsigned long)t.id, what, code);
}
// GET an image; the CDN may redirect, so redirects are followed
static int getImage(WiFiClientSecure &client, const char *url, PsramBuffer &body) {
  if (!tlsMemoryAvailable("futebol-escudo")) return -1;
  HttpsFetchOptions opt; opt.followRedirects = true; opt.userAgent = "Mozilla/5.0";
  body.clear();
  const int st = httpsFetch(client, url, body, 15000, true, nullptr, "image/png", opt);
  if (st != 200 || body.overflowed() || !body.length()) client.stop();
  return st;
}

static bool downloadCrest(WiFiClientSecure &client, const FTeam &t, PsramBuffer &body, int size) {
  if (!t.id || !t.logo[0]) return false;
  char url[200];
  // 1st try: ESPN's image service, already resized; 2nd: the original logo (resized here)
  snprintf(url, sizeof url, "https://a.espncdn.com/combiner/i?img=%s&w=%d&h=%d", t.logo, size, size);
  int st = getImage(client, url, body);
  unsigned char *rgba = nullptr; unsigned w = 0, h = 0;
  unsigned err = st == 200 ? decodePng(body.data(), body.length(), &rgba, &w, &h) : 1;
  if (st != 200 || err || !rgba) {
    if (rgba) { lv_free(rgba); rgba = nullptr; }
    crestLog(t, st != 200 ? "servi\xC3\xA7o de imagem HTTP" : "PNG (redimensionado) erro", st != 200 ? st : (int)err);
    snprintf(url, sizeof url, "https://a.espncdn.com%s", t.logo);
    st = getImage(client, url, body);
    if (st != 200) { crestLog(t, "logo original HTTP", st); return false; }
    err = decodePng(body.data(), body.length(), &rgba, &w, &h);
    if (err || !rgba || !w || !h) { if (rgba) lv_free(rgba); crestLog(t, "PNG erro", (int)err); return false; }
  }
  // the image service normally returns CREST x CREST; if not, scale down (box average)
  if (w > (unsigned)size || h > (unsigned)size) {
    const unsigned s = (w > h ? w : h), nw = w * size / s, nh = h * size / s;
    unsigned char *small = (unsigned char *)lv_malloc(nw * nh * 4);
    if (!small) { lv_free(rgba); return false; }
    for (unsigned y = 0; y < nh; ++y)
      for (unsigned x = 0; x < nw; ++x) {
        const unsigned x0 = x * w / nw, x1 = (x + 1) * w / nw, y0 = y * h / nh, y1 = (y + 1) * h / nh;
        uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
        for (unsigned yy = y0; yy < y1; ++yy)
          for (unsigned xx = x0; xx < x1; ++xx, ++cnt)
            for (int k = 0; k < 4; ++k) acc[k] += rgba[(yy * w + xx) * 4 + k];
        for (int k = 0; k < 4; ++k) small[(y * nw + x) * 4 + k] = (unsigned char)(cnt ? acc[k] / cnt : 0);
      }
    lv_free(rgba); rgba = small; w = nw; h = nh;
  }
  const size_t px = (size_t)w * h;
  uint8_t *out = (uint8_t *)psramMalloc(4 + px * 3);
  if (!out) { lv_free(rgba); return false; }
  out[0] = w & 255; out[1] = w >> 8; out[2] = h & 255; out[3] = h >> 8;
  uint8_t *c = out + 4, *a = out + 4 + px * 2;               // RGB565 plane, then alpha plane
  for (size_t i = 0; i < px; ++i) {
    const uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
    const uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    c[i * 2] = v & 255; c[i * 2 + 1] = v >> 8; a[i] = rgba[i * 4 + 3];
  }
  lv_free(rgba);
  char path[24]; crestPath(t.id, path, sizeof path);
  auto save = [&]() {
    File f = LittleFS.open(path, "w");
    const bool w = f && f.write(out, 4 + px * 3) == 4 + px * 3;
    if (f) f.close();
    if (!w) LittleFS.remove(path);                          // never leave a half file (it would block retries)
    return w;
  };
  static bool pruned = false;                               // once per boot is enough
  bool ok = save();
  if (!ok && !pruned) { pruned = true; if (pruneCrests()) ok = save(); }
  heap_caps_free(out);
  if (!ok) crestLog(t, "grava\xC3\xA7\xC3\xA3o LittleFS", 0);
  return ok;
}

// up to `budget` missing crests per job; scores always come first
static int fetchCrests(PsramBuffer &body, int budget, bool logosOnly) {
  if (!fsReady()) return 0;
  const FData &d = *net;
  uint32_t tried[120]; int nt = 0, got = 0;
  const int errBefore = crestErrors;
  bool released = false;
  WiFiClientSecure cdn; cdn.setInsecure(); cdn.setHandshakeTimeout(10);
  auto want = [&](const FTeam &t, int size) {
    if (got >= budget || !t.id || !t.logo[0] || !screenJobStillCurrent()) return;
    for (int i = 0; i < nt; ++i) if (tried[i] == t.id) return;
    if (nt < 120) tried[nt++] = t.id;
    char path[24]; crestPath(t.id, path, sizeof path);
    if (crestFileOk(path)) return;
    if (!released) { closeEspn(); released = true; }      // one TLS session at a time (keep the buffer)
    if (downloadCrest(cdn, t, body, size)) ++got;
  };
  for (int L = 0; L < L_COUNT; ++L) {                        // competition logos first (tabs + cards)
    FTeam lg = {}; lg.id = leagueImgId(L); copyStr(kLeagueShort[L], lg.abbr, sizeof lg.abbr);
    copyStr(d.leagueLogo[L], lg.logo, sizeof lg.logo);
    want(lg, LEAGUE_LOGO);
  }
  if (!logosOnly) {
    // first the 6 teams at the top of each tab (3 games, already in display order), then the rest
    for (int L = 0; L < L_COUNT; ++L)
      for (int i = 0; i < d.n[L] && i < 3; ++i) { want(d.m[L][i].home, CREST); want(d.m[L][i].away, CREST); }
    for (int L = 0; L < L_COUNT; ++L)
      for (int i = 3; i < d.n[L]; ++i) { want(d.m[L][i].home, CREST); want(d.m[L][i].away, CREST); }
  }
  cdn.stop();
  // wording without "falha"/"erro" when all went well: the log page colours those words red
  if (got && crestErrors == errBefore) Log.printf("Futebol: %d escudo(s)/logo(s) baixado(s)\n", got);
  else if (crestErrors != errBefore) Log.printf("Futebol: %d escudo(s)/logo(s) baixado(s), %d com problema\n", got, crestErrors - errBefore);
  return got;
}

// One competition's logo, right after its scoreboard arrived (only when not cached yet), so the
// tab shows the logo before the games and team crests fill in.
static bool fetchLeagueLogo(int L) {
  if (!fsReady()) return false;
  if (!net->leagueLogo[L][0]) { Log.printf("Futebol: logo %s: a ESPN n\xC3\xA3o enviou leagues.logos\n", kLeagueShort[L]); return false; }
  FTeam lg = {}; lg.id = leagueImgId(L); copyStr(kLeagueShort[L], lg.abbr, sizeof lg.abbr);
  copyStr(net->leagueLogo[L], lg.logo, sizeof lg.logo);
  char path[24]; crestPath(lg.id, path, sizeof path);
  if (crestFileOk(path)) return false;
  closeEspn();                                            // one TLS session at a time
  WiFiClientSecure cdn; cdn.setInsecure(); cdn.setHandshakeTimeout(10);
  crestErrors = 0;                                        // always log why a competition logo failed
  bool ok = downloadCrest(cdn, lg, body, LEAGUE_LOGO);
  char *dk = strstr(lg.logo, "-dark/");
  if (!ok && dk) {                                        // dark variant failed: try the normal one
    memmove(dk, dk + 5, strlen(dk + 5) + 1);
    ok = downloadCrest(cdn, lg, body, LEAGUE_LOGO);
  }
  cdn.stop();
  Log.printf("Futebol: logo %s %s (%s, %s)\n", kLeagueShort[L], ok ? "baixado" : "com problema", lg.logo,
             LittleFS.exists(path) ? "salvo" : "n\xC3\xA3o salvo");
  return ok;
}

// Network task (CMD_FOOTBALL): fetches only what is stale; cheap when everything is fresh.
bool footballFetch() {
  if (!lock || !shared) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!net) { net = (FData *)heap_caps_calloc(1, sizeof(FData), MALLOC_CAP_SPIRAM); if (!net) return false; }
  bool ok = true;
  auto publish = [&]() {
    net->version++; net->updatedMs = millis();
    xSemaphoreTake(lock, portMAX_DELAY); memcpy(shared, net, sizeof *net); xSemaphoreGive(lock);
  };
  bool anyOk = false;
  for (int L = 0; L < L_COUNT; ++L) if (net->n[L]) anyOk = true;
  // live leagues first, then the rest
  for (int pass = 0; pass < 2; ++pass)
    for (uint8_t L = 0; L < L_COUNT; ++L) {
      if (!screenJobStillCurrent()) return false;
      if ((pass == 0) != net->live[L]) continue;
      const uint32_t ttl = net->live[L] ? LIVE_TTL_MS : IDLE_TTL_MS;
      if (net->fetchedMs[L] && millis() - net->fetchedMs[L] < ttl) continue;
      if (fetchLeague(L, body)) {
        if (screenJobStillCurrent() && fetchLeagueLogo(L)) net->logoVersion++;
        publish(); anyOk = true;
      }
      else {                                                // failed: retry in 2 min, not every job
        net->fetchedMs[L] = millis() - IDLE_TTL_MS + 120000UL;
        if (!net->fetchedMs[L]) net->fetchedMs[L] = 1;
        ok = false;
      }
    }
  // competition logos first, shown at once (tab bar + cards), then team crests: up to 40 per job
  if (screenJobStillCurrent() && fetchCrests(body, L_COUNT, true)) { net->logoVersion++; publish(); }
  if (screenJobStillCurrent() && fetchCrests(body, 40, false)) { net->logoVersion++; publish(); }
  if (!ok) publish();                                       // show the error message
  return ok || anyOk;                                       // one failing competition is not a failed job
}

// ================================================================ UI
// Layout (480x320): header 0..40, tab bar 44..88 (competition logos), list 96..320 (scrolls).
// Cards 456x112: competition + phase / crest, name, big score / state pill.
static constexpr int NCARDS = 16;
static constexpr int CARD_W = 456, CARD_H = 112;
static constexpr uint32_t BG_TOP = 0x050E24, BG_BOT = 0x09204E, CARD = 0x141A26, CARD_EDGE = 0x2A3448;
static constexpr int NTAB = 7;                           // Ao vivo + 6 competitions
static const int8_t kTabLeague[NTAB] = {-1, L_CL, L_LIB, L_CWC, L_WC, L_SUL, L_UEL};
static const int8_t kTabExtra[NTAB] = {-1, -1, -1, L_INT, -1, -1, -1};   // MUN tab also lists the Intercontinental
static lv_obj_t *tabBar, *tabBtn[NTAB], *tabImg[NTAB], *tabTxt[NTAB], *list, *statusLbl, *emptyLbl;
struct CardUi { lv_obj_t *card, *lgImg, *comp, *phase, *crest[2], *crestImg[2], *crestTxt[2], *name[2], *score, *pill; };
static CardUi cards[NCARDS];
static uint8_t tab = 0;                                     // 0 live, 1 CL, 2 LIB, 3 Mundial
static uint32_t shownVersion = UINT32_MAX, shownLogoVersion = UINT32_MAX;
static uint8_t shownTab = 255;

// crest cache (UI thread): team id -> image loaded from LittleFS
struct Crest { uint32_t id; uint8_t *buf; lv_image_dsc_t dsc; uint32_t triedLogoVersion; bool ok; };
static constexpr int NCREST = 72;
static Crest *crests;
static const lv_image_dsc_t *crestFor(uint32_t id, uint32_t logoVersion) {
  if (!id || !crests) return nullptr;
  int slot = -1;
  for (int i = 0; i < NCREST; ++i) {
    if (crests[i].id == id) {
      if (crests[i].ok) return &crests[i].dsc;
      if (crests[i].triedLogoVersion == logoVersion) return nullptr;   // not on disk yet
      slot = i; break;
    }
    if (!crests[i].id && slot < 0) slot = i;
  }
  if (slot < 0) slot = (int)(id % NCREST);                  // full: reuse a slot
  Crest &c = crests[slot];
  if (c.id != id && c.buf) { heap_caps_free(c.buf); c.buf = nullptr; }
  c.id = id; c.ok = false; c.triedLogoVersion = logoVersion;
  char path[24]; crestPath(id, path, sizeof path);
  if (!photosFsReady() || !LittleFS.exists(path)) return nullptr;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  const size_t sz = f.size();
  if (sz < 8 || sz > 4 + CREST * CREST * 3) { f.close(); return nullptr; }
  if (!c.buf) c.buf = (uint8_t *)psramMalloc(4 + CREST * CREST * 3);
  if (!c.buf || f.read(c.buf, sz) != sz) { f.close(); return nullptr; }
  f.close();
  const uint16_t w = c.buf[0] | (c.buf[1] << 8), h = c.buf[2] | (c.buf[3] << 8);
  if (!w || !h || 4 + (size_t)w * h * 3 != sz) return nullptr;
  c.dsc = {};
  c.dsc.header.magic = LV_IMAGE_HEADER_MAGIC; c.dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
  c.dsc.header.w = w; c.dsc.header.h = h; c.dsc.header.stride = w * 2;
  c.dsc.data_size = (uint32_t)w * h * 3; c.dsc.data = c.buf + 4;
  c.ok = true;
  return &c.dsc;
}

static void plain(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_style_radius(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE); lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}
static void show(lv_obj_t *o, bool on) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

// crest = ESPN logo once downloaded; until then a disc in the team colours with the abbreviation
static void makeCrest(lv_obj_t *parent, lv_obj_t **box, lv_obj_t **img, lv_obj_t **txt) {
  *box = lv_obj_create(parent); plain(*box);
  lv_obj_set_size(*box, CREST, CREST);
  *txt = lv_label_create(*box);
  lv_obj_set_size(*txt, CREST, CREST);
  lv_obj_set_style_radius(*txt, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(*txt, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(*txt, 2, 0);
  lv_obj_set_style_text_font(*txt, &lv_font_weather_14, 0);
  lv_obj_set_style_text_align(*txt, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(*txt, CREST / 2 - 9, 0);
  *img = lv_image_create(*box);
  lv_obj_center(*img);
}
static void setCrest(lv_obj_t *img, lv_obj_t *txt, const FTeam &t, uint32_t logoVersion) {
  const lv_image_dsc_t *d = crestFor(t.id, logoVersion);
  if (d) { lv_image_set_src(img, d); show(img, true); show(txt, false); return; }
  show(img, false); show(txt, true);
  char ab[6]; snprintf(ab, sizeof ab, "%.3s", t.abbr[0] ? t.abbr : "?");
  setLabelIfChanged(txt, ab);
  lv_obj_set_style_bg_color(txt, lv_color_hex(t.color), 0);
  lv_obj_set_style_border_color(txt, lv_color_hex(t.alt), 0);
  lv_obj_set_style_text_color(txt, lv_color_hex(t.alt), 0);
}

static void buildCard(int i) {
  CardUi &c = cards[i];
  c.card = lv_obj_create(list);
  lv_obj_set_size(c.card, CARD_W, CARD_H);
  lv_obj_set_style_bg_color(c.card, lv_color_hex(CARD), 0);
  lv_obj_set_style_bg_opa(c.card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c.card, 16, 0);
  lv_obj_set_style_border_width(c.card, 1, 0);
  lv_obj_set_style_border_color(c.card, lv_color_hex(CARD_EDGE), 0);
  lv_obj_set_style_shadow_width(c.card, 0, 0);
  lv_obj_set_style_pad_all(c.card, 0, 0);
  lv_obj_remove_flag(c.card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(c.card, LV_OBJ_FLAG_CLICKABLE);
  // row 1: competition logo (ESPN) - or its short name until the logo arrives - + phase
  c.lgImg = lv_image_create(c.card);
  lv_image_set_scale(c.lgImg, 256 * 22 / LEAGUE_LOGO);          // 30 px logo shown at ~22 px
  lv_obj_set_pos(c.lgImg, 12, 4);
  lv_obj_set_size(c.lgImg, LEAGUE_LOGO, LEAGUE_LOGO);
  c.comp = makeLabel(c.card, "", &lv_font_weather_14, 0x7DB0FF);
  lv_obj_set_pos(c.comp, 16, 10);
  c.phase = makeLabel(c.card, "", &lv_font_weather_14, 0x9AA8C0);
  lv_obj_set_width(c.phase, CARD_W - 64); lv_label_set_long_mode(c.phase, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(c.phase, 48, 10);
  // row 2: crest + name | score | name + crest
  for (int s = 0; s < 2; ++s) {
    makeCrest(c.card, &c.crest[s], &c.crestImg[s], &c.crestTxt[s]);
    lv_obj_set_pos(c.crest[s], s ? CARD_W - 16 - CREST : 16, 38);
    c.name[s] = makeLabel(c.card, "", &lv_font_weather_20, 0xF2F6FF);
    lv_obj_set_width(c.name[s], 112); lv_label_set_long_mode(c.name[s], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(c.name[s], s ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(c.name[s], s ? CARD_W - 16 - CREST - 10 - 112 : 16 + CREST + 10, 46);
  }
  lv_obj_t *box = lv_obj_create(c.card); plain(box);
  lv_obj_set_size(box, 104, 44); lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0A0E16), 0); lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 12, 0);
  c.score = makeLabel(box, "", &lv_font_montserrat_36, 0xFFFFFF); lv_obj_center(c.score);
  // row 3: state pill (minute / interval / time / final)
  c.pill = makeLabel(c.card, "", &lv_font_weather_14, 0xAAB7CC);
  lv_obj_set_style_radius(c.pill, 8, 0);
  lv_obj_set_style_pad_hor(c.pill, 10, 0); lv_obj_set_style_pad_ver(c.pill, 1, 0);
  lv_obj_align(c.pill, LV_ALIGN_TOP_MID, 0, 85);
  show(c.card, false);
}

static void fmtWhen(uint32_t start, char *out, size_t n) {
  if (!start) { out[0] = 0; return; }
  const time_t now = time(nullptr), t = (time_t)start;
  struct tm a, b; localtime_r(&t, &a); localtime_r(&now, &b);
  static const char *const wd[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "S\xC3\xA1" "b"};
  if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) snprintf(out, n, "Hoje \xC2\xB7 %02d:%02d", a.tm_hour, a.tm_min);
  else if (a.tm_yday == b.tm_yday + 1 && a.tm_year == b.tm_year) snprintf(out, n, "Amanh\xC3\xA3 \xC2\xB7 %02d:%02d", a.tm_hour, a.tm_min);
  else snprintf(out, n, "%s %02d/%02d \xC2\xB7 %02d:%02d", wd[a.tm_wday], a.tm_mday, a.tm_mon + 1, a.tm_hour, a.tm_min);
}

static void fillCard(CardUi &c, const FMatch &m, bool top, uint32_t logoVersion) {
  static uint32_t flashId[16], flashUntil[16], flashScore[16];   // goal highlight per match id
  static int nextFlash = 0;
  const bool fav = isFavorite(m.home.abbr) || isFavorite(m.away.abbr);
  const bool hi = fav || top;
  const lv_image_dsc_t *lg = crestFor(leagueImgId(m.league), logoVersion);
  if (lg) { lv_image_set_src(c.lgImg, lg); show(c.lgImg, true); show(c.comp, false); }
  else { show(c.lgImg, false); show(c.comp, true); setLabelIfChanged(c.comp, kLeagueShort[m.league]); }
  lv_obj_set_style_text_color(c.comp, lv_color_hex(hi ? 0xFFCE5A : 0x7DB0FF), 0);
  setLabelIfChanged(c.phase, m.phase[0] ? m.phase : kLeagueName[m.league]);   // phase, else the competition name
  lv_obj_set_style_text_color(c.phase, lv_color_hex(hi ? 0xFFDB8A : 0x9AA8C0), 0);
  const FTeam *t[2] = {&m.home, &m.away};
  for (int s = 0; s < 2; ++s) {
    setLabelIfChanged(c.name[s], t[s]->name);
    setCrest(c.crestImg[s], c.crestTxt[s], *t[s], logoVersion);
  }
  char b[40];
  if (m.state == ST_PRE || m.home.score < 0) snprintf(b, sizeof b, "\xC3\x97");   // "×" before kick-off
  else snprintf(b, sizeof b, "%d-%d", m.home.score, m.away.score);
  setLabelIfChanged(c.score, b);
  lv_obj_set_style_text_font(c.score, m.state == ST_PRE ? &lv_font_weather_20 : &lv_font_montserrat_36, 0);
  // goal: the score changed since this match was last shown -> green tint for 8 s
  const uint32_t sum = (uint32_t)(m.home.score + 1) * 100 + (uint32_t)(m.away.score + 1);
  bool flash = false, known = false;
  for (int k = 0; k < 16; ++k) if (flashId[k] == m.id) {
    if (flashScore[k] != sum && m.state == ST_IN) flashUntil[k] = millis() + 8000;
    flashScore[k] = sum; flash = (int32_t)(flashUntil[k] - millis()) > 0; known = true; break;
  }
  if (!known) { flashId[nextFlash] = m.id; flashScore[nextFlash] = sum; flashUntil[nextFlash] = 0; nextFlash = (nextFlash + 1) % 16; }
  lv_obj_set_style_bg_color(c.card, lv_color_hex(flash ? 0x163A22 : CARD), 0);
  lv_obj_set_style_border_color(c.card, lv_color_hex(hi ? 0xFFC43C : CARD_EDGE), 0);
  lv_obj_set_style_border_width(c.card, hi ? 2 : 1, 0);
  // state pill
  if (m.state == ST_IN) {
    const bool ht = !strcmp(m.clock, "Intervalo");
    snprintf(b, sizeof b, "%s%s", ht ? "" : "\xE2\x80\xA2 AO VIVO  ", m.clock);
    setLabelIfChanged(c.pill, b);
    lv_obj_set_style_bg_opa(c.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c.pill, lv_color_hex(ht ? 0x7A5A0A : 0xA0141E), 0);
    lv_obj_set_style_text_color(c.pill, lv_color_hex(ht ? 0xFFECB4 : 0xFFFFFF), 0);
  } else {
    if (m.state == ST_POST) {                                // "Encerrado · Sáb 15/11"
      char when[24]; fmtWhen(m.start, when, sizeof when);
      char *dot = strstr(when, " \xC2\xB7 ");                // keep the day, drop the time
      if (dot) *dot = 0;
      snprintf(b, sizeof b, "%s \xC2\xB7 %s", m.clock, when);
    } else fmtWhen(m.start, b, sizeof b);
    setLabelIfChanged(c.pill, b);
    lv_obj_set_style_bg_opa(c.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c.pill, lv_color_hex(0x1C2434), 0);
    lv_obj_set_style_text_color(c.pill, lv_color_hex(m.state == ST_POST ? 0xAAB7CC : 0xDCE6F8), 0);
  }
}

static void renderList() {
  const FData &d = *ui;
  const FMatch *pick[NCARDS]; int n = 0;
  bool anyLive = false;
  if (tab == 0) {
    // Ao vivo: live games (by importance); then upcoming games in the next 48 h, widened to
    // 96 h and then 7 days when there are none; then finished games of the last 3 days.
    const FMatch *live[L_COUNT * MAX_M], *next[L_COUNT * MAX_M], *done[L_COUNT * MAX_M];
    int nl = 0, nn = 0, nd = 0;
    const uint32_t now = (uint32_t)time(nullptr);
    for (int L = 0; L < L_COUNT; ++L)
      for (int i = 0; i < d.n[L]; ++i) {
        const FMatch &m = d.m[L][i];
        if (m.state == ST_IN) live[nl++] = &m;
        else if (m.state == ST_POST && now > 1700000000 && m.start + 14 * 86400UL > now) done[nd++] = &m;   // last round
      }
    anyLive = nl > 0;
    static const uint32_t kHorizonH[3] = {48, 96, 168};
    int used = 0;
    for (int pass = 0; pass < 3 && !nn; ++pass) {
      used = pass;
      for (int L = 0; L < L_COUNT; ++L)
        for (int i = 0; i < d.n[L]; ++i) {
          const FMatch &m = d.m[L][i];
          if (m.state == ST_PRE && m.start > now && m.start - now < kHorizonH[pass] * 3600UL) next[nn++] = &m;
        }
    }
    auto sortBy = [](const FMatch **v, int k, int mode) {   // 0 importance, 1 soonest, 2 latest
      for (int i = 1; i < k; ++i)
        for (int j = i; j > 0; --j) {
          const FMatch *a = v[j - 1], *b = v[j];
          bool swap;
          if (mode == 0) { const int ia = importance(*a), ib = importance(*b); swap = ia < ib || (ia == ib && a->start > b->start); }
          else if (mode == 1) { const int ia = importance(*a) >= 1000, ib = importance(*b) >= 1000; swap = ia < ib || (ia == ib && a->start > b->start); }
          else swap = a->start < b->start;
          if (!swap) break;
          v[j - 1] = b; v[j] = a;
        }
    };
    sortBy(live, nl, 0); sortBy(next, nn, 1); sortBy(done, nd, 2);
    for (int i = 0; i < nl && n < NCARDS; ++i) pick[n++] = live[i];
    for (int i = 0; i < nn && n < NCARDS; ++i) pick[n++] = next[i];
    for (int i = 0; i < nd && n < NCARDS; ++i) pick[n++] = done[i];
    static const char *const kWin[3] = {"pr\xC3\xB3ximas 48 h", "pr\xC3\xB3ximos 4 dias", "pr\xC3\xB3ximos 7 dias"};
    char msg[96];
    if (anyLive || !d.updatedMs) msg[0] = 0;
    else if (nn) snprintf(msg, sizeof msg, "Nenhum jogo ao vivo  \xC2\xB7  %s", kWin[used]);
    else if (nd) snprintf(msg, sizeof msg, "Sem jogos nos pr\xC3\xB3ximos 7 dias  \xC2\xB7  \xC3\xBAltimos resultados");
    else snprintf(msg, sizeof msg, "Nenhum jogo ao vivo nem nos pr\xC3\xB3ximos 7 dias");
    setLabelIfChanged(emptyLbl, !d.updatedMs ? "Carregando jogos..." : msg);
  } else {
    const int L1 = kTabLeague[tab], L2 = kTabExtra[tab];
    if (L2 < 0) { for (int i = 0; i < d.n[L1] && n < NCARDS; ++i) pick[n++] = &d.m[L1][i]; }
    else {                                                  // two competitions: merge in the same order fetchLeague uses
      const FMatch *all[2 * MAX_M]; int k = 0;
      for (int i = 0; i < d.n[L1]; ++i) all[k++] = &d.m[L1][i];
      for (int i = 0; i < d.n[L2]; ++i) all[k++] = &d.m[L2][i];
      auto before = [](const FMatch *a, const FMatch *b) {  // live, then upcoming (soonest), then finished (latest)
        static const uint8_t rank[3] = {1, 0, 2};
        if (rank[a->state] != rank[b->state]) return rank[a->state] < rank[b->state];
        return a->state == ST_POST ? a->start > b->start : a->start < b->start;
      };
      for (int i = 1; i < k; ++i)
        for (int j = i; j > 0 && before(all[j], all[j - 1]); --j) { const FMatch *t = all[j]; all[j] = all[j - 1]; all[j - 1] = t; }
      for (int i = 0; i < k && n < NCARDS; ++i) pick[n++] = all[i];
    }
    setLabelIfChanged(emptyLbl, n ? "" : (d.updatedMs ? "Sem jogos nesta semana" : "Carregando jogos..."));
  }
  show(emptyLbl, lv_label_get_text(emptyLbl)[0] != 0);
  for (int i = 0; i < NCARDS; ++i) {
    if (i < n) { fillCard(cards[i], *pick[i], tab == 0 && anyLive && i == 0, d.logoVersion); show(cards[i].card, true); }
    else show(cards[i].card, false);
  }
}

static void styleTabs() {
  for (int i = 0; i < NTAB; ++i) {
    const bool on = i == tab;
    lv_obj_set_style_bg_opa(tabBtn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(tabTxt[i], lv_color_hex(on ? 0xFFFFFF : 0xB4C3DC), 0);
  }
}
// tab icons: the competitions' ESPN logos (Mundial: club world cup, else world cup)
static void refreshTabIcons(uint32_t logoVersion) {
  for (int i = 1; i < NTAB; ++i) {
    const lv_image_dsc_t *d = crestFor(leagueImgId(kTabLeague[i]), logoVersion);
    if (!d && kTabExtra[i] >= 0) d = crestFor(leagueImgId(kTabExtra[i]), logoVersion);
    static uint32_t loggedVer[NTAB];
    if (!d && loggedVer[i] != logoVersion + 1) {           // diagnostics: once per logo version and tab
      loggedVer[i] = logoVersion + 1;
      char path[24]; crestPath(leagueImgId(kTabLeague[i]), path, sizeof path);
      if (LittleFS.exists(path)) Log.printf("Futebol: aba %s: arquivo %s existe mas n\xC3\xA3o carregou\n", kLeagueShort[kTabLeague[i]], path);
    }
    if (d) { lv_image_set_src(tabImg[i], d); show(tabImg[i], true); show(tabTxt[i], false); }
    else { show(tabImg[i], false); show(tabTxt[i], true); }
  }
}
static void tabEvent(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i == tab) return;
  tab = (uint8_t)i;
  prefs.putUChar("futTab", tab);
  styleTabs();
  lv_obj_scroll_to_view(tabBtn[tab], LV_ANIM_ON);           // carousel: bring the chosen tab fully into view
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
  footballRender();
}

void footballSetFavorites(const char *favList) {          // web page (loop thread)
  String s = favList ? favList : ""; s.toUpperCase(); s.replace(" ", "");
  copyStr(s.c_str(), favorites, sizeof favorites);
  prefs.putString("futFav", favorites);
  shownVersion = UINT32_MAX;
}
String footballFavorites() { return String(favorites); }

void buildFootballScreen(lv_obj_t *scr) {
  lock = xSemaphoreCreateMutex();
  if (prefs.isKey("futFav")) copyStr(prefs.getString("futFav", "").c_str(), favorites, sizeof favorites);
  tab = prefs.isKey("futTab") ? prefs.getUChar("futTab", 0) : 0;
  if (tab >= NTAB) tab = 0;
  footballView = lv_obj_create(scr);
  lv_obj_set_size(footballView, SCR_W, SCR_H); lv_obj_set_pos(footballView, 0, 0);
  lv_obj_set_style_bg_color(footballView, lv_color_hex(BG_TOP), 0);
  lv_obj_set_style_bg_grad_color(footballView, lv_color_hex(BG_BOT), 0);
  lv_obj_set_style_bg_grad_dir(footballView, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(footballView, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(footballView, 0, 0); lv_obj_set_style_pad_all(footballView, 0, 0);
  lv_obj_set_style_radius(footballView, 0, 0);
  lv_obj_remove_flag(footballView, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = makeLabel(footballView, "Futebol", &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(title, 56, 10);
  statusLbl = makeLabel(footballView, "", &lv_font_weather_14, 0x8CA0BE);
  lv_obj_set_width(statusLbl, 250); lv_obj_set_style_text_align(statusLbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(statusLbl, SCR_W - 14 - 250, 14);

  // tab bar: "Ao vivo" (text) + one button per competition showing its ESPN logo
  tabBar = lv_obj_create(footballView);
  lv_obj_set_size(tabBar, SCR_W - 24, 44); lv_obj_set_pos(tabBar, 12, 44);
  lv_obj_set_style_bg_color(tabBar, lv_color_hex(0x0C162E), 0);
  lv_obj_set_style_bg_opa(tabBar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(0x1E2D50), 0); lv_obj_set_style_border_width(tabBar, 1, 0);
  lv_obj_set_style_radius(tabBar, 22, 0);
  lv_obj_set_style_pad_all(tabBar, 4, 0); lv_obj_set_style_pad_column(tabBar, 6, 0);
  lv_obj_set_flex_flow(tabBar, LV_FLEX_FLOW_ROW);
  // carousel: drag sideways to reach INT / SUL / UEL (horizontal only, no scrollbar, no chaining to the list)
  lv_obj_set_scroll_dir(tabBar, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(tabBar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(tabBar, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_remove_flag(tabBar, LV_OBJ_FLAG_SCROLL_ELASTIC);
  static const char *const kTabTxt[NTAB] = {"Ao vivo", "UCL", "LIB", "MUN", "COPA", "SUL", "UEL"};   // until the logos arrive
  for (int i = 0; i < NTAB; ++i) {
    lv_obj_t *b = tabBtn[i] = lv_button_create(tabBar);
    lv_obj_set_size(b, i == 0 ? 132 : 98, 34);
    lv_obj_set_style_radius(b, 17, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E6BFF), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, tabEvent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tabTxt[i] = makeLabel(b, kTabTxt[i], &lv_font_weather_20, 0xB4C3DC);
    lv_obj_align(tabTxt[i], LV_ALIGN_CENTER, i == 0 ? 7 : 0, 0);
    tabImg[i] = lv_image_create(b);
    lv_obj_center(tabImg[i]);
    lv_obj_remove_flag(tabImg[i], LV_OBJ_FLAG_CLICKABLE);
    show(tabImg[i], false);
  }
  lv_obj_t *dot = lv_obj_create(tabBtn[0]);                  // red "live" dot
  lv_obj_set_size(dot, 9, 9); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF5050), 0); lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);
  styleTabs();
  lv_obj_update_layout(tabBar);
  lv_obj_scroll_to_view(tabBtn[tab], LV_ANIM_OFF);          // saved tab may be off-screen

  list = lv_obj_create(footballView);
  lv_obj_set_size(list, SCR_W, SCR_H - 96); lv_obj_set_pos(list, 0, 96);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_style_pad_hor(list, 12, 0); lv_obj_set_style_pad_top(list, 0, 0); lv_obj_set_style_pad_bottom(list, 14, 0);
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x1E4FD8), LV_PART_SCROLLBAR);

  emptyLbl = makeLabel(list, "Carregando jogos...", &lv_font_weather_20, 0x9AAECC);
  lv_obj_set_width(emptyLbl, CARD_W); lv_obj_set_style_text_align(emptyLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_ver(emptyLbl, 6, 0);
  for (int i = 0; i < NCARDS; ++i) buildCard(i);

  lv_obj_move_foreground(addBackButton(footballView));     // standard back button (main.cpp)
  lv_obj_add_flag(footballView, LV_OBJ_FLAG_HIDDEN);
}

void footballOnEnter() {
  if (!shared) shared = (FData *)heap_caps_calloc(1, sizeof(FData), MALLOC_CAP_SPIRAM);
  if (!ui) ui = (FData *)heap_caps_calloc(1, sizeof(FData), MALLOC_CAP_SPIRAM);
  if (!crests) crests = (Crest *)heap_caps_calloc(NCREST, sizeof(Crest), MALLOC_CAP_SPIRAM);
  shownVersion = UINT32_MAX; shownTab = 255;
  footballRender();
}
void footballOnExit() {                                  // give the crest images back (PSRAM)
  if (!crests) return;
  for (int i = 0; i < NCARDS; ++i) {                      // nothing may point at the freed pixels
    lv_image_set_src(cards[i].crestImg[0], NULL); lv_image_set_src(cards[i].crestImg[1], NULL);
    lv_image_set_src(cards[i].lgImg, NULL);
  }
  for (int i = 1; i < NTAB; ++i) { lv_image_set_src(tabImg[i], NULL); show(tabImg[i], false); show(tabTxt[i], true); }
  for (int i = 0; i < NCREST; ++i) if (crests[i].buf) heap_caps_free(crests[i].buf);
  heap_caps_free(crests); crests = nullptr;
  shownLogoVersion = UINT32_MAX;
}

// UI tick (500 ms while shown)
void footballRender() {
  if (!ui || !shared || !footballView || lv_obj_has_flag(footballView, LV_OBJ_FLAG_HIDDEN)) return;
  if (xSemaphoreTake(lock, 0) == pdTRUE) {
    if (shared->version != ui->version) memcpy(ui, shared, sizeof *ui);
    xSemaphoreGive(lock);
  }
  char b[64];
  if (ui->msg[0] && !ui->updatedMs) snprintf(b, sizeof b, "%s", ui->msg);
  else if (!ui->updatedMs) snprintf(b, sizeof b, "conectando \xC3\xA0 ESPN...");
  else {
    const uint32_t s = (millis() - ui->updatedMs) / 1000;
    if (s < 60) snprintf(b, sizeof b, "\xE2\x80\xA2 atualizado h\xC3\xA1 %lu s", (unsigned long)s);
    else snprintf(b, sizeof b, "\xE2\x80\xA2 atualizado h\xC3\xA1 %lu min", (unsigned long)(s / 60));
  }
  setLabelIfChanged(statusLbl, b);
  lv_obj_set_style_text_color(statusLbl, lv_color_hex(ui->msg[0] ? 0xFFD477 : 0x8CA0BE), 0);
  if (ui->version == shownVersion && tab == shownTab && ui->logoVersion == shownLogoVersion) return;
  shownVersion = ui->version; shownTab = tab; shownLogoVersion = ui->logoVersion;
  refreshTabIcons(ui->logoVersion);
  renderList();
}
