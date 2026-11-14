// Futebol network side (network task only): ESPN scoreboards of each competition,
// team crests and competition logos (PNG -> RGB565A8 files on LittleFS).
#include "app_screens.h"
#include "modules/football/football.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"
#include "modules/football/football_internal.h"
#include "modules/photos/photos.h"

namespace football {
SemaphoreHandle_t lock;
FData *shared;
const char *const kLeagueShort[L_COUNT] = {"UCL", "LIB", "MUN", "COPA", "SUL", "UEL", "INT"};   // until the logo arrives
}  // namespace football
using namespace football;

static FData *net;                         // built by the network task (PSRAM)
static const char *const kSlug[L_COUNT] = {"uefa.champions", "conmebol.libertadores", "fifa.cwc", "fifa.world", "conmebol.sudamericana", "uefa.europa", "fifa.intercontinental_cup"};

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
void football::crestPath(uint32_t id, char *out, size_t n) { snprintf(out, n, "/fl3/%lu.bin", (unsigned long)id); }

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
