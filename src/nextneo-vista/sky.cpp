// Sky screen ("Céu"): the real sky over the place configured in "Cidade" (cfg.lat/lon),
// computed on the board (no network): 519 brightest stars (mag <= 4), constellation lines,
// Sun, Moon (with phase) and the naked-eye planets, seen from a mountain top towards one
// of 8 directions (N, NE, L, SE, S, SO, O, NO). Swipe or tap the side arrows to turn.
//
// Everything is drawn ONCE into an RGB565 canvas in PSRAM (anti-aliased by hand, like the
// runways on the radar map): no LVGL object per star. Redrawn when the direction changes
// and once a minute (the sky turns 0.25 deg/min). Algorithms: Paul Schlyter's "How to
// compute planetary positions" (Sun, Moon with main perturbations, planets) - accuracy
// ~0.1-0.5 deg, less than one pixel here (4.2 px per degree).
#include "app_screens.h"
#include "sky_data.h"
#include "services/app_log.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <WiFiClientSecure.h>
#include "services/https_fetch.h"

lv_obj_t *skyView;
static lv_obj_t *canvas;
static uint16_t *buf;                                   // SCR_W x SCR_H RGB565 (PSRAM)
static uint16_t *bg;                                    // same size: sky without satellites
static int dirIdx = 4;                                  // 0 = N ... 4 = S ... 7 = NO
static int renderedDir = -1;
static long renderedMinute = -1;
static lv_point_t pressAt;
static lv_obj_t *satLbl;                                // satellites status, bottom right

static constexpr float PX = 3.9f;                       // px per degree: ~123 x 73 deg view
static constexpr int HOR = 286;                         // horizon line (y)
static const char *kDir[8] = {"N", "NE", "L", "SE", "S", "SO", "O", "NO"};

// ---------------------------------------------------------------- astronomy (Schlyter)
static constexpr double RAD = M_PI / 180.0, DEG = 180.0 / M_PI;
static double rev(double x) { x = fmod(x, 360.0); return x < 0 ? x + 360 : x; }
static double kepler(double M, double e) {
  double E = M + DEG * e * sin(M * RAD) * (1 + e * cos(M * RAD));
  for (int i = 0; i < 6; ++i) E -= (E - DEG * e * sin(E * RAD) - M) / (1 - e * cos(E * RAD));
  return E;
}
struct Vec { double x, y, z; };
static void sunEcl(double d, double &lon, double &r, double &M, double &w) {
  w = 282.9404 + 4.70935E-5 * d; const double e = 0.016709 - 1.151E-9 * d; M = rev(356.0470 + 0.9856002585 * d);
  const double E = kepler(M, e), xv = cos(E * RAD) - e, yv = sqrt(1 - e * e) * sin(E * RAD);
  lon = rev(atan2(yv, xv) * DEG + w); r = sqrt(xv * xv + yv * yv);
}
static void eclToEq(Vec &v, double d) {
  const double ecl = (23.4393 - 3.563E-7 * d) * RAD, y = v.y, z = v.z;
  v.y = y * cos(ecl) - z * sin(ecl); v.z = y * sin(ecl) + z * cos(ecl);
}
static void toRaDec(const Vec &v, double &ra, double &dec) { ra = rev(atan2(v.y, v.x) * DEG); dec = atan2(v.z, sqrt(v.x * v.x + v.y * v.y)) * DEG; }
struct Orbit { double N0, N1, i0, i1, w0, w1, a, e0, e1, M0, M1; };
static const Orbit kOrb[5] = {           // Mercury, Venus, Mars, Jupiter, Saturn
  {48.3313, 3.24587E-5, 7.0047, 5.00E-8, 29.1241, 1.01444E-5, 0.387098, 0.205635, 5.59E-10, 168.6562, 4.0923344368},
  {76.6799, 2.46590E-5, 3.3946, 2.75E-8, 54.8910, 1.38374E-5, 0.723330, 0.006773, -1.302E-9, 48.0052, 1.6021302244},
  {49.5574, 2.11081E-5, 1.8497, -1.78E-8, 286.5016, 2.92961E-5, 1.523688, 0.093405, 2.516E-9, 18.6021, 0.5240207766},
  {100.4542, 2.76854E-5, 1.3030, -1.557E-7, 273.8777, 1.64505E-5, 5.20256, 0.048498, 4.469E-9, 19.8950, 0.0830853001},
  {113.6634, 2.38980E-5, 2.4886, -1.081E-7, 339.3939, 2.97661E-5, 9.55475, 0.055546, -9.499E-9, 316.9670, 0.0334442282}};
static Vec orbitPos(double N, double i, double w, double a, double e, double M) {
  const double E = kepler(M, e), xv = a * (cos(E * RAD) - e), yv = a * sqrt(1 - e * e) * sin(E * RAD);
  const double v = atan2(yv, xv) * DEG, r = sqrt(xv * xv + yv * yv), vw = (v + w) * RAD;
  return {r * (cos(N * RAD) * cos(vw) - sin(N * RAD) * sin(vw) * cos(i * RAD)),
          r * (sin(N * RAD) * cos(vw) + cos(N * RAD) * sin(vw) * cos(i * RAD)), r * sin(vw) * sin(i * RAD)};
}
static void planetRaDec(int k, double d, double &ra, double &dec, double &dist) {
  const Orbit &o = kOrb[k];
  Vec p = orbitPos(o.N0 + o.N1 * d, o.i0 + o.i1 * d, o.w0 + o.w1 * d, o.a, o.e0 + o.e1 * d, rev(o.M0 + o.M1 * d));
  double lon, r, M, w; sunEcl(d, lon, r, M, w);
  p.x += r * cos(lon * RAD); p.y += r * sin(lon * RAD);   // heliocentric -> geocentric
  dist = sqrt(p.x * p.x + p.y * p.y + p.z * p.z);          // AU
  eclToEq(p, d); toRaDec(p, ra, dec);
}
static void sunRaDec(double d, double &ra, double &dec, double &lon, double &r) {
  double M, w; sunEcl(d, lon, r, M, w);
  Vec v = {r * cos(lon * RAD), r * sin(lon * RAD), 0}; eclToEq(v, d); toRaDec(v, ra, dec);
}
static void moonRaDec(double d, double &ra, double &dec, double &lon, double &km) {
  const double N = 125.1228 - 0.0529538083 * d, w = 318.0634 + 0.1643573223 * d, M = rev(115.3654 + 13.0649929509 * d);
  Vec p = orbitPos(N, 5.1454, w, 60.2666, 0.054900, M);
  km = sqrt(p.x * p.x + p.y * p.y + p.z * p.z) * 6371.0;   // Earth radii -> km
  lon = atan2(p.y, p.x) * DEG; double lat = atan2(p.z, sqrt(p.x * p.x + p.y * p.y)) * DEG;
  double sl, sr, Ms, ws; sunEcl(d, sl, sr, Ms, ws);
  const double Ls = rev(Ms + ws), Lm = rev(N + w + M), D = rev(Lm - Ls), F = rev(Lm - N);
  auto s = [](double x) { return sin(x * RAD); };
  lon += -1.274 * s(M - 2 * D) + 0.658 * s(2 * D) - 0.186 * s(Ms) - 0.059 * s(2 * M - 2 * D) - 0.057 * s(M - 2 * D + Ms) +
         0.053 * s(M + 2 * D) + 0.046 * s(2 * D - Ms) + 0.041 * s(M - Ms) - 0.035 * s(D) - 0.031 * s(M + Ms);
  lat += -0.173 * s(F - 2 * D) - 0.055 * s(M - F - 2 * D) - 0.046 * s(M + F - 2 * D) + 0.033 * s(F + 2 * D);
  lon = rev(lon);
  Vec v = {cos(lon * RAD) * cos(lat * RAD), sin(lon * RAD) * cos(lat * RAD), sin(lat * RAD)};
  eclToEq(v, d); toRaDec(v, ra, dec);
}
// equatorial (deg) -> altitude / azimuth (deg, azimuth from North clockwise)
static void altAz(double raDeg, double decDeg, double lstDeg, double lat, float &alt, float &az) {
  const double H = (lstDeg - raDeg) * RAD, dc = decDeg * RAD, ph = lat * RAD;
  alt = (float)(asin(sin(ph) * sin(dc) + cos(ph) * cos(dc) * cos(H)) * DEG);
  az = (float)rev(atan2(sin(H), cos(H) * sin(ph) - tan(dc) * cos(ph)) * DEG + 180);
}

