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
#include "core/screen_registry.h"
#include "modules/f1/f1_internal.h"
static void f1Render();
#include "modules/photos/photos.h"
#include "services/app_log.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <JPEGDEC.h>
#include <new>

using namespace f1;

static lv_obj_t *f1View;

// ============================================================== static tables

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
static const char *kSession[S_COUNT] = {"TL1", "TL2", "TL3", "Classif. Sprint", "Sprint", "Classifica\xC3\xA7\xC3\xA3o", "CORRIDA"};
static F1Data *ui;                                        // UI thread copy

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
    if (i == p) lv_obj_set_hidden(pages[i], false); else lv_obj_set_hidden(pages[i], true);
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
                          lv_obj_set_style_border_width(r, 0, 0); lv_obj_set_style_pad_all(r, 0, 0); lv_obj_set_scrollable(r, false);
                          lv_obj_set_gesture_bubble(r, true); return r; };
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
static void closeProg() { lv_obj_set_hidden(progPanel, true); lv_timer_pause(progTimer); }
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
    lv_obj_set_hidden(progBox[k], false);
    ++k;
  }
  for (; k < S_COUNT; ++k) lv_obj_set_hidden(progBox[k], true);
  // forecast only for the race of the coming GP (not in the preview of other circuits)
  if (previewIdx < 0 && ui->wxHave[S_RACE] && !strcmp(ui->wxCid, r->cid)) {
    static char w[96];
    // main race only (not the sprint): day min/max at the circuit + the sky at the start time
    snprintf(w, sizeof w, "Previs\xC3\xA3o p/ a corrida:  m\xC3\xADn %.0f\xC2\xB0  \xC2\xB7  m\xC3\xA1x %.0f\xC2\xB0  \xC2\xB7  %s",
             ui->wxMin[S_RACE], ui->wxMax[S_RACE],
             shortSky(ui->wxCode[S_RACE], ui->wxNight[S_RACE]));
    lv_label_set_text(progRaceWx, w); lv_obj_set_hidden(progRaceWx, false);
  } else lv_obj_set_hidden(progRaceWx, true);
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
  lv_obj_set_hidden(progPanel, false); lv_obj_move_foreground(progPanel);
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
  postScreenJob(JOB_UPDATE);                                  // download that circuit if needed
}
static lv_obj_t *styleBtn, *styleIcon[2];
static const char *kStyleName[ST_COUNT] = {"ruas", "pista"};
static void mapLong(lv_event_t *) {                         // style button: streets -> drawing -> satellite
  mapStyle = (mapStyle + 1) % ST_COUNT;
  for (int i = 0; i < 2; ++i) {
    if (i == mapStyle) lv_obj_set_hidden(styleIcon[i], false); else lv_obj_set_hidden(styleIcon[i], true);
  }
  LogV.printf("F1: mapa em modo %s\n", kStyleName[mapStyle]);
  strcpy(shownMapCid, "?");                                 // redraw now (with or without the picture)
  postScreenJob(JOB_UPDATE);                                    // download the picture for this style if needed
}

static lv_obj_t *plainBox(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_scrollable(o, false); lv_obj_set_gesture_bubble(o, true);
  return o;
}

static void buildF1Screen(lv_obj_t *scr) {
  lock = xSemaphoreCreateMutex();
  f1View = lv_obj_create(scr);
  lv_obj_set_size(f1View, SCR_W, SCR_H); lv_obj_set_pos(f1View, 0, 0);
  lv_obj_set_style_radius(f1View, 0, 0); lv_obj_set_style_border_width(f1View, 0, 0); lv_obj_set_style_pad_all(f1View, 0, 0);
  lv_obj_set_style_bg_color(f1View, lv_color_hex(C_BG), 0); lv_obj_set_style_bg_opa(f1View, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(f1View, false);
  lv_obj_set_hidden(f1View, true);
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
  lv_obj_set_clickable(left, true);
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
  for (lv_obj_t *o : {lblGp, lblPlace, lblRound, lblNextKind, f1LblDate, lblTime, lblCount, lblHint}) lv_obj_set_clickable(o, false);
  mapCanvas = lv_canvas_create(p);
  lv_obj_set_pos(mapCanvas, SCR_W - MAP_W - 2, 0);
  lv_obj_set_clickable(mapCanvas, true); lv_obj_set_gesture_bubble(mapCanvas, true);
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
    lv_obj_set_clickable(styleIcon[i], false); lv_obj_set_scrollable(styleIcon[i], false);
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
  lv_obj_set_hidden(styleIcon[1], true);
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
    lv_obj_set_gesture_bubble(lists[i], true);
  }
  // weekend programme overlay
  progPanel = lv_obj_create(f1View);
  lv_obj_set_size(progPanel, 464, 308); lv_obj_align(progPanel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(progPanel, lv_color_hex(0x10151F), 0); lv_obj_set_style_bg_opa(progPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(progPanel, lv_color_hex(0x2C3850), 0); lv_obj_set_style_border_width(progPanel, 1, 0);
  lv_obj_set_style_radius(progPanel, 16, 0); lv_obj_set_style_pad_all(progPanel, 0, 0);
  lv_obj_set_scrollable(progPanel, false);
  lv_obj_set_clickable(progPanel, true);
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
    lv_obj_set_scrollable(progBox[i], false); lv_obj_set_clickable(progBox[i], false);
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
  lv_obj_set_hidden(progPanel, true);
  progTimer = lv_timer_create([](lv_timer_t *) { closeProg(); }, 20000, NULL);
  lv_timer_pause(progTimer);
  addBackButton(f1View);
  setPage(0);
}

static void f1OnEnter() {
  if (!ui) ui = (F1Data *)heap_caps_calloc(1, sizeof(F1Data), MALLOC_CAP_SPIRAM);
  if (!shared) shared = (F1Data *)heap_caps_calloc(1, sizeof(F1Data), MALLOC_CAP_SPIRAM);
  if (!mapBuf) {
    mapBuf = (uint16_t *)heap_caps_malloc(MAP_W * MAP_H * 2, MALLOC_CAP_SPIRAM);
    if (mapBuf) lv_canvas_set_buffer(mapCanvas, mapBuf, MAP_W, MAP_H, LV_COLOR_FORMAT_RGB565);
  }
  shownVersion = 0xFFFFFFFF; autoPageDone = -1; strcpy(shownMapCid, "?");
  f1Render();
}
static void f1OnExit() { closeProg(); previewIdx = -1; }

static void countdown(char *b, size_t n, long s) {
  if (s <= 0) { snprintf(b, n, "agora"); return; }
  const long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
  if (d) snprintf(b, n, "em %ldd %02ldh %02ldmin", d, h, m);
  else if (h) snprintf(b, n, "em %ldh %02ldmin", h, m);
  else snprintf(b, n, "em %ldmin %02lds", m, s % 60);
}

// UI tick (500 ms while shown)
static void f1Render() {
  if (!ui || !shared || !f1View || lv_obj_is_hidden(f1View)) return;
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
  if (progPanel && !lv_obj_is_hidden(progPanel) && ui->version != progVersion) fillProg();
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

static const NetJobDef kF1Jobs[] = {{"f1", f1Fetch, true}};
const ScreenLifecycle kF1Screen = {
  SCREEN_F1, "f1", &f1View, buildF1Screen, f1OnEnter, f1OnExit, f1Render,
  nullptr, F1_UPDATE_INTERVAL_MS, F1_RETRY_INTERVAL_MS, kF1Jobs, 1, nullptr,
};
