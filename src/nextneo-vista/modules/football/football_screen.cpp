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
#include "core/screen_registry.h"
static void footballRender();
#include "modules/football/football.h"
#include "modules/football/football_internal.h"
#include "modules/photos/photos.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "services/https_fetch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"


using namespace football;

static lv_obj_t *footballView;

// ================================================================ data
static const char *const kLeagueName[L_COUNT] = {"CHAMPIONS LEAGUE", "LIBERTADORES", "MUNDIAL DE CLUBES", "COPA DO MUNDO", "SUL-AMERICANA", "EUROPA LEAGUE", "INTERCONTINENTAL"};
// importance in "Ao vivo": UCL / Copa / LIB in a tier of their own (a phase bonus of up to +40 never lifts
// another competition above them); favourites (+1000) still come first
static const uint8_t kLeagueWeight[L_COUNT] = {220, 210, 60, 230, 40, 40, 60};

static FData *ui;                          // UI thread copy
static char favorites[64] = "";            // "FLA,PAL" (ESPN abbreviations), prefs "futFav"

// ================================================================ helpers
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

static void buildFootballScreen(lv_obj_t *scr) {
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

static void footballOnEnter() {
  if (!shared) shared = (FData *)heap_caps_calloc(1, sizeof(FData), MALLOC_CAP_SPIRAM);
  if (!ui) ui = (FData *)heap_caps_calloc(1, sizeof(FData), MALLOC_CAP_SPIRAM);
  if (!crests) crests = (Crest *)heap_caps_calloc(NCREST, sizeof(Crest), MALLOC_CAP_SPIRAM);
  shownVersion = UINT32_MAX; shownTab = 255;
  footballRender();
}
static void footballOnExit() {                                  // give the crest images back (PSRAM)
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
static void footballRender() {
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

static const NetJobDef kFootballJobs[] = {{CMD_FOOTBALL, "football", footballFetch, true}};
const ScreenLifecycle kFootballScreen = {
  SCREEN_FOOTBALL, "football", &footballView, buildFootballScreen, footballOnEnter, footballOnExit, footballRender,
  nullptr, FOOTBALL_UPDATE_INTERVAL_MS, FOOTBALL_RETRY_INTERVAL_MS, kFootballJobs, 1, footballReleaseConnection,
};
