// Clock screen ("Céu vivo"): lock-screen style layout over an animated sky
// picked from pre-rendered photo-like skies (time of day x weather, sky_images.c)
// plus live effects: rain, lightning, fog drift, twinkling and shooting stars.
// Weather comes from the same Open-Meteo job as the Weather screen and is only
// requested while this screen is active. Tap toggles a minimal black mode.
// Everything here runs on the LVGL thread.
#include <stddef.h>
#include "app_screens.h"
#include "core/screen_registry.h"
#include "modules/weather/weather.h"
#include <math.h>
#include <time.h>
#include <JPEGDEC.h>
#include <new>
// getLocalTime(&t, 0) fails at random: with a 0 ms timeout its loop is skipped whenever
// millis() ticks between two reads. Read the clock directly instead.
#include "modules/clock/sky_images.h"
#include "services/psram_alloc.h"

LV_FONT_DECLARE(lv_font_clock_120);

static lv_obj_t *clockView;

static constexpr int CLOUDS = 4;
static constexpr int STARS = 14, DROPS = 30, FOG = 3, HOURS = 5, SPLASH = 8;
static constexpr uint32_t FRAME_MS = 66;      // ~15 fps: every frame pushes the full panel

static lv_obj_t *sky, *skyVeil, *star[STARS], *shoot, *drop[DROPS], *fog[FOG], *flash, *cloud[CLOUDS];
static float cloudX[CLOUDS];
static int cloudsShown = 0;
static lv_obj_t *textBox, *clkTime, *clkDate, *clkTimeSh, *clkDateSh, *chev, *hoursRow;
static bool expanded = false;
static uint32_t expandedAtMs = 0;
static constexpr int CARD_W0 = 300, CARD_H0 = 52, CARD_W1 = 452, CARD_H1 = 136;
static lv_obj_t *card, *nowIcon, *nowTemp, *nowCond, *nowRange, *hourCol[HOURS], *hourLbl[HOURS], *hourIco[HOURS], *hourTmp[HOURS];
static lv_timer_t *animTimer;
static float dropX[DROPS], dropY[DROPS], dropSpeed[DROPS], fogX[FOG];
// Rain in 3 depth layers (far: thin/slow/faint, near: long/fast/bright), drawn as
// slanted streaks that match their motion, plus small splashes on the ground.
static uint8_t dropLayer[DROPS];
static constexpr float RAIN_SLANT = 0.22f;                   // dx/dy of the fall
static const lv_point_precise_t kStreak[3][2] = {{{2, 0}, {0, 9}}, {{4, 0}, {0, 16}}, {{6, 0}, {0, 25}}};
static const lv_opa_t kStreakOpa[3] = {70, 120, 175};
static const uint8_t kStreakW[3] = {1, 1, 2};
static const float kStreakSpeed[3][2] = {{230, 290}, {320, 400}, {430, 520}};
// Intensity (rainLevel): how many streaks, how fast, how long, how bright. Garoa = few,
// short, slow and faint; chuva forte = all 30, full length, splashes on the ground.
static const lv_point_precise_t kStreakDrizzle[3][2] = {{{1, 0}, {0, 5}}, {{2, 0}, {0, 8}}, {{2, 0}, {0, 10}}};
static const int kRainDrops[4] = {0, 10, 20, 30};
static const float kRainSpeedK[4] = {0, 0.45f, 0.8f, 1.0f};
static const float kRainOpaK[4] = {0, 0.6f, 0.85f, 1.0f};
static int rainLvl = 0;
static lv_obj_t *splash[SPLASH];
static uint32_t splashStart[SPLASH];
static int splashX[SPLASH], splashY[SPLASH];
static uint8_t starPhase[STARS];
static bool minimal = false, night = false, dimmed = false;
static int wxCode = -1, renderedMinute = -1;
static uint32_t nextFlashMs = 0, flashStartMs = 0, nextShootMs = 0, shootStartMs = 0;
static float shootX0, shootY0;
static int iconCodes[HOURS + 1];