// ---------------------------------------------------------------- raster helpers (RGB565)
static inline uint16_t c565(uint32_t c) { return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3)); }
static uint16_t *tgt;                                   // where blend/disc/glow/line draw
static int tgtW, tgtH;
static inline void blend(int x, int y, uint32_t col, float a) {
  if (x < 0 || y < 0 || x >= tgtW || y >= tgtH || a <= 0) return;
  uint16_t &d = tgt[y * tgtW + x];
  const int ia = (int)((a > 1 ? 1 : a) * 256);
  const int sr = (col >> 16) & 0xFF, sg = (col >> 8) & 0xFF, sb = col & 0xFF;
  const int dr = (d >> 11) << 3, dg = ((d >> 5) & 0x3F) << 2, db = (d & 0x1F) << 3;
  const int r = (sr * ia + dr * (256 - ia)) >> 8, g = (sg * ia + dg * (256 - ia)) >> 8, b = (sb * ia + db * (256 - ia)) >> 8;
  d = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void disc(float cx, float cy, float r, uint32_t col, float alpha) {        // anti-aliased dot
  for (int y = (int)floorf(cy - r - 1); y <= (int)ceilf(cy + r + 1); ++y)
    for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
      if (cov > 0) blend(x, y, col, (cov > 1 ? 1 : cov) * alpha);
    }
}
static void ring(float cx, float cy, float r, uint32_t col, float alpha) {     // 1.2 px circle outline
  for (int y = (int)floorf(cy - r - 2); y <= (int)ceilf(cy + r + 2); ++y)
    for (int x = (int)floorf(cx - r - 2); x <= (int)ceilf(cx + r + 2); ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = 1.1f - fabsf(sqrtf(dx * dx + dy * dy) - r);
      if (cov > 0) blend(x, y, col, fminf(1, cov) * alpha);
    }
}
static void glow(float cx, float cy, float R, uint32_t col, float alpha) {        // soft halo
  for (int y = (int)(cy - R); y <= (int)(cy + R); ++y)
    for (int x = (int)(cx - R); x <= (int)(cx + R); ++x) {
      const float t = 1 - sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / R;
      if (t > 0) blend(x, y, col, t * t * alpha);
    }
}
static void line(float x0, float y0, float x1, float y1, uint32_t col, float alpha) {   // 1 px, anti-aliased
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  const int steps = (int)(len * 1.5f);
  for (int i = 0; i <= steps; ++i) {
    const float x = x0 + dx * i / steps, y = y0 + dy * i / steps;
    const int xi = (int)floorf(x), yi = (int)floorf(y); const float fx = x - xi, fy = y - yi;
    const float a = alpha / 1.5f;
    blend(xi, yi, col, (1 - fx) * (1 - fy) * a); blend(xi + 1, yi, col, fx * (1 - fy) * a);
    blend(xi, yi + 1, col, (1 - fx) * fy * a); blend(xi + 1, yi + 1, col, fx * fy * a);
  }
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  auto ch = [&](int s) { return (uint32_t)lroundf(((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t) << s; };
  return ch(16) | ch(8) | ch(0);
}
static uint32_t starColor(int bv50) {                   // B-V colour index -> RGB
  const float bv = bv50 / 50.0f;
  if (bv < 0.0f) return mix(0xAFC4FF, 0xDCE6FF, (bv + 0.4f) / 0.4f);
  if (bv < 0.6f) return mix(0xDCE6FF, 0xFFF4E0, bv / 0.6f);
  if (bv < 1.2f) return mix(0xFFF4E0, 0xFFC890, (bv - 0.6f) / 0.6f);
  return mix(0xFFC890, 0xFF9A66, (bv - 1.2f) / 0.8f);
}

// ---------------------------------------------------------------- scene
struct Label { int x, y; const char *txt; uint32_t col; bool bold; };
static Label labels[48];
static float gSunAlt = -90;                             // Sun altitude at the last full redraw
// Sun, Moon and planets as drawn (for the tap -> info panel)
enum { B_MERCURY, B_VENUS, B_MARS, B_JUPITER, B_SATURN, B_SUN, B_MOON, B_COUNT };
struct Body { bool shown; float x, y, alt, az; double dist; };
static Body bodies[B_COUNT];
static float moonElong;                                 // 0 new, 90 first quarter, 180 full
static float moonSunDx = 1, moonSunDy = 0;              // on-screen direction Moon -> Sun
// Lit part of the Moon: the limb facing the Sun; terminator is an ellipse set by the phase.
static bool moonLit(float fx, float fy, float R) {
  const float u = fx * moonSunDx + fy * moonSunDy, v = -fx * moonSunDy + fy * moonSunDx;
  const float h = sqrtf(fmaxf(0, R * R - v * v));
  return u >= h * cosf(moonElong * (float)RAD);
}
static int labelN;
static bool showNames = true;                           // tap in the middle of the sky: names on/off
static void addLabel(float x, float y, const char *t, uint32_t col, bool bold) {
  if (!showNames) return;
  if (labelN < 48 && x > -20 && x < SCR_W - 10 && y > 2 && y < HOR - 6) labels[labelN++] = {(int)x, (int)y, t, col, bold};
}

static void drawScene() {
  const time_t now = time(nullptr);
  const double lat = cfg.lat, lon = cfg.lon;
  struct tm g; gmtime_r(&now, &g);
  const int y = g.tm_year + 1900, m = g.tm_mon + 1, dd = g.tm_mday;
  const double ut = g.tm_hour + g.tm_min / 60.0 + g.tm_sec / 3600.0;
  const double d = 367L * y - (7L * (y + ((m + 9) / 12))) / 4 + (275L * m) / 9 + dd - 730530 + ut / 24.0;
  const double jd = now / 86400.0 + 2440587.5;
  const double lst = rev((18.697374558 + 24.06570982441908 * (jd - 2451545.0)) * 15.0 + lon);
  const float az0 = dirIdx * 45.0f;
  auto proj = [&](float alt, float az, float &x, float &py) {
    float da = fmodf(az - az0 + 540.0f, 360.0f) - 180.0f;
    // x shrinks with cos(alt) (equal-area dome): near the zenith the whole circle of azimuths
    // fits on screen, so the top of the view is as full of stars as the horizon
    x = SCR_W / 2.0f + da * PX * cosf(alt * (float)RAD); py = HOR - alt * PX;
  };
  labelN = 0;
  tgt = buf; tgtW = SCR_W; tgtH = SCR_H;
  for (Body &b : bodies) b.shown = false;

  // Sun first: it sets the sky colour and how many stars are visible
  double sra, sdec, slon, sdist; sunRaDec(d, sra, sdec, slon, sdist);
  float sunAlt, sunAz; altAz(sra, sdec, lst, lat, sunAlt, sunAz);
  gSunAlt = sunAlt;
  struct Stop { float alt; uint32_t top, bot; };
  static const Stop kSky[] = {{10, 0x2F6CC0, 0x9CCBF0}, {0, 0x2A4F8E, 0xE8A070}, {-6, 0x14284F, 0x6A4A6A},
                              {-12, 0x070F25, 0x29304F}, {-18, 0x03060F, 0x1A2440}};
  uint32_t top = kSky[4].top, bot = kSky[4].bot;
  for (int k = 0; k < 4; ++k)
    if (sunAlt <= kSky[k].alt && sunAlt >= kSky[k + 1].alt) {
      const float t = (kSky[k].alt - sunAlt) / (kSky[k].alt - kSky[k + 1].alt);
      top = mix(kSky[k].top, kSky[k + 1].top, t); bot = mix(kSky[k].bot, kSky[k + 1].bot, t);
    }
  if (sunAlt > 10) { top = kSky[0].top; bot = kSky[0].bot; }
  // brighter sky -> fewer stars: mag limit 4.0 at night, ~1 in deep twilight, none by day
  const float magLim = sunAlt > -3 ? -9 : fminf(4.0f, -1.5f + (-sunAlt) * 0.33f);
  static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int py = 0; py < SCR_H; ++py) {                   // gradient with 4x4 dithering: no visible bands
    const float t = powf(fminf(1, fmaxf(0, py / (float)HOR)), 2.2f);
    const float r = ((top >> 16) & 0xFF) * (1 - t) + ((bot >> 16) & 0xFF) * t;
    const float g = ((top >> 8) & 0xFF) * (1 - t) + ((bot >> 8) & 0xFF) * t;
    const float b = (top & 0xFF) * (1 - t) + (bot & 0xFF) * t;
    uint16_t *row = buf + py * SCR_W;
    for (int x = 0; x < SCR_W; ++x) {
      const float k = (kBayer[py & 3][x & 3] + 0.5f) / 16.0f;
      const int r5 = (int)fminf(31, r / 8 + k), g6 = (int)fminf(63, g / 4 + k), b5 = (int)fminf(31, b / 8 + k);
      row[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
  }

  // stars + constellation lines
  static float sx[SKY_STARS], sy[SKY_STARS], salt[SKY_STARS];
  for (int i = 0; i < SKY_STARS; ++i) {
    const SkyStar &s = kSkyStars[i];
    float a, z; altAz(s.ra * 360.0 / 65535.0, s.dec * 90.0 / 32767.0, lst, lat, a, z);
    salt[i] = a; proj(a, z, sx[i], sy[i]);
  }
  if (magLim > 0.5f)
    for (int k = 0; k < SKY_LINES; ++k) {
      const int a = kSkyLines[k][0], b = kSkyLines[k][1];
      if (salt[a] < -3 && salt[b] < -3) continue;
      if (fabsf(sx[a] - sx[b]) > SCR_W / 2) continue;      // wraps around behind the viewer
      if ((sx[a] < -60 && sx[b] < -60) || (sx[a] > SCR_W + 60 && sx[b] > SCR_W + 60)) continue;
      line(sx[a], sy[a], sx[b], sy[b], 0x7896D2, 0.38f);
    }
  for (int i = SKY_STARS - 1; i >= 0; --i) {             // faint first, bright on top
    const float mag = kSkyStars[i].mag / 20.0f - 2.0f;
    if (salt[i] < 0 || mag > magLim || sx[i] < -8 || sx[i] > SCR_W + 8 || sy[i] < 0) continue;
    const uint32_t col = starColor(kSkyStars[i].bv);
    const float r = fmaxf(0.55f, 2.7f - 0.48f * mag), a = fminf(1, fmaxf(0.35f, 1.0f - (mag - 1.5f) * 0.18f));
    if (mag < 1.2f) glow(sx[i], sy[i], r * 4.5f, col, 0.22f);
    disc(sx[i], sy[i], r, col, a);
  }
  for (int k = 0; k < SKY_NAMES; ++k) {
    const int i = kSkyNames[k].star;
    const float mag = kSkyStars[i].mag / 20.0f - 2.0f;
    if (salt[i] >= 0 && mag <= magLim && sy[i] > 0) addLabel(sx[i] + 5, sy[i] - 8, kSkyNames[k].name, 0x9FB3D6, false);
  }

  // planets
  static const char *kPlanet[5] = {"Merc\xC3\xBArio", "V\xC3\xAAnus", "Marte", "J\xC3\xBApiter", "Saturno"};
  static const uint32_t kPCol[5] = {0xD8D0C0, 0xFFF8E0, 0xFF8A5C, 0xFFE7B8, 0xF2DFA0};
  static const float kPMag[5] = {0.0f, -4.2f, 0.8f, -2.3f, 0.6f};
  for (int k = 0; k < 5; ++k) {
    double ra, de, dist; planetRaDec(k, d, ra, de, dist);
    float a, z, x, py; altAz(ra, de, lst, lat, a, z); proj(a, z, x, py);
    if (a < 0 || kPMag[k] > magLim + 1.5f || x < -10 || x > SCR_W + 10 || py < 0) continue;
    bodies[k] = {true, x, py, a, z, dist};
    glow(x, py, 13, kPCol[k], 0.35f);
    disc(x, py, 3.2f, kPCol[k], 1);
    addLabel(x + 7, py - 9, kPlanet[k], kPCol[k], true);
  }

  // Sun (by day / twilight) and Moon with its phase
  { float x, py; proj(sunAlt, sunAz, x, py);
    if (sunAlt > -1 && x > -30 && x < SCR_W + 30) { bodies[B_SUN] = {true, x, py, sunAlt, sunAz, sdist}; glow(x, py, 40, 0xFFE9A8, 0.6f); disc(x, py, 10, 0xFFF2C8, 1); addLabel(x + 14, py - 9, "Sol", 0xFFE9A8, true); } }
  double mra, mde, mlon, mkm; moonRaDec(d, mra, mde, mlon, mkm);
  moonElong = (float)rev(mlon - slon);
  float ma, mz, mx, my; altAz(mra, mde, lst, lat, ma, mz); proj(ma, mz, mx, my);
  if (ma > -1 && mx > -20 && mx < SCR_W + 20) {
    const float R = 9;
    { float sxp, syp; proj(sunAlt, sunAz, sxp, syp);
      const float ddx = sxp - mx, ddy = syp - my, n = sqrtf(ddx * ddx + ddy * ddy);
      if (n > 1) { moonSunDx = ddx / n; moonSunDy = ddy / n; } }
    bodies[B_MOON] = {true, mx, my, ma, mz, mkm};
    glow(mx, my, 26, 0xE8E4D0, 0.25f);
    for (int yy = -10; yy <= 10; ++yy)
      for (int xx = -10; xx <= 10; ++xx) {
        const float fx = xx + 0.5f, fy = yy + 0.5f, dist = sqrtf(fx * fx + fy * fy), cov = R + 0.5f - dist;
        if (cov <= 0) continue;
        const bool lit = moonLit(fx, fy, R);
        blend((int)mx + xx, (int)my + yy, lit ? 0xF0ECDC : 0x3C404A, fminf(1, cov));
      }
    addLabel(mx + 13, my - 9, "Lua", 0xE8E4D0, true);
  }

  // mountains + ground
  for (int x = 0; x < SCR_W; ++x) {
    const float azx = az0 + (x - SCR_W / 2.0f) / PX;
    const float h1 = 8 + 16 * (0.5f + 0.3f * sinf((azx * 3) * (float)RAD) + 0.2f * sinf((azx * 7.3f) * (float)RAD));
    const float h2 = 11 * (0.5f + 0.3f * sinf((azx * 3 + 40) * (float)RAD) + 0.2f * sinf((azx * 7.3f + 13) * (float)RAD));
    for (int py = (int)(HOR + 4 - h1); py < SCR_H; ++py) buf[py * SCR_W + x] = c565(py < HOR + 4 - h2 ? 0x0E1422 : 0x070A12);
  }
  for (int py = HOR + 4; py < SCR_H; ++py) for (int x = 0; x < SCR_W; ++x) buf[py * SCR_W + x] = c565(0x05080E);
  for (int k = 0; k < 16; ++k) {                         // ticks every 22.5 deg
    float x, py; proj(0, k * 22.5f, x, py);
    if (x < -2 || x > SCR_W + 2) continue;
    const int len = k % 2 ? 4 : 7;
    for (int t = 0; t < len; ++t) blend((int)x, HOR + 4 + t, 0x7890AA, 1);
  }

  // text on top (LVGL draws onto the canvas buffer)
  lv_layer_t layer; lv_canvas_init_layer(canvas, &layer);
  lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
  for (int i = 0; i < labelN; ++i) {
    ld.font = &lv_font_weather_14; ld.color = lv_color_hex(labels[i].col); ld.text = labels[i].txt;
    lv_area_t a = {labels[i].x, labels[i].y, labels[i].x + 120, labels[i].y + 18};
    lv_draw_label(&layer, &ld, &a);
  }
  for (int k = 0; k < 8; ++k) {                          // N, NE, L, ... under the horizon
    float x, py; proj(0, k * 45.0f, x, py);
    if (x < -10 || x > SCR_W + 10) continue;
    ld.font = &lv_font_weather_14; ld.text = kDir[k]; ld.align = LV_TEXT_ALIGN_CENTER;
    ld.color = lv_color_hex(k == dirIdx ? 0xFFD54A : 0xAAB9D2);
    lv_area_t a = {(int)x - 20, HOR + 12, (int)x + 20, HOR + 30}; lv_draw_label(&layer, &ld, &a);
  }
  ld.align = LV_TEXT_ALIGN_AUTO;
  lv_canvas_finish_layer(canvas, &layer);
  if (bg) memcpy(bg, buf, SCR_W * SCR_H * 2);           // satellites are drawn over this copy
  lv_obj_invalidate(canvas);
}

// ---------------------------------------------------------------- satellites (CelesTrak TLE + SGP4)
// Downloaded only while this screen is open, at most every 6 h (CelesTrak asks for no more than
// that): ISS + Tiangong (group "stations"), the ~150 brightest ("visual") and the Starlinks
// launched in the last 30 days ("trains"). Positions are computed here with SGP4 (near-Earth
// model, WGS-72), so the dots move in real time without more network.
enum { SAT_ISS, SAT_CSS, SAT_VISUAL, SAT_STARLINK };
struct Sat {
  char name[22]; uint8_t kind; bool up;                   // up: near/above the horizon (checked in turns)
  double epochJd, no, ecco, inclo, nodeo, argpo, mo, bstar;
  // SGP4 init constants
  bool isimp; double aycof, con41, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao, t2cof, t3cof,
      t4cof, t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf;
};
static constexpr int MAX_SATS = 700;
static Sat *sats; static int satN;                        // UI thread
static Sat *volatile satPending; static volatile int satPendingN;   // handed over by the network task
static volatile uint32_t satFetchedMs; static volatile bool satEverFetched, satFetching;

static constexpr double XKE = 0.0743669161331734, RE_KM = 6378.135, J2 = 0.001082616, J3OJ2 = -0.00000253881 / 0.001082616,
                        J4 = -0.00000165597, K_2PI = 2 * M_PI;

static void sgp4Init(Sat &s) {
  const double x2o3 = 2.0 / 3.0, cosio = cos(s.inclo), cosio2 = cosio * cosio, eccsq = s.ecco * s.ecco;
  const double omeosq = 1 - eccsq, rteosq = sqrt(omeosq);
  const double ak = pow(XKE / s.no, x2o3), d1 = 0.75 * J2 * (3 * cosio2 - 1) / (rteosq * omeosq);
  double del = d1 / (ak * ak);
  const double adel = ak * (1 - del * del - del * (1.0 / 3.0 + 134 * del * del / 81));
  del = d1 / (adel * adel);
  s.no = s.no / (1 + del);
  const double ao = pow(XKE / s.no, x2o3), sinio = sin(s.inclo), po = ao * omeosq, con42 = 1 - 5 * cosio2;
  s.con41 = -con42 - cosio2 - cosio2;
  const double posq = po * po, rp = ao * (1 - s.ecco);
  s.isimp = rp < (220 / RE_KM + 1);
  double sfour = 78 / RE_KM + 1, qzms24 = pow((120 - 78) / RE_KM, 4);
  const double perige = (rp - 1) * RE_KM;
  if (perige < 156) {
    sfour = perige < 98 ? 20 : perige - 78;
    qzms24 = pow((120 - sfour) / RE_KM, 4); sfour = sfour / RE_KM + 1;
  }
  const double pinvsq = 1 / posq, tsi = 1 / (ao - sfour);
  s.eta = ao * s.ecco * tsi;
  const double etasq = s.eta * s.eta, eeta = s.ecco * s.eta, psisq = fabs(1 - etasq), coef = qzms24 * pow(tsi, 4),
               coef1 = coef / pow(psisq, 3.5);
  const double cc2 = coef1 * s.no * (ao * (1 + 1.5 * etasq + eeta * (4 + etasq)) +
                                     0.375 * J2 * tsi / psisq * s.con41 * (8 + 3 * etasq * (8 + etasq)));
  s.cc1 = s.bstar * cc2;
  const double cc3 = s.ecco > 1e-4 ? -2 * coef * tsi * J3OJ2 * s.no * sinio / s.ecco : 0;
  s.x1mth2 = 1 - cosio2;
  s.cc4 = 2 * s.no * coef1 * ao * omeosq *
          (s.eta * (2 + 0.5 * etasq) + s.ecco * (0.5 + 2 * etasq) -
           J2 * tsi / (ao * psisq) * (-3 * s.con41 * (1 - 2 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                                      0.75 * s.x1mth2 * (2 * etasq - eeta * (1 + etasq)) * cos(2 * s.argpo)));
  s.cc5 = 2 * coef1 * ao * omeosq * (1 + 2.75 * (etasq + eeta) + eeta * etasq);
  const double cosio4 = cosio2 * cosio2, temp1 = 1.5 * J2 * pinvsq * s.no, temp2 = 0.5 * temp1 * J2 * pinvsq,
               temp3 = -0.46875 * J4 * pinvsq * pinvsq * s.no;
  s.mdot = s.no + 0.5 * temp1 * rteosq * s.con41 + 0.0625 * temp2 * rteosq * (13 - 78 * cosio2 + 137 * cosio4);
  s.argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7 - 114 * cosio2 + 395 * cosio4) + temp3 * (3 - 36 * cosio2 + 49 * cosio4);
  const double xhdot1 = -temp1 * cosio;
  s.nodedot = xhdot1 + (0.5 * temp2 * (4 - 19 * cosio2) + 2 * temp3 * (3 - 7 * cosio2)) * cosio;
  s.omgcof = s.bstar * cc3 * cos(s.argpo);
  s.xmcof = s.ecco > 1e-4 ? -x2o3 * coef * s.bstar / eeta : 0;
  s.nodecf = 3.5 * omeosq * xhdot1 * s.cc1;
  s.t2cof = 1.5 * s.cc1;
  s.xlcof = -0.25 * J3OJ2 * sinio * (3 + 5 * cosio) / (fabs(cosio + 1) > 1.5e-12 ? 1 + cosio : 1.5e-12);
  s.aycof = -0.5 * J3OJ2 * sinio;
  s.delmo = pow(1 + s.eta * cos(s.mo), 3);
  s.sinmao = sin(s.mo);
  s.x7thm1 = 7 * cosio2 - 1;
  s.d2 = s.d3 = s.d4 = s.t3cof = s.t4cof = s.t5cof = 0;
  if (!s.isimp) {
    const double cc1sq = s.cc1 * s.cc1;
    s.d2 = 4 * ao * tsi * cc1sq;
    const double temp = s.d2 * tsi * s.cc1 / 3;
    s.d3 = (17 * ao + sfour) * temp;
    s.d4 = 0.5 * temp * ao * tsi * (221 * ao + 31 * sfour) * s.cc1;
    s.t3cof = s.d2 + 2 * cc1sq;
    s.t4cof = 0.25 * (3 * s.d3 + s.cc1 * (12 * s.d2 + 10 * cc1sq));
    s.t5cof = 0.2 * (3 * s.d4 + 12 * s.cc1 * s.d3 + 6 * s.d2 * s.d2 + 15 * cc1sq * (2 * s.d2 + cc1sq));
  }
}

// Position (km, TEME frame) t minutes after the TLE epoch. false if decayed / invalid.
static bool sgp4(const Sat &s, double t, double r[3]) {
  const double xmdf = s.mo + s.mdot * t, argpdf = s.argpo + s.argpdot * t, nodedf = s.nodeo + s.nodedot * t, t2 = t * t;
  double argpm = argpdf, mm = xmdf, nodem = nodedf + s.nodecf * t2;
  double tempa = 1 - s.cc1 * t, tempe = s.bstar * s.cc4 * t, templ = s.t2cof * t2;
  if (!s.isimp) {
    const double delomg = s.omgcof * t, dm = 1 + s.eta * cos(xmdf);
    const double delm = s.xmcof * (dm * dm * dm - s.delmo), temp = delomg + delm, t3 = t2 * t, t4 = t3 * t;
    mm = xmdf + temp; argpm = argpdf - temp;
    tempa -= s.d2 * t2 + s.d3 * t3 + s.d4 * t4;
    tempe += s.bstar * s.cc5 * (sin(mm) - s.sinmao);
    templ += s.t3cof * t3 + t4 * (s.t4cof + t * s.t5cof);
  }
  const double am = pow(XKE / s.no, 2.0 / 3.0) * tempa * tempa;
  double em = s.ecco - tempe;
  if (em >= 1 || em < -0.001 || am < 0.95) return false;
  if (em < 1e-6) em = 1e-6;
  mm += s.no * templ;
  double xlm = fmod(mm + argpm + nodem, K_2PI);
  nodem = fmod(nodem, K_2PI); argpm = fmod(argpm, K_2PI);
  mm = fmod(xlm - argpm - nodem, K_2PI);
  const double sinip = sin(s.inclo), cosip = cos(s.inclo);
  const double axnl = em * cos(argpm), temp0 = 1 / (am * (1 - em * em)), aynl = em * sin(argpm) + temp0 * s.aycof;
  const double xl = mm + argpm + nodem + temp0 * s.xlcof * axnl;
  const double u = fmod(xl - nodem, K_2PI);
  double eo1 = u, tem5 = 9999, sineo1 = 0, coseo1 = 1;
  for (int k = 0; fabs(tem5) >= 1e-12 && k < 10; ++k) {
    sineo1 = sin(eo1); coseo1 = cos(eo1);
    tem5 = 1 - coseo1 * axnl - sineo1 * aynl;
    tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
    if (fabs(tem5) >= 0.95) tem5 = tem5 > 0 ? 0.95 : -0.95;
    eo1 += tem5;
  }
  const double ecose = axnl * coseo1 + aynl * sineo1, esine = axnl * sineo1 - aynl * coseo1;
  const double el2 = axnl * axnl + aynl * aynl, pl = am * (1 - el2);
  if (pl < 0) return false;
  const double rl = am * (1 - ecose), betal = sqrt(1 - el2), temp = esine / (1 + betal);
  const double sinu = am / rl * (sineo1 - aynl - axnl * temp), cosu = am / rl * (coseo1 - axnl + aynl * temp);
  double su = atan2(sinu, cosu);
  const double sin2u = (cosu + cosu) * sinu, cos2u = 1 - 2 * sinu * sinu, tp = 1 / pl, temp1 = 0.5 * J2 * tp, temp2 = temp1 * tp;
  const double mrt = rl * (1 - 1.5 * temp2 * betal * s.con41) + 0.5 * temp1 * s.x1mth2 * cos2u;
  if (mrt < 1) return false;
  su -= 0.25 * temp2 * s.x7thm1 * sin2u;
  const double xnode = nodem + 1.5 * temp2 * cosip * sin2u, xinc = s.inclo + 1.5 * temp2 * cosip * sinip * cos2u;
  const double sinsu = sin(su), cossu = cos(su), snod = sin(xnode), cnod = cos(xnode), sini = sin(xinc), cosi = cos(xinc);
  const double xmx = -snod * cosi, xmy = cnod * cosi;
  r[0] = mrt * (xmx * sinsu + cnod * cossu) * RE_KM;
  r[1] = mrt * (xmy * sinsu + snod * cossu) * RE_KM;
  r[2] = mrt * (sini * sinsu) * RE_KM;
  return true;
}

struct SatView { float alt, az, rangeKm, heightKm; bool lit; };
// TEME position -> what the observer sees (alt/az), plus whether the Sun lights it.
static bool satLook(const Sat &s, double jd, SatView &v) {
  double r[3];
  if (!sgp4(s, (jd - s.epochJd) * 1440.0, r)) return false;
  const double g = rev(280.46061837 + 360.98564736629 * (jd - 2451545.0)) * RAD;      // GMST
  const double x = r[0] * cos(g) + r[1] * sin(g), y = -r[0] * sin(g) + r[1] * cos(g), z = r[2];
  const double la = cfg.lat * RAD, lo = cfg.lon * RAD, e2 = 0.00669438, N = 6378.137 / sqrt(1 - e2 * sin(la) * sin(la));
  const double ox = (N + 0.76) * cos(la) * cos(lo), oy = (N + 0.76) * cos(la) * sin(lo), oz = (N * (1 - e2) + 0.76) * sin(la);
  const double dx = x - ox, dy = y - oy, dz = z - oz;
  const double S = sin(la) * cos(lo) * dx + sin(la) * sin(lo) * dy - cos(la) * dz;
  const double E = -sin(lo) * dx + cos(lo) * dy;
  const double Z = cos(la) * cos(lo) * dx + cos(la) * sin(lo) * dy + sin(la) * dz;
  const double range = sqrt(dx * dx + dy * dy + dz * dz);
  v.alt = (float)(asin(Z / range) * DEG); v.az = (float)rev(atan2(E, -S) * DEG);
  v.rangeKm = (float)range;
  const double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  v.heightKm = (float)(rr - RE_KM);
  // Earth's shadow as a cylinder behind the Earth (good enough at these heights)
  double lon, sr, M, w; sunEcl(jd - 2451543.5, lon, sr, M, w);
  const double ecl = 23.4393 * RAD, sx = cos(lon * RAD), sy = sin(lon * RAD) * cos(ecl), sz = sin(lon * RAD) * sin(ecl);
  const double p = r[0] * sx + r[1] * sy + r[2] * sz;
  if (p > 0) v.lit = true;
  else {
    const double qx = r[0] - p * sx, qy = r[1] - p * sy, qz = r[2] - p * sz;
    v.lit = sqrt(qx * qx + qy * qy + qz * qz) > RE_KM;
  }
  return true;
}

// ---- TLE download + parsing (network task)
static double tleField(const char *l, int a, int b) { char t[24]; int n = b - a + 1; memcpy(t, l + a, n); t[n] = 0; return atof(t); }
static double tleExp(const char *l, int a) {             // " 12345-3" -> 0.12345e-3
  char m[8]; memcpy(m, l + a + 1, 5); m[5] = 0;
  const char ex[3] = {l[a + 6], l[a + 7], 0};
  const double v = atof(m) * 1e-5 * pow(10.0, atoi(ex));
  return l[a] == '-' ? -v : v;
}
static double jdOfYearDay(int yy, double day) {
  const int y = yy < 57 ? 2000 + yy : 1900 + yy;
  const long a = (14 - 1) / 12, Y = y + 4800 - a, m = 1 + 12 * a - 3;           // Jan 1 -> JDN
  const long jdn = 1 + (153 * m + 2) / 5 + 365 * Y + Y / 4 - Y / 100 + Y / 400 - 32045;
  return jdn - 0.5 + (day - 1);
}
static int parseTle(const char *txt, Sat *out, int n, int cap, uint8_t mode) {
  // mode: 0 = stations (ISS + Tiangong only), 1 = visual, 2 = last-30-days (Starlink only)
  const char *p = txt;
  char l0[80], l1[80], l2[80];
  auto getLine = [&](char *dst) -> bool {
    if (!*p) return false;
    int k = 0;
    while (*p && *p != '\n') { if (*p != '\r' && k < 79) dst[k++] = *p; ++p; }
    if (*p) ++p;
    dst[k] = 0; return true;
  };
  while (n < cap && getLine(l0)) {
    if (l0[0] == '1' && l0[1] == ' ') continue;
    if (!getLine(l1) || !getLine(l2)) break;
    if (l1[0] != '1' || l2[0] != '2' || strlen(l1) < 69 || strlen(l2) < 63) continue;
    const int cat = (int)tleField(l1, 2, 6);
    const double revDay = tleField(l2, 52, 62);
    if (revDay < 6.4) continue;                           // deep space (GEO, Molniya...): skip
    uint8_t kind = SAT_VISUAL;
    if (mode == 0) { if (cat == 25544) kind = SAT_ISS; else if (cat == 48274) kind = SAT_CSS; else continue; }
    if (mode == 2) { if (strncmp(l0, "STARLINK", 8)) continue; kind = SAT_STARLINK; }
    Sat &s = out[n];
    memset(&s, 0, sizeof s);
    int e = strlen(l0); while (e > 0 && l0[e - 1] == ' ') --e; l0[e] = 0;
    snprintf(s.name, sizeof s.name, "%s", kind == SAT_ISS ? "ISS" : kind == SAT_CSS ? "Tiangong" : l0);
    if (mode == 1 && (cat == 25544 || cat == 48274)) continue;   // already from "stations"
    s.kind = kind;
    s.epochJd = jdOfYearDay((int)tleField(l1, 18, 19), tleField(l1, 20, 31));
    s.bstar = tleExp(l1, 53);
    s.inclo = tleField(l2, 8, 15) * RAD; s.nodeo = tleField(l2, 17, 24) * RAD;
    char ec[12] = "0."; memcpy(ec + 2, l2 + 26, 7); ec[9] = 0; s.ecco = atof(ec);
    s.argpo = tleField(l2, 34, 41) * RAD; s.mo = tleField(l2, 43, 50) * RAD;
    s.no = revDay * K_2PI / 1440.0;
    s.up = true;
    sgp4Init(s);
    ++n;
  }
  return n;
}

// Network task (CMD_SKY). Quick no-op while the data is fresh.
bool skyFetch() {
  if (satEverFetched && millis() - satFetchedMs < 6UL * 3600UL * 1000UL) return true;
  static const char *kUrl[3] = {"https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle",
                                "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle",
                                "https://celestrak.org/NORAD/elements/gp.php?GROUP=last-30-days&FORMAT=tle"};
  satFetching = true;
  Sat *list = (Sat *)heap_caps_malloc(sizeof(Sat) * MAX_SATS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!list) { satFetching = false; return false; }
  static PsramBuffer body(600 * 1024);
  int n = 0, okCount = 0;
  for (int k = 0; k < 3; ++k) {
    if (!screenJobStillCurrent()) break;
    if (!tlsMemoryAvailable("celestrak")) continue;
    body.clear();
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(8);
    const uint32_t t0 = millis();
    const int status = httpsFetch(client, kUrl[k], body, 15000, false);
    client.stop();
    const int before = n;
    if (status == 200 && !body.overflowed()) { n = parseTle(body.c_str(), list, n, MAX_SATS, k); ++okCount; }
    Log.printf("C\xC3\xA9u: CelesTrak %s HTTP %d, %u bytes, +%d sat\xC3\xA9lites (%lums)\n",
               k == 0 ? "stations" : k == 1 ? "visual" : "last-30-days", status, (unsigned)body.length(), n - before,
               (unsigned long)(millis() - t0));
  }
  satFetching = false;
  if (!okCount || !n) { heap_caps_free(list); return false; }
  while (satPending) delay(20);                           // UI has not taken the previous list yet
  satPendingN = n; satPending = list;
  satFetchedMs = millis(); satEverFetched = true;
  return true;
}

// ---- drawing (UI thread)
struct SatShown { int idx; float x, y; SatView v; };
static SatShown shown[48]; static int shownN;
static int scanPos;                                       // round-robin "is it up?" check
static uint32_t satLastSec;
static bool satsOnScreen;
static int shadowN;                                       // shown but not lit (hollow circles)

static void satAdopt() {
  if (!satPending) return;
  if (sats) heap_caps_free(sats);
  sats = satPending; satN = satPendingN; satPending = nullptr; scanPos = 0;
  LogV.printf("C\xC3\xA9u: %d sat\xC3\xA9lites carregados\n", satN);
}
static void projXY(float alt, float az, float &x, float &py) {
  const float da = fmodf(az - dirIdx * 45.0f + 540.0f, 360.0f) - 180.0f;
  x = SCR_W / 2.0f + da * PX * cosf(alt * (float)RAD); py = HOR - alt * PX;
}
static void satPass(bool force) {
  const time_t now = time(nullptr);
  if (!force && (uint32_t)now == satLastSec) return;
  satLastSec = (uint32_t)now;
  if (!sats || !satN || !bg) return;
  const double jd = now / 86400.0 + 2440587.5;
  for (int k = 0; k < 40 && satN; ++k) {                  // refresh the "up" flags little by little
    Sat &s = sats[scanPos]; SatView v;
    s.up = s.kind <= SAT_CSS || (satLook(s, jd, v) && v.alt > -8);
    scanPos = (scanPos + 1) % satN;
  }
  shownN = 0; shadowN = 0;
  memcpy(buf, bg, SCR_W * SCR_H * 2);
  tgt = buf; tgtW = SCR_W; tgtH = SCR_H;
  lv_layer_t layer; bool layerOpen = false;
  lv_draw_label_dsc_t ld;
  for (int i = 0; i < satN && shownN < 48; ++i) {
    Sat &s = sats[i];
    if (!s.up) continue;
    SatView v;
    if (!satLook(s, jd, v) || v.alt < 0) continue;
    float x, y; projXY(v.alt, v.az, x, y);
    if (x < -5 || x > SCR_W + 5 || y < 0 || y > HOR) continue;
    const bool bright = v.lit, big = s.kind <= SAT_CSS;
    // direction of motion (from where it was 30 s ago): orients the solar panels
    float ux = 1, uy = 0;
    SatView pv; float px, py;
    if (satLook(s, jd - 30.0 / 86400.0, pv) && pv.alt > -2) {
      projXY(pv.alt, pv.az, px, py);
      const float ddx = x - px, ddy = y - py, n = sqrtf(ddx * ddx + ddy * ddy);
      if (n > 0.5f && n < 160) {
        ux = ddx / n; uy = ddy / n;
      }
    }
    if (bright) {
      // lit: a tiny spacecraft (body + two solar panels across the motion), not a star-like dot
      const float qx = -uy, qy = ux, w = big ? 9 : 6, b0 = big ? 3.2f : 2.4f;
      glow(x, y, big ? 14 : 9, 0xCFE0FF, big ? 0.45f : 0.32f);
      for (int k = -1; k <= 1; ++k) {                      // panels: 3 parallel lines = ~3 px wide
        const float ox = ux * k * 0.8f, oy = uy * k * 0.8f;
        line(x + qx * b0 + ox, y + qy * b0 + oy, x + qx * (b0 + w) + ox, y + qy * (b0 + w) + oy, 0x7FB0FF, 1);
        line(x - qx * b0 + ox, y - qy * b0 + oy, x - qx * (b0 + w) + ox, y - qy * (b0 + w) + oy, 0x7FB0FF, 1);
      }
      disc(x, y, big ? 2.6f : 1.9f, 0xFFFFFF, 1);            // body
      if (big) disc(x + ux * 3, y + uy * 3, 1.3f, 0xFFF4D8, 1);   // ISS/Tiangong: longer body
      if (now & 1) {                                       // blinking red light, like a "REC" lamp (1 s on / 1 s off)
        const float lx = x - ux * (big ? 5 : 4), ly = y - uy * (big ? 5 : 4);
        glow(lx, ly, 5, 0xFF2A2A, 0.6f);
        disc(lx, ly, big ? 1.7f : 1.4f, 0xFF3B30, 1);
      }
    } else {
      ring(x, y, big ? 4.5f : 3.2f, 0x9AAACB, 0.9f); disc(x, y, 1.0f, 0x9AAACB, 0.9f);   // in shadow: hollow
    }
    if (!bright) ++shadowN;
    shown[shownN++] = {i, x, y, v};
    if (big && showNames) {                              // name only for ISS / Tiangong
      if (!layerOpen) { lv_canvas_init_layer(canvas, &layer); lv_draw_label_dsc_init(&ld); layerOpen = true; }
      ld.font = &lv_font_weather_14; ld.color = lv_color_hex(bright ? 0xFFFFFF : 0x9FB3D6); ld.text = s.name;
      lv_area_t a = {(int)x + 7, (int)y - 9, (int)x + 120, (int)y + 9}; lv_draw_label(&layer, &ld, &a);
    }
  }
  if (layerOpen) lv_canvas_finish_layer(canvas, &layer);
  if (shownN || satsOnScreen) lv_obj_invalidate(canvas);  // nothing moved -> no redraw
  satsOnScreen = shownN > 0;
}

// ---------------------------------------------------------------- info panel (tap on Sun/Moon/planet)
// Card: picture on the left, name + subtitle, then a 2 x 3 grid of LABEL / value.
static lv_obj_t *infoPanel, *infoArt, *infoTitle, *infoSub, *infoKey[6], *infoVal[6];
static uint16_t *artBuf;                                // ART x ART RGB565 (PSRAM)
static lv_timer_t *infoTimer;
static constexpr int ART = 116;
static constexpr uint32_t C_CARD = 0x0B1426;

struct BodyInfo { const char *name, *sub, *diam, *yearKey, *year; };
static const BodyInfo kInfo[B_COUNT] = {
  {"Merc\xC3\xBArio", "Planeta  \xC2\xB7  o mais perto do Sol", "4.879 km", "ANO", "88 dias"},
  {"V\xC3\xAAnus", "Planeta  \xC2\xB7  o mais brilhante do c\xC3\xA9u", "12.104 km", "ANO", "225 dias"},
  {"Marte", "Planeta  \xC2\xB7  o planeta vermelho", "6.779 km", "ANO", "687 dias"},
  {"J\xC3\xBApiter", "Planeta  \xC2\xB7  o maior do sistema solar", "139.820 km", "ANO", "11,9 anos"},
  {"Saturno", "Planeta  \xC2\xB7  an\xC3\xA9is de gelo e rocha", "116.460 km", "ANO", "29,5 anos"},
  {"Sol", "Estrela  \xC2\xB7  an\xC3\xA3 amarela", "1,39 mi de km", "IDADE", "4,6 bi de anos"},
  {"Lua", "Sat\xC3\xA9lite natural da Terra", "3.474 km", "\xC3\x93RBITA", "27,3 dias"}};

static uint32_t shadeCol(uint32_t c, float k) {
  k = fmaxf(0, fminf(1.2f, k));
  auto ch = [&](int sh) { return (uint32_t)fminf(255, ((c >> sh) & 0xFF) * k) << sh; };
  return ch(16) | ch(8) | ch(0);
}
static float smooth01(float e0, float e1, float x) { float t = fminf(1, fmaxf(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); }
// soft spot: 1 at the centre, 0 outside radius r (unit-disc coordinates)
static float spot(float fx, float fy, float cx, float cy, float rx, float ry) {
  const float dx = (fx - cx) / rx, dy = (fy - cy) / ry; return 1 - smooth01(0.55f, 1.0f, sqrtf(dx * dx + dy * dy));
}

// Smooth, lit sphere (light from the upper left), surface patterns made of soft shapes.
static void drawSphere(int kind, float cx, float cy, float R) {
  for (int y = (int)(cy - R - 1); y <= (int)(cy + R + 1); ++y)
    for (int x = (int)(cx - R - 1); x <= (int)(cx + R + 1); ++x) {
      const float fx = (x + 0.5f - cx) / R, fy = (y + 0.5f - cy) / R, rr = fx * fx + fy * fy;
      const float cov = (1 - sqrtf(rr)) * R + 0.5f;
      if (cov <= 0) continue;
      const float fz = sqrtf(fmaxf(0, 1 - rr));
      const float diff = fmaxf(0, -0.42f * fx - 0.36f * fy + 0.83f * fz);
      float light = 0.10f + 1.0f * powf(diff, 0.8f);
      uint32_t c = 0;
      switch (kind) {
        case B_MERCURY: {
          c = mix(0xB2ACA2, 0x8A847B, 0.5f + 0.25f * sinf(fx * 4 + fy * 3) + 0.25f * sinf(fy * 6 - fx * 2));
          static const float cr[5][3] = {{-0.3f, -0.2f, 0.16f}, {0.35f, 0.25f, 0.12f}, {0.1f, -0.5f, 0.10f}, {-0.45f, 0.4f, 0.13f}, {0.5f, -0.25f, 0.08f}};
          for (auto &k : cr) c = mix(c, 0x6E6962, 0.6f * spot(fx, fy, k[0], k[1], k[2], k[2]));
        } break;
        case B_VENUS: c = mix(0xF6ECCB, 0xE0C68E, 0.5f + 0.35f * sinf(fy * 5 + 1.5f * sinf(fx * 3))); break;
        case B_MARS:
          c = mix(0xD2683F, 0xA4492A, 0.5f + 0.3f * sinf(fx * 5 + fy * 2) + 0.2f * sinf(fy * 7));
          c = mix(c, 0x7A3420, 0.7f * spot(fx, fy, 0.1f, 0.15f, 0.45f, 0.18f));        // dark plains
          c = mix(c, 0xF6F2EC, smooth01(-0.78f, -0.9f, fy));                            // polar cap
          break;
        case B_JUPITER: {
          static const uint32_t band[7] = {0xD8C6A8, 0xBE8E62, 0xF0E4CC, 0xC59366, 0xF2E8D2, 0xB98A60, 0xDCCBAE};
          const float t = (fy + 1) * 3 + 0.08f * sinf(fx * 6);
          const int i = (int)fminf(5, fmaxf(0, t)); c = mix(band[i], band[i + 1], smooth01(0, 1, t - i));
          c = mix(c, 0xC4603E, spot(fx, fy, 0.3f, 0.32f, 0.2f, 0.1f));                 // Great Red Spot
        } break;
        case B_SATURN: {
          const float t = (fy + 1) * 2.5f; const int i = (int)fminf(3, fmaxf(0, t));
          static const uint32_t band[5] = {0xD9C18A, 0xEBDAA8, 0xE2CB92, 0xF0E0B4, 0xD3B880};
          c = mix(band[i], band[i + 1], smooth01(0, 1, t - i));
        } break;
        case B_SUN: c = mix(0xFFF8DC, 0xFF9A2E, powf(rr, 1.3f)); light = 1; break;
        case B_MOON: {
          c = 0xD9D5CA;
          static const float ma[5][4] = {{-0.25f, -0.3f, 0.32f, 0.22f}, {0.2f, -0.35f, 0.2f, 0.16f}, {0.3f, 0.0f, 0.22f, 0.28f},
                                         {-0.35f, 0.25f, 0.2f, 0.3f}, {0.05f, 0.35f, 0.15f, 0.12f}};
          for (auto &k : ma) c = mix(c, 0x8D8A84, 0.75f * spot(fx, fy, k[0], k[1], k[2], k[3]));
          light = moonLit(fx, fy, 1) ? 0.35f + 0.8f * fz : 0.07f;
        } break;
      }
      blend(x, y, shadeCol(c, light), fminf(1, cov));
    }
}
static void drawRing(float cx, float cy, float R, bool front) {   // Saturn's rings, tilted
  for (int y = (int)(cy - R); y <= (int)(cy + R); ++y)
    for (int x = (int)(cx - 2.2f * R); x <= (int)(cx + 2.2f * R); ++x) {
      const float fx = (x + 0.5f - cx) / R, fy = (y + 0.5f - cy) / (R * 0.34f);
      const float r = sqrtf(fx * fx + fy * fy);
      if (r < 1.25f || r > 2.1f || (fy > 0) != front) continue;
      if (!front && fx * fx + (fy * 0.34f) * (fy * 0.34f) < 1) continue;              // hidden by the planet
      float a = 0.9f * smooth01(1.25f, 1.32f, r) * (1 - smooth01(2.0f, 2.1f, r));
      a *= (r > 1.62f && r < 1.7f) ? 0.15f : 1;                                       // Cassini division
      blend(x, y, mix(0xF0DFAE, 0x9C845A, (r - 1.25f) / 0.85f), a);
    }
}
static void drawArt(int kind) {
  tgt = artBuf; tgtW = ART; tgtH = ART;
  for (int i = 0; i < ART * ART; ++i) artBuf[i] = c565(C_CARD);
  const float c = ART / 2.0f;
  switch (kind) {
    case B_SUN: glow(c, c, 56, 0xFFB040, 0.55f); drawSphere(kind, c, c, 34); break;
    case B_SATURN: drawRing(c, c, 24, false); drawSphere(kind, c, c, 24); drawRing(c, c, 24, true); break;
    default: glow(c, c, 54, kind == B_MARS ? 0xD2683F : 0x8FA6D0, 0.12f); drawSphere(kind, c, c, 42); break;
  }
  tgt = buf; tgtW = SCR_W; tgtH = SCR_H;
  lv_obj_invalidate(infoArt);
}

// simple satellite drawings for the card
static void box(float x0, float y0, float x1, float y1, uint32_t c, float a = 1) {
  for (int y = (int)y0; y < (int)y1; ++y) for (int x = (int)x0; x < (int)x1; ++x) blend(x, y, c, a);
}
static void panel(float x0, float y0, float w, float h) {               // solar array with cell grid
  box(x0, y0, x0 + w, y0 + h, 0x2B4C8C);
  for (float x = x0 + 4; x < x0 + w; x += 5) box(x, y0, x + 1, y0 + h, 0x5C80C4, 0.6f);
  box(x0, y0, x0 + w, y0 + 1, 0x9CB6E8, 0.7f);
}
static void drawSatArt(int kind) {
  tgt = artBuf; tgtW = ART; tgtH = ART;
  for (int i = 0; i < ART * ART; ++i) artBuf[i] = c565(C_CARD);
  const float c = ART / 2.0f;
  glow(c, c, 54, 0x6F8FD0, 0.10f);
  if (kind == SAT_ISS) {
    box(c - 46, c - 2, c + 46, c + 2, 0xC8CCD4);                        // main truss
    for (int k = 0; k < 4; ++k) {                                      // 8 big solar wings
      const float x = k < 2 ? c - 46 + k * 13 : c + 22 + (k - 2) * 13;
      panel(x, c - 34, 10, 30); panel(x, c + 4, 10, 30);
    }
    box(c - 6, c - 14, c + 6, c + 18, 0xE4E6EA); box(c - 14, c + 6, c + 14, c + 12, 0xD4D7DD);   // modules
    box(c - 3, c - 22, c + 3, c - 14, 0xE4E6EA);
  } else if (kind == SAT_CSS) {
    box(c - 6, c - 30, c + 6, c + 26, 0xE4E6EA);                       // Tianhe core
    box(c - 30, c - 5, c + 30, c + 5, 0xD9DCE2);                       // Wentian + Mengtian (T shape)
    panel(c - 54, c - 16, 22, 10); panel(c - 54, c + 6, 22, 10);
    panel(c + 32, c - 16, 22, 10); panel(c + 32, c + 6, 22, 10);
    box(c - 4, c + 26, c + 4, c + 34, 0xB8BCC4);
  } else if (kind == SAT_STARLINK) {
    box(c - 26, c - 4, c + 10, c + 6, 0xC4C8D0);                       // flat body
    box(c - 26, c - 4, c + 10, c - 2, 0xE8EAEE);
    panel(c + 12, c - 3, 40, 8);                                       // one long solar array
    box(c + 10, c, c + 12, c + 2, 0x9AA0AA);
  } else {
    box(c - 9, c - 12, c + 9, c + 12, 0xD8B45A);                       // gold-foil body
    box(c - 9, c - 12, c + 9, c - 9, 0xF0D27E);
    panel(c - 46, c - 8, 34, 16); panel(c + 12, c - 8, 34, 16);
    box(c - 12, c - 1, c - 9, c + 1, 0x9AA0AA); box(c + 9, c - 1, c + 12, c + 1, 0x9AA0AA);
  }
  tgt = buf; tgtW = SCR_W; tgtH = SCR_H;
  lv_obj_invalidate(infoArt);
}

static void closeInfo() {
  if (infoPanel) lv_obj_add_flag(infoPanel, LV_OBJ_FLAG_HIDDEN);
  if (infoTimer) lv_timer_pause(infoTimer);
}
static void infoTimerCb(lv_timer_t *) { closeInfo(); }

static const char *moonPhase(float e) {
  static const char *n[8] = {"Lua nova", "Crescente", "Quarto crescente", "Crescente gibosa", "Lua cheia",
                             "Minguante gibosa", "Quarto minguante", "Minguante"};
  return n[(int)((e + 22.5f) / 45.0f) % 8];
}
static void fmtBig(char *o, size_t n, double v) {               // 384400 -> "384.400"
  char t[24]; snprintf(t, sizeof t, "%.0f", v);
  const int len = strlen(t); size_t k = 0;
  for (int i = 0; i < len && k + 2 < n; ++i) { if (i && (len - i) % 3 == 0) o[k++] = '.'; o[k++] = t[i]; }
  o[k] = 0;
}
static void comma(char *t) { for (; *t; ++t) if (*t == '.') *t = ','; }
static void cell(int i, const char *key, const char *val) { lv_label_set_text(infoKey[i], key); lv_label_set_text(infoVal[i], val); }

static void openInfo(int k) {
  if (!artBuf) {
    artBuf = (uint16_t *)heap_caps_malloc(ART * ART * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!artBuf) return;
    lv_canvas_set_buffer(infoArt, artBuf, ART, ART, LV_COLOR_FORMAT_RGB565);
  }
  const Body &b = bodies[k];
  const BodyInfo &in = kInfo[k];
  drawArt(k);
  lv_label_set_text(infoTitle, in.name);
  lv_label_set_text(infoSub, in.sub);
  char v[32];
  snprintf(v, sizeof v, "%.0f\xC2\xB0", b.alt); cell(0, "ALTURA", v);
  snprintf(v, sizeof v, "%s  %.0f\xC2\xB0", kDir[((int)lroundf(b.az / 45.0f)) % 8], b.az); cell(1, "DIRE\xC3\x87\xC3\x83O", v);
  if (k == B_MOON) {
    char big[16]; fmtBig(big, sizeof big, b.dist); snprintf(v, sizeof v, "%s km", big); cell(2, "DIST\xC3\x82NCIA", v);
    cell(3, "FASE", moonPhase(moonElong));
    snprintf(v, sizeof v, "%.0f%%", 50.0f * (1 - cosf(moonElong * (float)RAD))); cell(4, "ILUMINADA", v);
    cell(5, "DI\xC3\x82METRO", in.diam);
  } else {
    snprintf(v, sizeof v, "%.0f mi de km", b.dist * 149.6); cell(2, "DIST\xC3\x82NCIA", v);
    snprintf(v, sizeof v, "%.1f min", b.dist * 8.317); comma(v); cell(3, "LUZ CHEGA EM", v);
    cell(4, "DI\xC3\x82METRO", in.diam);
    cell(5, in.yearKey, in.year);
  }
  lv_obj_remove_flag(infoPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(infoPanel);
  lv_timer_reset(infoTimer); lv_timer_resume(infoTimer);
  LogV.printf("C\xC3\xA9u: info %s\n", in.name);
}

static void openSat(int k) {
  if (!artBuf) {
    artBuf = (uint16_t *)heap_caps_malloc(ART * ART * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!artBuf) return;
    lv_canvas_set_buffer(infoArt, artBuf, ART, ART, LV_COLOR_FORMAT_RGB565);
  }
  const Sat &s = sats[shown[k].idx];
  const SatView &v = shown[k].v;
  drawSatArt(s.kind);
  lv_label_set_text(infoTitle, s.name);
  lv_label_set_text(infoSub, s.kind == SAT_ISS ? "Esta\xC3\xA7\xC3\xA3o Espacial Internacional"
                           : s.kind == SAT_CSS ? "Esta\xC3\xA7\xC3\xA3o espacial chinesa"
                           : s.kind == SAT_STARLINK ? "Starlink lan\xC3\xA7" "ado h\xC3\xA1 menos de 30 dias"
                                                    : "Um dos sat\xC3\xA9lites mais brilhantes");
  char t[32];
  snprintf(t, sizeof t, "%.0f\xC2\xB0", v.alt); cell(0, "ALTURA NO C\xC3\x89U", t);
  snprintf(t, sizeof t, "%s  %.0f\xC2\xB0", kDir[((int)lroundf(v.az / 45.0f)) % 8], v.az); cell(1, "DIRE\xC3\x87\xC3\x83O", t);
  char big[16]; fmtBig(big, sizeof big, v.rangeKm); snprintf(t, sizeof t, "%s km", big); cell(2, "DIST\xC3\x82NCIA", t);
  fmtBig(big, sizeof big, v.heightKm); snprintf(t, sizeof t, "%s km", big); cell(3, "ALTITUDE", t);
  snprintf(t, sizeof t, "%.1f km/s", sqrt(398600.8 / (RE_KM + v.heightKm))); comma(t); cell(4, "VELOCIDADE", t);
  cell(5, "LUZ DO SOL", !v.lit ? "na sombra da Terra" : gSunAlt < -6 ? "iluminado: vis\xC3\xADvel" : "iluminado (c\xC3\xA9u claro)");
  lv_obj_remove_flag(infoPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(infoPanel);
  lv_timer_reset(infoTimer); lv_timer_resume(infoTimer);
  LogV.printf("C\xC3\xA9u: info %s\n", s.name);
}

static void buildInfoPanel() {
  infoPanel = lv_obj_create(skyView);
  lv_obj_set_size(infoPanel, 452, 170); lv_obj_align(infoPanel, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_radius(infoPanel, 18, 0); lv_obj_set_style_pad_all(infoPanel, 0, 0);
  lv_obj_set_style_bg_color(infoPanel, lv_color_hex(C_CARD), 0); lv_obj_set_style_bg_opa(infoPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(infoPanel, lv_color_hex(0x2C4066), 0); lv_obj_set_style_border_width(infoPanel, 1, 0);
  lv_obj_remove_flag(infoPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(infoPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(infoPanel, [](lv_event_t *) { closeInfo(); }, LV_EVENT_CLICKED, NULL);
  infoArt = lv_canvas_create(infoPanel);
  lv_obj_set_pos(infoArt, 14, (170 - ART) / 2);
  lv_obj_remove_flag(infoArt, LV_OBJ_FLAG_CLICKABLE);
  constexpr int X0 = 144, COL = 150;
  infoTitle = makeLabel(infoPanel, "", &lv_font_weather_20, 0xF4F7FF);
  lv_obj_set_pos(infoTitle, X0, 12);
  infoSub = makeLabel(infoPanel, "", &lv_font_weather_14, 0x8EA3C6);
  lv_obj_set_pos(infoSub, X0, 37);
  lv_obj_t *sep = lv_obj_create(infoPanel);                     // thin divider
  lv_obj_set_size(sep, 452 - X0 - 16, 1); lv_obj_set_pos(sep, X0, 60);
  lv_obj_set_style_bg_color(sep, lv_color_hex(0x22355A), 0); lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_style_radius(sep, 0, 0); lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < 6; ++i) {
    const int x = X0 + (i % 2) * COL, y = 66 + (i / 2) * 32;
    infoKey[i] = makeLabel(infoPanel, "", &lv_font_weather_14, 0x6F84A8);
    lv_obj_set_pos(infoKey[i], x, y);
    infoVal[i] = makeLabel(infoPanel, "", &lv_font_weather_14, 0xECF2FC);
    lv_obj_set_pos(infoVal[i], x, y + 15);
    lv_obj_set_width(infoVal[i], COL - 8);
    lv_label_set_long_mode(infoVal[i], LV_LABEL_LONG_DOT);
  }
  lv_obj_add_flag(infoPanel, LV_OBJ_FLAG_HIDDEN);
  infoTimer = lv_timer_create(infoTimerCb, 12000, NULL);
  lv_timer_pause(infoTimer);
}

// ---------------------------------------------------------------- touch
static void turn(int step) { closeInfo(); dirIdx = (dirIdx + step + 8) % 8; renderedDir = -1; skyRender(); }
static void touchEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) { pressAt = p; return; }
  if (code != LV_EVENT_RELEASED) return;
  const int dx = p.x - pressAt.x;
  if (dx < -40) { turn(+1); return; }                     // drag the sky left = look to the right
  if (dx > 40) { turn(-1); return; }
  { int bs = -1; float bd = 18 * 18;                      // tap on a satellite
    for (int k = 0; k < shownN; ++k) {
      const float ddx = p.x - shown[k].x, ddy = p.y - shown[k].y, d2 = ddx * ddx + ddy * ddy;
      if (d2 < bd) { bd = d2; bs = k; }
    }
    if (bs >= 0) { openSat(bs); return; } }
  int best = -1; float bestD = 26 * 26;                   // tap on the Sun, Moon or a planet
  for (int k = 0; k < B_COUNT; ++k) {
    if (!bodies[k].shown) continue;
    const float ddx = p.x - bodies[k].x, ddy = p.y - bodies[k].y, d2 = ddx * ddx + ddy * ddy;
    if (d2 < bestD) { bestD = d2; best = k; }
  }
  if (best >= 0) { openInfo(best); return; }
  if (infoPanel && !lv_obj_has_flag(infoPanel, LV_OBJ_FLAG_HIDDEN)) { closeInfo(); return; }
  if (p.x > 120 && p.x < SCR_W - 120 && p.y > 50 && p.y < HOR) {   // middle: names on/off
    showNames = !showNames; renderedDir = -1; skyRender(); return;
  }
  if (p.x < 70 && p.y > 60) turn(-1);                     // tap on the side arrows
  else if (p.x > SCR_W - 70 && p.y > 60) turn(+1);
}

// ---------------------------------------------------------------- lifecycle
void buildSkyScreen(lv_obj_t *scr) {
  skyView = lv_obj_create(scr);
  lv_obj_set_size(skyView, SCR_W, SCR_H); lv_obj_set_pos(skyView, 0, 0);
  lv_obj_set_style_radius(skyView, 0, 0);
  lv_obj_set_style_border_width(skyView, 0, 0); lv_obj_set_style_pad_all(skyView, 0, 0);
  lv_obj_set_style_bg_color(skyView, lv_color_hex(0x03060F), 0); lv_obj_set_style_bg_opa(skyView, LV_OPA_COVER, 0);
  lv_obj_remove_flag(skyView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(skyView, LV_OBJ_FLAG_HIDDEN);
  canvas = lv_canvas_create(skyView);
  lv_obj_set_pos(canvas, 0, 0);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, touchEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(canvas, touchEvent, LV_EVENT_RELEASED, NULL);
  satLbl = makeLabel(skyView, "", &lv_font_weather_14, 0x8A9CBC);       // small pill at the top
  lv_obj_set_style_bg_color(satLbl, lv_color_hex(0x0A1222), 0); lv_obj_set_style_bg_opa(satLbl, LV_OPA_60, 0);
  lv_obj_set_style_radius(satLbl, 9, 0); lv_obj_set_style_pad_hor(satLbl, 9, 0); lv_obj_set_style_pad_ver(satLbl, 2, 0);
  lv_obj_align(satLbl, LV_ALIGN_TOP_MID, 0, 6);
  buildInfoPanel();
  addBackButton(skyView);                                 // standard back button (main.cpp)
}

void skyOnEnter() {
  if (!buf) {
    buf = (uint16_t *)heap_caps_malloc(SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) lv_canvas_set_buffer(canvas, buf, SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565);
  }
  if (!bg) bg = (uint16_t *)heap_caps_malloc(SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  renderedDir = -1;
  skyRender();
}

void skyOnExit() { closeInfo(); }

// UI tick (every 500 ms while the screen is shown): redraw on a new minute or direction.
void skyRender() {
  if (!buf || !skyView || lv_obj_has_flag(skyView, LV_OBJ_FLAG_HIDDEN)) return;
  satAdopt();
  const time_t now = time(nullptr);
  if (now < 1700000000) return;                           // clock not synced yet
  const long minute = (long)(now / 60);
  const bool full = minute != renderedMinute || dirIdx != renderedDir;
  if (full) {
    renderedMinute = minute; renderedDir = dirIdx;
    const uint32_t t0 = millis();
    drawScene();
    LogV.printf("C\xC3\xA9u: desenhado em %lums (%s)\n", (unsigned long)(millis() - t0), kDir[dirIdx]);
  }
  satPass(full);
  if (satLbl) {
    char t[48];
    if (satFetching) snprintf(t, sizeof t, "baixando sat\xC3\xA9lites...");
    else if (!satN) snprintf(t, sizeof t, "%s", satEverFetched ? "" : "sat\xC3\xA9lites: sem dados");
    else if (!shownN) snprintf(t, sizeof t, "nenhum sat\xC3\xA9lite nesta dire\xC3\xA7\xC3\xA3o");
    else if (shownN > shadowN) snprintf(t, sizeof t, "%d sat\xC3\xA9lite%s vis\xC3\xADve%s", shownN - shadowN,
                                        shownN - shadowN == 1 ? "" : "s", shownN - shadowN == 1 ? "l" : "is");
    else snprintf(t, sizeof t, "%d sat\xC3\xA9lite%s nesta dire\xC3\xA7\xC3\xA3o", shownN, shownN == 1 ? "" : "s");
    setLabelIfChanged(satLbl, t);
    if (t[0]) lv_obj_remove_flag(satLbl, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(satLbl, LV_OBJ_FLAG_HIDDEN);
  }
}