// ---------------------------------------------------------------- helpers
static lv_obj_t *shape(lv_obj_t *p, int w, int h, uint32_t color, lv_opa_t opa, int radius = LV_RADIUS_CIRCLE) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_clickable(o, false);
  lv_obj_set_scrollable(o, false);
  return o;
}
static lv_obj_t *group(lv_obj_t *p, int w, int h) {
  lv_obj_t *o = shape(p, w, h, 0, LV_OPA_TRANSP, 0);
  return o;
}
static void show(lv_obj_t *o, bool on) {
  if (on == !lv_obj_is_hidden(o)) return;
  if (on) lv_obj_set_hidden(o, false); else lv_obj_set_hidden(o, true);
}
static uint32_t mixHex(uint32_t a, uint32_t b, float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  auto ch = [&](int s) { return (uint32_t)lroundf(((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t) << s; };
  return ch(16) | ch(8) | ch(0);
}
static float frand(float a, float b) { return a + (b - a) * (float)(esp_random() % 10000) / 10000.0f; }

static bool isFog(int c) { return c == 45 || c == 48; }
static bool isStorm(int c) { return c >= 95; }
static bool isOvercast(int c) { return c >= 3; }
static bool isWet(int c) { return isRainCode(c) || isStorm(c); }

// ---------------------------------------------------------------- tiny weather icons (cards)
// Builds a small vector icon (about 34x26) for a WMO code inside `box` (cleared first).
static void drawMiniIcon(lv_obj_t *box, int code, bool isNight, float s = 1.0f) {
  lv_obj_clean(box);
  auto px = [&](float v) { return (int)lroundf(v * s); };
  const bool cloudy = code >= 1, sunny = code <= 2;   // 1 poucas nuvens / 2 parcialmente nublado: sun + cloud
  if (sunny) {
    if (isNight) {
      lv_obj_t *m = shape(box, px(16), px(16), 0xEEF1F8, LV_OPA_COVER); lv_obj_set_pos(m, px(code ? 4 : 9), px(1));
      lv_obj_t *c = shape(box, px(13), px(13), 0x1A2140, LV_OPA_COVER); lv_obj_set_pos(c, px(code ? 9 : 14), px(-1));
    } else {
      lv_obj_t *h = shape(box, px(22), px(22), 0xFFD43B, LV_OPA_30); lv_obj_set_pos(h, px(code ? 1 : 6), px(-2));
      lv_obj_t *sun = shape(box, px(16), px(16), 0xFFC21A, LV_OPA_COVER); lv_obj_set_pos(sun, px(code ? 4 : 9), px(1));
    }
  }
  if (cloudy) {
    const uint32_t col = isWet(code) || code == 3 ? 0xC9D1DD : 0xFFFFFF;
    const int oy = sunny ? 6 : 2;
    lv_obj_t *a = shape(box, px(14), px(14), col, LV_OPA_COVER); lv_obj_set_pos(a, px(6), px(oy + 6));
    lv_obj_t *b = shape(box, px(17), px(17), col, LV_OPA_COVER); lv_obj_set_pos(b, px(13), px(oy + 1));
    lv_obj_t *c = shape(box, px(28), px(10), col, LV_OPA_COVER, px(5)); lv_obj_set_pos(c, px(4), px(oy + 10));
  }
  switch (rainLevel(code)) {        // garoa: small dots / chuva: 3 drops / forte: 4 long drops
    case 1: for (int i = 0; i < 4; ++i) { lv_obj_t *d = shape(box, px(2), px(3), 0xA8D8FF, LV_OPA_90); lv_obj_set_pos(d, px(8 + i * 6), px(22 + (i & 1) * 3)); } break;
    case 2: for (int i = 0; i < 3; ++i) { lv_obj_t *d = shape(box, px(2), px(6), 0x6CB6FF, LV_OPA_COVER); lv_obj_set_pos(d, px(9 + i * 7), px(22)); } break;
    case 3: for (int i = 0; i < 4; ++i) { lv_obj_t *d = shape(box, px(2), px(8), 0x4A9CF5, LV_OPA_COVER); lv_obj_set_pos(d, px(7 + i * 6), px(21 + (i & 1) * 2)); } break;
    default: break;
  }
  if (isStorm(code)) { lv_obj_t *z = shape(box, px(4), px(8), 0xFFD43B, LV_OPA_COVER, 1); lv_obj_set_pos(z, px(17), px(21)); }
  if (isFog(code)) {
    for (int i = 0; i < 2; ++i) { lv_obj_t *f = shape(box, px(26), px(3), 0xE6EAF0, LV_OPA_80, 1); lv_obj_set_pos(f, px(5), px(20 + i * 5)); }
  }
}

// ---------------------------------------------------------------- sky photo
// One 480x320 RGB565 buffer in PSRAM; a JPEG from flash is decoded into it only
// when the scene (time-of-day x weather) changes.
static uint16_t *skyPixels;
static lv_image_dsc_t skyDsc;
static int skyLoaded = -1;

static int jpegDraw(JPEGDRAW *d) {
  for (int row = 0; row < d->iHeight; ++row) {
    const int y = d->y + row;
    if (y >= SCR_H) break;
    const int w = min(d->iWidth, SCR_W - d->x);
    if (w > 0) memcpy(skyPixels + y * SCR_W + d->x, d->pPixels + row * d->iWidth, w * 2);
  }
  return 1;
}

static void loadSky(int id) {
  if (id == skyLoaded || id < 0 || id >= SKY_COUNT) return;
  if (!skyPixels) skyPixels = (uint16_t *)psramMalloc(SCR_W * SCR_H * 2);
  void *mem = psramMalloc(sizeof(JPEGDEC));
  if (!skyPixels || !mem) { if (mem) heap_caps_free(mem); return; }
  JPEGDEC *jpeg = new (mem) JPEGDEC();
  if (jpeg->openFLASH((uint8_t *)kSkyImages[id].data, (int)kSkyImages[id].size, jpegDraw)) {
    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    jpeg->decode(0, 0, 0);
    jpeg->close();
    skyLoaded = id;
    if (prefs.getUChar("clkSky", 255) != id) prefs.putUChar("clkSky", (uint8_t)id);
  }
  jpeg->~JPEGDEC();
  heap_caps_free(mem);
  skyDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  skyDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  skyDsc.header.w = SCR_W; skyDsc.header.h = SCR_H; skyDsc.header.stride = SCR_W * 2;
  skyDsc.data_size = SCR_W * SCR_H * 2; skyDsc.data = (const uint8_t *)skyPixels;
  lv_image_set_src(sky, &skyDsc);
  lv_obj_invalidate(sky);
}

struct WxCacheShared { int code; float temp, tmax, tmin; int rain; time_t at; };   // == WxCache below
static constexpr time_t WX_CACHE_MAX_AGE_S = 2 * 3600;   // cache older than 2 h is ignored
// Sun altitude (deg) at the configured place, now. Low-precision solar formula
// (~0.5 deg), plenty for picking a sky. NAN while the clock is not set.
float sunAltitudeNow() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return NAN;
  const double RADF = M_PI / 180.0, d = (now - 946728000.0) / 86400.0;   // days since J2000.0
  const double g = 357.529 + 0.98560028 * d, q = 280.459 + 0.98564736 * d;
  const double L = (q + 1.915 * sin(g * RADF) + 0.020 * sin(2 * g * RADF)) * RADF;
  const double e = (23.439 - 0.00000036 * d) * RADF;
  const double ra = atan2(cos(e) * sin(L), cos(L)), dec = asin(sin(e) * sin(L));
  const double lst = fmod(280.46061837 + 360.98564736629 * d + cfg.lon, 360.0) * RADF;
  const double lat = cfg.lat * RADF;
  return (float)(asin(sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(lst - ra)) / RADF);
}
// Without a set clock: a rough altitude from the local hour (sun up 6-18 h).
static float sunAltFor(float h) {
  const float a = sunAltitudeNow();
  if (!isnan(a)) return a;
  return 60.0f * sinf((h - 6.0f) / 12.0f * (float)M_PI);
}
// Sky photo by sun altitude: day above +6 deg, golden hour +6..0, twilight 0..-6, night below.
static int pickSky(float alt, int code) {
  const bool nightA = alt < -6.0f;
  if (isStorm(code)) return SKY_STORM;
  if (isWet(code)) return nightA ? SKY_NIGHT_OVERCAST : rainLevel(code) == 1 ? SKY_OVERCAST_DAY : SKY_RAIN;
  if (isFog(code)) return nightA ? SKY_NIGHT_OVERCAST : SKY_FOG;
  if (code >= 3) return nightA ? SKY_NIGHT_OVERCAST : SKY_OVERCAST_DAY;
  if (nightA) return code >= 1 ? SKY_NIGHT_PARTLY : SKY_NIGHT_CLEAR;
  if (alt < 0.0f) return SKY_DUSK;
  if (alt < 6.0f) return SKY_DAWN;
  return code >= 1 ? SKY_DAY_PARTLY : SKY_DAY_CLEAR;
}
static int lastSkyId = -1;
static float lastSkyAlt = 90.0f;
int currentSkyId() { return lastSkyId; }

// Light veil over the day photos that have no twilight version (overcast, rain, fog):
// amber near sunset/sunrise, deepening to dark blue in civil twilight. Night photos,
// storm and the clear-sky dawn/dusk photos are left as they are.
void applySkyVeil(lv_obj_t *veil) {
  if (!veil) return;
  const bool dayGrey = lastSkyId == SKY_OVERCAST_DAY || lastSkyId == SKY_RAIN || lastSkyId == SKY_FOG;
  const float a = lastSkyAlt;
  if (!dayGrey || a >= 8.0f || a < -6.5f) { lv_obj_set_hidden(veil, true); return; }
  auto mix = [](uint32_t x, uint32_t y, float t) -> uint32_t {
    auto ch = [&](int sh) { return (uint32_t)(((x >> sh) & 0xFF) * (1 - t) + ((y >> sh) & 0xFF) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
  };
  uint32_t top, bot; float opa;
  if (a >= 0.0f) {                                   // +8..0: amber glow grows toward the horizon
    const float t = (8.0f - a) / 8.0f;
    top = 0x3C285A; bot = 0xFF8C3C; opa = 0.38f * t;
  } else {                                           // 0..-6.5: amber -> dark blue, heavier
    const float t = fminf(1.0f, -a / 6.5f);
    top = mix(0x3C285A, 0x0F193C, t); bot = mix(0xFF8C3C, 0x463C6E, t); opa = 0.38f + 0.27f * t;
  }
  lv_obj_set_style_bg_color(veil, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(veil, lv_color_hex(bot), 0);
  lv_obj_set_style_bg_opa(veil, (lv_opa_t)(opa * 255), 0);
  lv_obj_set_hidden(veil, false);
}
// Fade a freshly decoded sky in (from the dark card behind it) instead of a hard cut.
static void skyOpaCb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
void fadeInSky(lv_obj_t *img) {
  if (!img) return;
  lv_anim_delete(img, skyOpaCb);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, img); lv_anim_set_exec_cb(&a, skyOpaCb);
  lv_anim_set_values(&a, 0, LV_OPA_COVER); lv_anim_set_duration(&a, 1200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}
// Common setup for a full-screen veil object (created right after the sky image).
lv_obj_t *createSkyVeil(lv_obj_t *parent) {
  lv_obj_t *v = lv_obj_create(parent);
  lv_obj_remove_style_all(v);
  lv_obj_set_pos(v, 0, 0); lv_obj_set_size(v, SCR_W, SCR_H);
  lv_obj_set_style_bg_grad_dir(v, LV_GRAD_DIR_VER, 0);
  lv_obj_set_clickable(v, false);
  lv_obj_set_hidden(v, true);
  return v;
}

// Shared with the Weather screen: same PSRAM buffer, only one screen is visible.
// Weather code saved by the Clock (last good forecast), for the Weather screen's sky.
bool cachedWeatherCode(int &code) {
  WxCacheShared w;
  if (prefs.getBytes("clkWx", &w, sizeof w) != sizeof w) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000 || now - w.at > WX_CACHE_MAX_AGE_S || w.at > now + 60) return false;
  code = w.code; return true;
}
const lv_image_dsc_t *skyImageFor(float hour, int code) {
  lastSkyAlt = sunAltFor(hour);
  lastSkyId = pickSky(lastSkyAlt, code);
  loadSky(lastSkyId);
  return skyLoaded >= 0 ? &skyDsc : nullptr;
}

// ---------------------------------------------------------------- per-minute scene
static void updateScene(const struct tm &ti) {
  const float hour = ti.tm_hour + ti.tm_min / 60.0f;
  const float alt = sunAltFor(hour);
  night = alt < -6.0f;                                  // after civil twilight, by the real sun
  // Sky only when the right one is known (time + live or cached weather) and decoded;
  // otherwise plain black. Never an old picture first.
  bool skyOn = false;
  if (!minimal && wxCode >= 0) {
    const int id = pickSky(alt, wxCode); lastSkyId = id; lastSkyAlt = alt;
    const int before = skyLoaded, wasHidden = lv_obj_is_hidden(sky);
    loadSky(id); skyOn = skyLoaded == id;
    if (skyOn && (before != id || wasHidden)) fadeInSky(sky);
  }
  show(sky, skyOn);
  if (skyOn) applySkyVeil(skyVeil); else show(skyVeil, false);
  lv_obj_set_style_bg_color(card, lv_color_hex(minimal ? 0x111111 : night ? 0x0A0E1C : 0x0E1A2C), 0);
  const bool clearNight = skyOn && night && wxCode <= 2;
  for (int i = 0; i < STARS; ++i) show(star[i], clearNight);
  const int lvl = skyOn ? rainLevel(wxCode) : 0;
  if (lvl != rainLvl) {                                   // restyle the streaks for the new intensity
    rainLvl = lvl;
    for (int i = 0; i < DROPS; ++i) {
      const int L = dropLayer[i];
      lv_line_set_points(drop[i], lvl == 1 ? kStreakDrizzle[L] : kStreak[L], 2);
      lv_obj_set_style_line_width(drop[i], lvl == 1 ? 1 : kStreakW[L], 0);
      lv_obj_set_style_line_opa(drop[i], (lv_opa_t)(kStreakOpa[L] * kRainOpaK[lvl]), 0);
    }
  }
  for (int i = 0; i < DROPS; ++i) show(drop[i], i < kRainDrops[lvl]);
  if (lvl < 3) for (int k = 0; k < SPLASH; ++k) { splashStart[k] = 0; show(splash[k], false); }
  for (int i = 0; i < FOG; ++i) show(fog[i], skyOn && isFog(wxCode));
  // Drifting clouds over the clear / partly cloudy photos: few for "poucas nuvens",
  // more for "parcialmente nublado". Dimmer and bluer at night.
  cloudsShown = !skyOn ? 0 : wxCode == 1 ? 2 : wxCode == 2 ? CLOUDS : 0;
  for (int i = 0; i < CLOUDS; ++i) {
    lv_obj_set_style_bg_color(cloud[i], lv_color_hex(night ? 0x9AA6BC : 0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(cloud[i], night ? LV_OPA_20 : LV_OPA_30, 0);
    show(cloud[i], i < cloudsShown);
  }

  const bool wantDim = ti.tm_hour >= 23 || ti.tm_hour < 6;          // night dimming
  if (wantDim != dimmed) { dimmed = wantDim; analogWrite(GFX_BL, wantDim ? 40 : map(cfg.bright, 0, 100, 25, 255)); }
  const int s = (ti.tm_min / 5) % 4;                                 // burn-in shift
  lv_obj_set_pos(textBox, (s == 1) - (s == 3), (s == 2) * 2);
}

// ---------------------------------------------------------------- animation (20 fps)
static void animCb(lv_timer_t *) {
  static uint32_t last = 0;
  const uint32_t now = lv_tick_get();
  float dt = last ? (now - last) / 1000.0f : 0.05f;
  last = now;
  if (dt > 0.2f) dt = 0.05f;
  if (minimal) return;

  if (rainLvl && !lv_obj_is_hidden(drop[0])) {
    const int nDrops = kRainDrops[rainLvl];
    const float spd = kRainSpeedK[rainLvl];
    for (int i = 0; i < nDrops; ++i) {
      dropY[i] += dropSpeed[i] * spd * dt; dropX[i] -= dropSpeed[i] * spd * RAIN_SLANT * dt;
      const int ground = dropLayer[i] == 2 ? 300 : dropLayer[i] == 1 ? 270 : 245;   // nearer drops fall lower
      if (dropY[i] > ground) {
        if (dropLayer[i] == 2 && rainLvl == 3) {       // near drop landed: splash (heavy rain only)
          for (int k = 0; k < SPLASH; ++k) if (!splashStart[k]) {
            splashStart[k] = now; splashX[k] = (int)dropX[i]; splashY[k] = ground + 18; break;
          }
        }
        dropY[i] = frand(-60, -20);                    // respawn; keep most rain off the clock digits
        do dropX[i] = frand(0, SCR_W + 80); while (dropX[i] > 70 && dropX[i] < 470 && frand(0, 1) < 0.5f);
      }
      lv_obj_set_pos(drop[i], (int)dropX[i], (int)dropY[i]);
    }
    for (int k = 0; k < SPLASH; ++k) {                 // splash: small ellipse ring growing and fading
      if (!splashStart[k]) continue;
      const uint32_t e = now - splashStart[k];
      if (e > 260) { splashStart[k] = 0; show(splash[k], false); continue; }
      const int w = 4 + (int)(e * 14 / 260), h = 2 + (int)(e * 3 / 260);
      lv_obj_set_size(splash[k], w, h);
      lv_obj_set_pos(splash[k], splashX[k] - w / 2, splashY[k] - h / 2);
      lv_obj_set_style_border_opa(splash[k], (lv_opa_t)(150 - e * 150 / 260), 0);
      show(splash[k], true);
    }
  }
  for (int i = 0; i < cloudsShown; ++i) {                          // slow drift, wrap around
    cloudX[i] += (4 + i * 2) * dt;
    if (cloudX[i] > SCR_W) cloudX[i] = -(float)lv_obj_get_width(cloud[i]);
    lv_obj_set_x(cloud[i], (int)cloudX[i]);
  }
  if (!lv_obj_is_hidden(fog[0])) {
    for (int i = 0; i < FOG; ++i) {
      fogX[i] += (5 + i * 3) * dt * ((i & 1) ? -1 : 1);
      if (fogX[i] > 0) fogX[i] = -80;
      if (fogX[i] < -80) fogX[i] = 0;
      lv_obj_set_x(fog[i], (int)fogX[i]);
    }
  }
  if (!lv_obj_is_hidden(star[0])) {
    for (int k = 0; k < 4; ++k) {                                   // twinkle
      const int i = esp_random() % STARS;
      starPhase[i] = (starPhase[i] + 25) % 200;
      const int v = starPhase[i] < 100 ? starPhase[i] : 200 - starPhase[i];
      lv_obj_set_style_bg_opa(star[i], (lv_opa_t)(70 + v * 1.8f), 0);
    }
    if (!nextShootMs) nextShootMs = now + 8000 + esp_random() % 15000;   // shooting star
    if (!shootStartMs && (int32_t)(now - nextShootMs) >= 0) {
      shootStartMs = now; shootX0 = frand(140, SCR_W - 20); shootY0 = frand(10, 70); show(shoot, true);
    }
    if (shootStartMs) {
      const float e = (now - shootStartMs) / 700.0f;
      lv_obj_set_pos(shoot, (int)(shootX0 - e * 160), (int)(shootY0 + e * 60));
      lv_obj_set_style_opa(shoot, (lv_opa_t)(255 * (1 - e)), 0);
      if (e >= 1) { shootStartMs = 0; show(shoot, false); nextShootMs = now + 12000 + esp_random() % 20000; }
    }
  }
  if (isStorm(wxCode) && !minimal && !lv_obj_is_hidden(sky)) {                                // lightning: double flicker
    if (!nextFlashMs) nextFlashMs = now + 4000;
    if (!flashStartMs && (int32_t)(now - nextFlashMs) >= 0) { flashStartMs = now; show(flash, true); }
    if (flashStartMs) {
      const uint32_t e = now - flashStartMs;
      const int o = e < 60 ? 150 : e < 120 ? 40 : e < 170 ? 120 : e < 320 ? 120 - (int)(e - 170) * 120 / 150 : 0;
      lv_obj_set_style_bg_opa(flash, (lv_opa_t)max(0, o), 0);
      if (e >= 320) { flashStartMs = 0; show(flash, false); nextFlashMs = now + 5000 + esp_random() % 9000; }
    }
  }
}

// ---------------------------------------------------------------- build
static void tapToggle(lv_event_t *) {
  minimal = !minimal;
  prefs.putBool("clkMin", minimal);
  renderedMinute = -1;
}

static void cardAnimCb(void *, int32_t v) {          // v: 0 = collapsed ... 256 = expanded
  lv_obj_set_size(card, CARD_W0 + (CARD_W1 - CARD_W0) * v / 256, CARD_H0 + (CARD_H1 - CARD_H0) * v / 256);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -12);
}
static void setExpanded(bool on) {
  if (on == expanded) return;
  expanded = on; expandedAtMs = lv_tick_get();
  lv_label_set_text(chev, on ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
  if (!on) show(hoursRow, false);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_exec_cb(&a, cardAnimCb);
  lv_anim_set_values(&a, on ? 0 : 256, on ? 256 : 0);
  lv_anim_set_duration(&a, 220);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  if (on) lv_anim_set_completed_cb(&a, [](lv_anim_t *) { show(hoursRow, true); });
  lv_anim_start(&a);
}
static void toggleCard(lv_event_t *) { if (!lv_obj_is_hidden(chev)) setExpanded(!expanded); }

static void buildClockScreen(lv_obj_t *scr) {
  clockView = lv_obj_create(scr);
  lv_obj_set_size(clockView, SCR_W, SCR_H); lv_obj_set_pos(clockView, 0, 0);
  lv_obj_set_style_border_width(clockView, 0, 0); lv_obj_set_style_pad_all(clockView, 0, 0);
  lv_obj_set_style_radius(clockView, 0, 0);
  lv_obj_set_style_bg_color(clockView, lv_color_hex(0x000000), 0); lv_obj_set_style_bg_opa(clockView, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(clockView, false);
  lv_obj_set_hidden(clockView, true);
  lv_obj_add_event_cb(clockView, tapToggle, LV_EVENT_CLICKED, NULL);

  sky = lv_image_create(clockView);           // photo-like background (see sky_images.c)
  lv_obj_set_pos(sky, 0, 0); lv_obj_set_size(sky, SCR_W, SCR_H);
  lv_obj_set_clickable(sky, false);
  skyVeil = createSkyVeil(clockView);             // twilight tint over the grey day photos

  for (int i = 0; i < STARS; ++i) {               // a few twinkling stars over the baked star field
    star[i] = shape(clockView, 3, 3, 0xFFFFFF, LV_OPA_70);
    lv_obj_set_pos(star[i], (int)frand(6, SCR_W - 6), (int)frand(6, 180));
    starPhase[i] = esp_random() % 200;
  }
  shoot = shape(clockView, 46, 2, 0xFFFFFF, LV_OPA_COVER, 1);
  lv_obj_set_style_bg_grad_color(shoot, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_grad_dir(shoot, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_bg_main_opa(shoot, LV_OPA_COVER, 0); lv_obj_set_style_bg_grad_opa(shoot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_transform_rotation(shoot, -200, 0);      // -20°
  show(shoot, false);

  for (int i = 0; i < CLOUDS; ++i) {               // soft drifting clouds (wxCode 1-2)
    static const int16_t kW[CLOUDS] = {170, 120, 150, 100}, kY[CLOUDS] = {28, 70, 112, 50};
    cloud[i] = shape(clockView, kW[i], kW[i] / 4, 0xFFFFFF, LV_OPA_30);
    cloudX[i] = (float)(i * SCR_W / CLOUDS);
    lv_obj_set_pos(cloud[i], (int)cloudX[i], kY[i]);
    lv_obj_add_flag(cloud[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = 0; i < FOG; ++i) {
    fog[i] = shape(clockView, SCR_W + 160, 30, 0xE3E8EF, LV_OPA_20, 15);
    lv_obj_set_pos(fog[i], -60, 140 + i * 44); fogX[i] = -20 - i * 25;
  }
  for (int i = 0; i < DROPS; ++i) {
    const int L = i % 5 == 0 ? 2 : i % 2 ? 1 : 0;      // ~20 % near, ~40 % mid, ~40 % far
    dropLayer[i] = (uint8_t)L;
    drop[i] = lv_line_create(clockView);
    lv_line_set_points(drop[i], kStreak[L], 2);
    lv_obj_set_style_line_width(drop[i], kStreakW[L], 0);
    lv_obj_set_style_line_color(drop[i], lv_color_hex(0xDDEBFF), 0);
    lv_obj_set_style_line_opa(drop[i], kStreakOpa[L], 0);
    lv_obj_set_style_line_rounded(drop[i], true, 0);
    lv_obj_set_clickable(drop[i], false);
    dropX[i] = frand(0, SCR_W + 80); dropY[i] = frand(-SCR_H, SCR_H);
    dropSpeed[i] = frand(kStreakSpeed[L][0], kStreakSpeed[L][1]);
  }
  for (int k = 0; k < SPLASH; ++k) {
    splash[k] = shape(clockView, 4, 2, 0xDDEBFF, LV_OPA_TRANSP);
    lv_obj_set_style_border_width(splash[k], 1, 0);
    lv_obj_set_style_border_color(splash[k], lv_color_hex(0xDDEBFF), 0);
    show(splash[k], false); splashStart[k] = 0;
  }
  flash = shape(clockView, SCR_W, SCR_H, 0xFFFFFF, LV_OPA_TRANSP, 0);

  // ---- lock-screen text: date above, big time
  textBox = group(clockView, SCR_W, 170);
  clkDateSh = makeLabel(textBox, "", &lv_font_weather_20, 0x000000);       // drop shadows
  lv_obj_set_style_text_opa(clkDateSh, LV_OPA_50, 0);
  lv_obj_align(clkDateSh, LV_ALIGN_TOP_MID, 1, 24);
  clkTimeSh = makeLabel(textBox, "--:--", &lv_font_clock_120, 0x000000);
  lv_obj_set_style_text_opa(clkTimeSh, LV_OPA_40, 0);
  lv_obj_align(clkTimeSh, LV_ALIGN_TOP_MID, 2, 44);
  clkDate = makeLabel(textBox, "", &lv_font_weather_20, 0xFFFFFF);
  lv_obj_align(clkDate, LV_ALIGN_TOP_MID, 0, 22);
  clkTime = makeLabel(textBox, "--:--", &lv_font_clock_120, 0xFFFFFF);
  lv_obj_align(clkTime, LV_ALIGN_TOP_MID, 0, 40);

  // ---- glass pill: today only; tap to expand the next hours
  card = shape(clockView, CARD_W0, CARD_H0, 0x0E1A2C, LV_OPA_50, 22);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_20, 0);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_clickable(card, true);
  lv_obj_add_event_cb(card, toggleCard, LV_EVENT_CLICKED, NULL);

  nowIcon = group(card, 40, 30); lv_obj_set_pos(nowIcon, 14, 11);
  nowTemp = makeLabel(card, "--\xC2\xB0", &lv_font_montserrat_28, 0xFFFFFF); lv_obj_set_pos(nowTemp, 58, 10);
  nowCond = makeLabel(card, "", &lv_font_weather_14, 0xFFFFFF);
  lv_obj_set_size(nowCond, 170, 18); lv_label_set_long_mode(nowCond, LV_LABEL_LONG_DOT); lv_obj_set_pos(nowCond, 116, 8);
  nowRange = makeLabel(card, "", &lv_font_weather_14, 0xFFFFFF);
  lv_obj_set_style_text_opa(nowRange, LV_OPA_70, 0);
  lv_obj_set_size(nowRange, 150, 18); lv_label_set_long_mode(nowRange, LV_LABEL_LONG_DOT); lv_obj_set_pos(nowRange, 116, 26);
  chev = makeLabel(card, LV_SYMBOL_UP, &lv_font_weather_14, 0xFFFFFF);
  lv_obj_set_style_text_opa(chev, LV_OPA_60, 0);
  lv_obj_align(chev, LV_ALIGN_TOP_RIGHT, -14, 17);

  hoursRow = group(card, CARD_W1 - 20, 80); lv_obj_set_pos(hoursRow, 10, CARD_H0 - 2);
  lv_obj_t *sep = shape(hoursRow, CARD_W1 - 40, 1, 0xFFFFFF, LV_OPA_20, 0); lv_obj_set_pos(sep, 10, 0);
  for (int i = 0; i < HOURS; ++i) {
    hourCol[i] = group(hoursRow, 60, 78); lv_obj_set_pos(hourCol[i], 16 + i * 82, 4);
    hourLbl[i] = makeLabel(hourCol[i], "--", &lv_font_weather_14, 0xFFFFFF);
    lv_obj_set_style_text_opa(hourLbl[i], LV_OPA_70, 0); lv_obj_align(hourLbl[i], LV_ALIGN_TOP_MID, 0, 2);
    hourIco[i] = group(hourCol[i], 36, 30); lv_obj_align(hourIco[i], LV_ALIGN_TOP_MID, 0, 22);
    hourTmp[i] = makeLabel(hourCol[i], "--", &lv_font_weather_14, 0xFFFFFF); lv_obj_align(hourTmp[i], LV_ALIGN_TOP_MID, 0, 56);
  }
  show(hoursRow, false);
  show(card, false);
  for (int &c : iconCodes) c = -99;

  lv_obj_move_foreground(addBackButton(clockView));     // standard back button (main.cpp)

  animTimer = lv_timer_create(animCb, FRAME_MS, NULL);
  lv_timer_pause(animTimer);
}

// ---------------------------------------------------------------- lifecycle
// Last good weather + sky, kept in flash so the screen opens complete right after
// boot instead of showing empty placeholders while the first request runs.
struct WxCache { int code; float temp, tmax, tmin; int rain; time_t at; };   // at: when it was live
static bool cardShown = false;
static void showCard(bool on) {
  if (on == cardShown) return;
  cardShown = on;
  if (on) { show(card, true); lv_obj_fade_in(card, 350, 0); } else show(card, false);
}

static void clockOnEnter() {
  minimal = prefs.getBool("clkMin", false);
  // black until clockRender picks the right sky (time + live/cached weather)
  show(sky, false); show(skyVeil, false);
  for (int i = 0; i < STARS; ++i) show(star[i], false);
  for (int i = 0; i < DROPS; ++i) show(drop[i], false);
  for (int i = 0; i < FOG; ++i) show(fog[i], false);
  for (int i = 0; i < CLOUDS; ++i) show(cloud[i], false);
  cloudsShown = 0;
  renderedMinute = -1;
  nextFlashMs = flashStartMs = nextShootMs = shootStartMs = 0;
  for (int &c : iconCodes) c = -99;
  if (expanded) { lv_anim_delete(NULL, cardAnimCb); expanded = false; show(hoursRow, false); cardAnimCb(NULL, 0); lv_label_set_text(chev, LV_SYMBOL_UP); }
  struct tm ti;
  show(textBox, localNow(&ti));
  lv_timer_resume(animTimer);
}

static void clockOnExit() {
  lv_timer_pause(animTimer);
  if (dimmed) { dimmed = false; analogWrite(GFX_BL, map(cfg.bright, 0, 100, 25, 255)); }
}

static void clockRender() {
  struct tm ti;
  if (!localNow(&ti)) { show(textBox, false); showCard(false); renderedMinute = -1; return; }   // wait quietly for NTP
  if (lv_obj_is_hidden(textBox)) { show(textBox, true); lv_obj_fade_in(textBox, 350, 0); }
  char b[96];
  strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M", &ti);
  setLabelIfChanged(clkTime, b); setLabelIfChanged(clkTimeSh, b);
  if (expanded && lv_tick_elaps(expandedAtMs) > 15000) setExpanded(false);   // auto-collapse

  xSemaphoreTake(wxMtx, portMAX_DELAY);
  const bool have = wx.haveWeather;
  const int code = wx.code, rain = wx.dayRain[0], hcount = wx.hourCount;
  const float temp = wx.temp, tmax = wx.tmax[0], tmin = wx.tmin[0];
  char hl[HOURS][6]; float ht[HOURS]; int hc[HOURS]; bool hd[HOURS];
  for (int i = 0; i < HOURS; ++i) { memcpy(hl[i], wx.hourLabel[i], 6); ht[i] = wx.hourTemp[i]; hc[i] = wx.hourCode[i]; hd[i] = wx.hourIsDay[i]; }
  const bool liveDay = wx.isDay;
  const uint32_t wv = wx.weatherVersion;
  xSemaphoreGive(wxMtx);
  WxCache wc = {};
  bool cached = false;
  if (have) {                                  // remember for the next boot (only when it changed)
    WxCache now = {code, temp, tmax, tmin, rain, time(nullptr)}, old = {};
    // rewrite when the forecast changed, or every 10 min to keep the timestamp fresh
    const bool got = prefs.getBytes("clkWx", &old, sizeof old) == sizeof old;
    if (!got || memcmp(&old, &now, offsetof(WxCache, at)) || now.at - old.at > 600) prefs.putBytes("clkWx", &now, sizeof now);
    wc = now;
  } else {                                     // cache only if it is at most 2 h old
    const time_t t = time(nullptr);
    cached = prefs.getBytes("clkWx", &wc, sizeof wc) == sizeof wc &&
             t > 1700000000 && t - wc.at <= WX_CACHE_MAX_AGE_S && wc.at <= t + 60;
  }

  static uint32_t seenWv = UINT32_MAX;
  const int key = ti.tm_hour * 60 + ti.tm_min;
  if (key == renderedMinute && wv == seenWv) return;
  renderedMinute = key; seenWv = wv;
  wxCode = have || cached ? wc.code : -1;           // -1: no data at all -> black

  if (gLang == LANG_EN) snprintf(b, sizeof b, "%s, %s %d", weekdayLong(ti.tm_wday), monthLong(ti.tm_mon), ti.tm_mday);
  else snprintf(b, sizeof b, "%s, %d de %s", weekdayLong(ti.tm_wday), ti.tm_mday, monthLong(ti.tm_mon));
  setLabelIfChanged(clkDate, b); setLabelIfChanged(clkDateSh, b);
  updateScene(ti);

  if (!have && !cached) { showCard(false); return; }        // nothing yet: keep just sky + clock
  snprintf(b, sizeof b, "%.0f\xC2\xB0", wc.temp); setLabelIfChanged(nowTemp, b);
  const bool nowNight = have ? !liveDay : night;           // Open-Meteo is_day when live, clock hours otherwise
  setLabelIfChanged(nowCond, weatherText(wc.code, nowNight));   // "Céu limpo" at night
  snprintf(b, sizeof b, T("%.0f\xC2\xB0 / %.0f\xC2\xB0  \xC2\xB7  chuva %d%%", "%.0f\xC2\xB0 / %.0f\xC2\xB0  \xC2\xB7  rain %d%%"), wc.tmax, wc.tmin, wc.rain);
  setLabelIfChanged(nowRange, b);
  if (iconCodes[0] != wc.code * 2 + nowNight) { iconCodes[0] = wc.code * 2 + nowNight; drawMiniIcon(nowIcon, wc.code, nowNight, 1.15f); }
  showCard(true);
  show(chev, have && hcount > 0);
  if (!have) return;                                          // hours only with fresh data
  for (int i = 0; i < HOURS; ++i) {
    const bool ok = i < hcount;
    show(hourCol[i], ok);
    if (!ok) continue;
    setLabelIfChanged(hourLbl[i], i == 0 ? T("Agora", "Now") : hl[i]);
    snprintf(b, sizeof b, "%.0f\xC2\xB0", ht[i]); setLabelIfChanged(hourTmp[i], b);
    const bool hn = !hd[i];                                   // Open-Meteo is_day (sunrise/sunset of the place)
    if (iconCodes[i + 1] != hc[i] * 2 + hn) { iconCodes[i + 1] = hc[i] * 2 + hn; drawMiniIcon(hourIco[i], hc[i], hn); }
  }
}

static const NetJobDef kClockJobs[] = {{"weather", weatherFetch, true, true}};
const ScreenLifecycle kClockScreen = {
  SCREEN_CLOCK, "clock", &clockView, buildClockScreen, clockOnEnter, clockOnExit, clockRender,
  nullptr, WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS, kClockJobs, 1, nullptr,
};
