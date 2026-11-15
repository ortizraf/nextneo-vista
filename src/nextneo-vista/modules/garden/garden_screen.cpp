// Garden ("Jardim") screen: one plant in a pot, in front of a garden, under the same
// sky photo as Weather/Clock (time of day x live weather). The plant grows over real
// days (garden_model.cpp); a tap on the drop waters it; heat dries it, rain waters it.
//
// Depth: sky photo -> pre-rendered blurred tree line, hedge, bokeh -> lawn -> plant -> out-of-focus
// leaves in the front corners. The plant and the pot are pre-rendered 3D-looking
// pictures (tools/garden_art.py -> garden_art.cpp), decoded only when the plant changes.
// Life: wind gusts, a butterfly visiting the flowers, a celebration on each new stage.
// The state lives in Preferences ("garden") and survives power-off (see garden_model.h).
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <lvgl.h>
#include "app_screens.h"
#include "core/screen_registry.h"
#include "modules/garden/garden.h"
#include "modules/garden/garden_art.h"
#include <JPEGDEC.h>
#include <new>
#include "services/psram_alloc.h"
#include "modules/weather/weather.h"
#include "services/app_log.h"

using namespace garden;

const lv_image_dsc_t *skyImageFor(float hour, int code);   // clock_screen.cpp (shared PSRAM buffer)

static constexpr int ART_W = 220;                              // plant frame width (garden_art.py)
static constexpr int PLANT_X = (SCR_W - ART_W) / 2;            // frame left edge on screen
static constexpr int POT_Y = SCR_H - 92 - 2;                   // pot picture top (150 x 92)
static constexpr int SOIL_Y = POT_Y + 15;                      // stem foot = soil centre
static constexpr int SPARKS = 8;
static constexpr uint32_t TICK_MS = 60UL * 1000UL;            // model step while the screen is open
static constexpr uint32_t SAVE_MS = 10UL * 60UL * 1000UL;     // flash write at most every 10 min
static constexpr int RAIN_DROPS = 26, WATER_DROPS = 9, BACK_Y = 82;

static lv_obj_t *gardenView, *skyImg, *nightShade, *potImg, *plantImg, *flyImg, *toast, *toastLbl;
static lv_obj_t *spark[SPARKS];
static const GardenArt *plantArt;
static lv_draw_buf_t *backBuf, *potBuf, *flyBuf[2];
static const void *flyDsc[2];
static float gustDeg = 1.5f;
static int sparkX, sparkY;
static uint32_t toastHideMs = 0;
static lv_timer_t *gustTimer, *flyTimer;
static lv_obj_t *infoCard, *lblName, *lblSub;
static lv_obj_t *waterBar, *healthBar, *lblWater, *lblHealth, *waterBtn, *waterIcon;
static lv_obj_t *picker, *pickerWarn, *pickerCheck[SPECIES_COUNT];
static lv_obj_t *rain[RAIN_DROPS], *drops[WATER_DROPS];
static lv_timer_t *rainTimer;
static lv_draw_buf_t *plantSnap;
static bool badgeDirty = true;

static State st;
static bool stLoaded = false;
static uint32_t bootEpoch = 0, lastTickMs = 0, lastSaveMs = 0;
static Weather weather;
static int wxCode = -1, skyKey = -1;
static uint32_t seenWxVersion = UINT32_MAX;
static int plantKey = -1;
static bool raining = false;

// ---------------------------------------------------------------- persistence
static void loadState() {
  if (stLoaded) return;
  State s;
  if (prefs.getBytes("garden", &s, sizeof s) == sizeof s && s.version == 1) st = s;
  stLoaded = true;
}
garden::State &gardenState() { return st; }

static void saveState() {
  prefs.putBytes("garden", &st, sizeof st);
  lastSaveMs = millis();
  badgeDirty = true;
}

static uint32_t epochNow() {
  const time_t t = time(nullptr);
  if (t < 1700000000) return 0;                                  // NTP not synced yet
  if (!bootEpoch) bootEpoch = (uint32_t)t - millis() / 1000;     // when the board was switched on
  return (uint32_t)t;
}

// Moves the plant to now. Time between the last save and this boot was "board off".
static void tick(bool forceSave) {
  const uint32_t now = epochNow();
  if (!now || !hasPlant(st)) return;
  const uint32_t offline = st.lastTick && st.lastTick < bootEpoch ? bootEpoch - st.lastTick : 0;
  advance(st, now, offline, weather);
  lastTickMs = millis();
  if (forceSave || offline || millis() - lastSaveMs > SAVE_MS) saveState();
}

// ---------------------------------------------------------------- shape helpers
static lv_obj_t *shape(lv_obj_t *p, int x, int y, int w, int h, uint32_t c1, uint32_t c2 = 0,
                       lv_grad_dir_t dir = LV_GRAD_DIR_NONE, lv_opa_t opa = LV_OPA_COVER, int radius = LV_RADIUS_CIRCLE) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(c1), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  if (dir != LV_GRAD_DIR_NONE) {
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(c2), 0);
    lv_obj_set_style_bg_grad_dir(o, dir, 0);
  }
  lv_obj_set_clickable(o, false); lv_obj_set_scrollable(o, false);
  return o;
}
static uint32_t mix(uint32_t a, uint32_t b, uint8_t amountB) {   // amountB/255 of b
  return lv_color_to_u32(lv_color_mix(lv_color_hex(b), lv_color_hex(a), amountB)) & 0xFFFFFF;
}

// ---------------------------------------------------------------- pre-rendered pictures
// garden_art.cpp holds each picture as JPEG colour + RLE 4-bit alpha. They are decoded
// into an ARGB8888 buffer in PSRAM only when the plant changes (stage, wilt, death), so
// redraws just blit the picture. Wilting and fading are applied while decoding (rows
// shifted sideways = drooping stem, colours tinted), never per frame.
static uint16_t *jpegTmp;                 // RGB565 scratch for the JPEG decoder
static int jpegTmpW, jpegTmpH;
static int artJpegDraw(JPEGDRAW *d) {          // blocks may run past the picture edge: clip
  const int w = min(d->iWidth, jpegTmpW - d->x);
  for (int row = 0; row < d->iHeight && d->y + row < jpegTmpH; ++row)
    if (w > 0) memcpy(jpegTmp + (d->y + row) * jpegTmpW + d->x, d->pPixels + row * d->iWidth, w * 2);
  return 1;
}

enum ArtLook : uint8_t { LOOK_FRESH, LOOK_WILT, LOOK_FADED, LOOK_DEAD };

// Decodes one picture. bendPx: the top row moves this far right (quadratic droop).
static lv_draw_buf_t *decodeArt(const GardenArt &art, ArtLook look) {
  const int w = art.w, h = art.h;
  const int bendPx = look == LOOK_WILT ? 24 : look == LOOK_DEAD ? 34 : 0;
  const int outW = w + bendPx;
  lv_draw_buf_t *buf = lv_draw_buf_create(outW, h, LV_COLOR_FORMAT_ARGB8888, 0);
  jpegTmp = (uint16_t *)psramMalloc((size_t)w * h * 2);
  uint8_t *alpha = (uint8_t *)psramMalloc((size_t)w * h);
  void *mem = psramMalloc(sizeof(JPEGDEC));
  bool ok = buf && jpegTmp && alpha && mem;
  if (ok) {
    jpegTmpW = w; jpegTmpH = h;
    memset(jpegTmp, 0, (size_t)w * h * 2);
    JPEGDEC *jpeg = new (mem) JPEGDEC();
    ok = jpeg->openFLASH((uint8_t *)art.jpg, (int)art.jpgSize, artJpegDraw);
    if (ok) { jpeg->setPixelType(RGB565_LITTLE_ENDIAN); ok = jpeg->decode(0, 0, 0); jpeg->close(); }
    jpeg->~JPEGDEC();
  }
  if (ok) {                                                     // alpha: RLE of 4-bit values
    size_t o = 0, n = (size_t)w * h;
    for (size_t i = 0; i < art.alphaSize && o < n; ++i) {
      const uint8_t b = art.alpha[i], v = (b >> 4) * 17;
      size_t run = b & 15;
      if (!run && i + 1 < art.alphaSize) run = art.alpha[++i] + 16;
      while (run-- && o < n) alpha[o++] = v;
    }
    lv_draw_buf_clear(buf, nullptr);
    const int tint = look == LOOK_WILT ? 95 : look == LOOK_FADED ? 120 : look == LOOK_DEAD ? 190 : 0;
    for (int y = 0; y < h; ++y) {
      const float up = 1.0f - (float)y / (h - 1);
      const int shift = (int)lroundf(bendPx * up * up);
      uint32_t *dst = (uint32_t *)(buf->data + y * buf->header.stride) + shift;
      const uint16_t *src = jpegTmp + y * w;
      const uint8_t *a = alpha + y * w;
      for (int x = 0; x < w; ++x) {
        const uint16_t c = src[x];
        int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
        if (tint) {                                             // towards dry straw / brown
          const int l = (r * 3 + g * 6 + b) / 10;
          const int tr = l * 160 / 255 + 70, tg = l * 130 / 255 + 45, tb = l * 70 / 255 + 20;
          r += (tr - r) * tint / 255; g += (tg - g) * tint / 255; b += (tb - b) * tint / 255;
        }
        dst[x] = (uint32_t)a[x] << 24 | r << 16 | g << 8 | b;
      }
    }
  } else if (buf) { lv_draw_buf_destroy(buf); buf = nullptr; }
  if (mem) heap_caps_free(mem);
  if (alpha) heap_caps_free(alpha);
  if (jpegTmp) heap_caps_free(jpegTmp);
  jpegTmp = nullptr;
  return buf;
}

static const GardenArt *artFor(const State &s, Stage stg) {
  if (!hasPlant(s) || stg == STAGE_SEED) return &kGardenSeed;
  if (stg == STAGE_SPROUT) return &kGardenSprout;
  const int i = (stg >= STAGE_BLOOM ? STAGE_BLOOM : stg) - STAGE_YOUNG;
  return &kGardenPlant[s.species][i];
}

static void placePlant() {          // foot of the picture on the soil of the pot
  if (!plantSnap || !plantArt) return;
  lv_obj_set_pos(plantImg, PLANT_X + plantArt->ox, SOIL_Y - plantArt->h + 1);
  lv_image_set_pivot(plantImg, ART_W / 2 - plantArt->ox, plantArt->h - 1);
}

// Re-decodes the picture only when something visible changed.
static void renderPlant() {
  const Stage stg = stage(st);
  const bool dead = hasPlant(st) && !st.alive, wilt = !dead && wilting(st, weather);
  const ArtLook look = dead ? LOOK_DEAD : wilt ? LOOK_WILT : stg == STAGE_SEEDS ? LOOK_FADED : LOOK_FRESH;
  const GardenArt *art = hasPlant(st) ? artFor(st, stg) : nullptr;
  const int key = art ? (int)(art - &kGardenPlant[0][0]) * 4 + look + (art == &kGardenSeed ? 1000 : art == &kGardenSprout ? 2000 : 0) : -2;
  if (key == plantKey) return;
  plantKey = key;
  if (!art) {
    lv_obj_set_hidden(plantImg, true);
    if (plantSnap) { lv_draw_buf_destroy(plantSnap); plantSnap = nullptr; }
    plantArt = nullptr;
    return;
  }
  const uint32_t t0 = millis();
  lv_draw_buf_t *nb = decodeArt(*art, look);
  if (!nb) { LogV.println("Jardim: sem mem\xC3\xB3ria para a imagem da planta"); return; }
  lv_image_set_src(plantImg, nb);
  if (plantSnap) lv_draw_buf_destroy(plantSnap);
  plantSnap = nb;
  plantArt = art;
  placePlant();
  lv_obj_set_hidden(plantImg, false);
  LogV.printf("Jardim: planta decodificada em %lums\n", (unsigned long)(millis() - t0));
}
const lv_draw_buf_t *gardenPlantPicture() { return plantSnap; }

// ---------------------------------------------------------------- wind gusts
// A gust every 7-14 s tilts the plant around its foot for ~2.4 s. The picture is only
// transformed while a gust runs; the rest of the time it is a plain blit.
static void swayCb(void *, int32_t v) {
  const float t = v / 1000.0f;                                  // 0..1
  const float ang = sinf(t * 6.2832f * 2) * (1 - t) * gustDeg;
  lv_image_set_rotation(plantImg, (int32_t)lroundf(ang * 10));
}
static void swayDone(lv_anim_t *) { lv_image_set_rotation(plantImg, 0); }
static void gustCb(lv_timer_t *t) {
  lv_timer_set_period(t, 7000 + esp_random() % 7000);
  if (!plantSnap || !hasPlant(st) || !st.alive || stage(st) == STAGE_SEED || !lv_obj_is_hidden(picker)) return;
  gustDeg = 1.2f + (esp_random() % 100) / 100.0f * (raining ? 2.0f : 1.0f);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, plantImg);
  lv_anim_set_custom_exec_cb(&a, nullptr);
  lv_anim_set_exec_cb(&a, swayCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_duration(&a, 2400);
  lv_anim_set_completed_cb(&a, swayDone);
  lv_anim_start(&a);
}

// ---------------------------------------------------------------- butterfly
// Visits a plant in bloom by day when it is dry: flies in, rests on the flowers
// flapping slowly, flies away. One small sprite, two frames.
enum FlyPhase : uint8_t { FLY_IDLE, FLY_IN, FLY_REST, FLY_OUT };
static FlyPhase flyPhase = FLY_IDLE;
static uint32_t flyT0 = 0, flyNextMs = 0;
static int flyFromX, flyFromY, flyToX, flyToY;
static bool flyFrame = false;

static bool butterflyWeather() {
  struct tm ti;
  const bool day = !localNow(&ti) || (ti.tm_hour >= 7 && ti.tm_hour < 18);
  return day && !raining && hasPlant(st) && st.alive && stage(st) == STAGE_BLOOM && plantArt;
}
static void flyStart() {
  flyPhase = FLY_IN; flyT0 = millis();
  const bool left = esp_random() & 1;
  flyFromX = left ? -40 : SCR_W + 10; flyFromY = 60 + esp_random() % 80;
  flyToX = PLANT_X + plantArt->ox + plantArt->w / 2 - 17 + (int)(esp_random() % 30) - 15;
  flyToY = SOIL_Y - plantArt->h + 2;
  lv_obj_set_pos(flyImg, flyFromX, flyFromY);
  lv_obj_set_hidden(flyImg, false);
}
// Test hook: a visit now when the plant is in bloom, whatever the hour or the rain.
void gardenButterflyGo() { if (hasPlant(st) && st.alive && stage(st) == STAGE_BLOOM && plantArt) flyStart(); }
lv_obj_t *gardenButterfly() { return flyImg; }

static void flyCb(lv_timer_t *) {
  const uint32_t now = millis();
  if (flyPhase == FLY_IDLE) {
    if (!flyNextMs) flyNextMs = now + 15000 + esp_random() % 20000;
    if ((int32_t)(now - flyNextMs) >= 0) { flyNextMs = 0; if (butterflyWeather()) flyStart(); }
    return;
  }
  if (!butterflyWeather() && flyPhase != FLY_OUT) { flyPhase = FLY_OUT; flyT0 = now; flyFromX = lv_obj_get_x(flyImg); flyFromY = lv_obj_get_y(flyImg); flyToX = SCR_W + 20; flyToY = 20; }
  const uint32_t el = now - flyT0;
  if (flyPhase == FLY_REST) {
    if (el % 1600 < 120) { flyFrame = !flyFrame; lv_image_set_src(flyImg, flyDsc[flyFrame]); }   // slow flap
    if (el > 7000) {
      flyPhase = FLY_OUT; flyT0 = now;
      flyFromX = flyToX; flyFromY = flyToY;
      flyToX = (esp_random() & 1) ? -40 : SCR_W + 10; flyToY = 20 + esp_random() % 60;
    }
    return;
  }
  const float t = el / 3800.0f;
  if (t >= 1) {
    if (flyPhase == FLY_IN) { flyPhase = FLY_REST; flyT0 = now; lv_obj_set_pos(flyImg, flyToX, flyToY); }
    else { flyPhase = FLY_IDLE; lv_obj_set_hidden(flyImg, true); }
    return;
  }
  const float e = t * t * (3 - 2 * t);
  const int x = flyFromX + (int)((flyToX - flyFromX) * e);
  const int y = flyFromY + (int)((flyToY - flyFromY) * e + sinf(t * 18) * 10 * (1 - e));   // fluttering path
  lv_obj_set_pos(flyImg, x, y);
  flyFrame = !flyFrame;
  lv_image_set_src(flyImg, flyDsc[flyFrame]);
}

// ---------------------------------------------------------------- new stage celebration
static void scaleCb(void *, int32_t v) { lv_image_set_scale(plantImg, v); }
static void sparkCb(void *o, int32_t v) {
  const int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)o);
  const float a = i * 0.785f + 0.3f, r = v * 0.07f;
  lv_obj_set_pos((lv_obj_t *)o, sparkX + (int)(cosf(a) * r) - 3, sparkY + (int)(sinf(a) * r * 0.8f) - 3);
  lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)(255 - v * 255 / 1000), 0);
}
static void sparkDone(lv_anim_t *a) { lv_obj_set_hidden((lv_obj_t *)a->var, true); }
static void celebrate() {
  const bool en = gLang == LANG_EN;
  char b[64];
  snprintf(b, sizeof b, T("Nova fase: %s!", "New stage: %s!"), stageName(stage(st), en));
  lv_label_set_text(toastLbl, b);
  lv_obj_set_hidden(toast, false);
  lv_obj_move_foreground(toast);
  toastHideMs = millis() + 3500;
  if (plantArt) {
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, plantImg);
    lv_anim_set_exec_cb(&a, scaleCb);
    lv_anim_set_values(&a, 200, 256);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
    sparkX = PLANT_X + plantArt->ox + plantArt->w / 2;
    sparkY = SOIL_Y - plantArt->h + 20;
  }
  for (int i = 0; i < SPARKS; ++i) {
    lv_obj_set_hidden(spark[i], false);
    lv_obj_move_foreground(spark[i]);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, spark[i]);
    lv_anim_set_exec_cb(&a, sparkCb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 1300);
    lv_anim_set_delay(&a, i * 40);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, sparkDone);
    lv_anim_start(&a);
  }
  markStageSeen(st);
  saveState();
}
const lv_draw_buf_t *gardenBackdrop() { return backBuf; }
lv_obj_t *gardenToast() { return toast; }

// ---------------------------------------------------------------- texts and bars
static void renderInfo() {
  const bool en = gLang == LANG_EN;
  char b[96];
  if (!hasPlant(st)) {
    setLabelIfChanged(lblName, T("Jardim", "Garden"));
    setLabelIfChanged(lblSub, T("Toque aqui para plantar", "Tap here to plant"));
  } else {
    const Species &sp = kSpecies[st.species];
    setLabelIfChanged(lblName, en ? sp.en : sp.pt);
    const Stage stg = stage(st);
    const Mood m = mood(st, weather);
    if (m == MOOD_DEAD) snprintf(b, sizeof b, T("Dia %d \xC2\xB7 secou \xC2\xB7 toque para replantar", "Day %d \xC2\xB7 dried up \xC2\xB7 tap to replant"), dayNumber(st));
    else if (stg == STAGE_SEEDS) snprintf(b, sizeof b, T("Dia %d \xC2\xB7 sementes prontas", "Day %d \xC2\xB7 seeds ready"), dayNumber(st));
    else if (m != MOOD_OK) snprintf(b, sizeof b, T("Dia %d \xC2\xB7 %s \xC2\xB7 %s", "Day %d \xC2\xB7 %s \xC2\xB7 %s"), dayNumber(st), stageName(stg, en), moodText(m, en));
    else if (stg < STAGE_BLOOM) {
      const int left = (int)ceilf(daysToBloom(st));
      snprintf(b, sizeof b, T("Dia %d \xC2\xB7 %s \xC2\xB7 flores em %d d", "Day %d \xC2\xB7 %s \xC2\xB7 flowers in %d d"), dayNumber(st), stageName(stg, en), left);
    } else snprintf(b, sizeof b, T("Dia %d \xC2\xB7 %s", "Day %d \xC2\xB7 %s"), dayNumber(st), stageName(stg, en));
    setLabelIfChanged(lblSub, b);
  }
  const int w = hasPlant(st) ? (int)lroundf(st.water) : 0, h = hasPlant(st) ? (int)lroundf(st.health) : 0;
  lv_bar_set_value(waterBar, w, LV_ANIM_ON);
  lv_bar_set_value(healthBar, h, LV_ANIM_ON);
  snprintf(b, sizeof b, T("\xC3\x81gua %d%%", "Water %d%%"), w); setLabelIfChanged(lblWater, b);
  snprintf(b, sizeof b, T("Sa\xC3\xBA" "de %d%%", "Health %d%%"), h); setLabelIfChanged(lblHealth, b);
  lv_obj_set_style_bg_color(waterBar, lv_color_hex(hasPlant(st) && st.water > kSpecies[st.species].wetLimit ? 0x8A6A3A : 0x4FA8F0), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(healthBar, lv_color_hex(h < 40 ? 0xF0A030 : 0x5ACB6A), LV_PART_INDICATOR);
  const bool replant = !hasPlant(st) || !st.alive || stage(st) == STAGE_SEEDS;
  lv_label_set_text(waterIcon, replant ? LV_SYMBOL_PLUS : LV_SYMBOL_TINT);
}

// ---------------------------------------------------------------- sky and rain
static void renderSky() {
  struct tm ti;
  const bool haveTime = localNow(&ti);
  int code = wxCode;
  if (code < 0 && !cachedWeatherCode(code)) code = -1;
  const float hour = haveTime ? ti.tm_hour + ti.tm_min / 60.0f : 12;
  const int key = haveTime && code >= 0 ? (int)(hour * 4) * 100 + code : -1;
  if (key != skyKey) {
    skyKey = key;
    const lv_image_dsc_t *img = key >= 0 ? skyImageFor(hour, code) : nullptr;
    if (img) { lv_image_set_src(skyImg, img); lv_obj_set_hidden(skyImg, false); }
    else lv_obj_set_hidden(skyImg, true);                      // view gradient as fallback
    const bool night = haveTime && (hour < 5.6f || hour >= 19.4f);
    lv_obj_set_style_bg_opa(nightShade, night ? LV_OPA_40 : LV_OPA_TRANSP, 0);
  }
  const bool wet = code >= 0 && isRainCode(code);
  if (wet != raining) {
    raining = wet;
    for (lv_obj_t *d : rain) lv_obj_set_hidden(d, !wet);
    if (wet) lv_timer_resume(rainTimer); else lv_timer_pause(rainTimer);
  }
}

static void rainCb(lv_timer_t *) {
  for (int i = 0; i < RAIN_DROPS; ++i) {
    int y = lv_obj_get_y(rain[i]) + 14 + (i % 3) * 4;
    int x = lv_obj_get_x(rain[i]) - 2;
    if (y > SCR_H) { y = -20 - (i * 7) % 40; x = (i * 53 + (int)(millis() / 7)) % (SCR_W + 40); }
    lv_obj_set_pos(rain[i], x, y);
  }
}

// ---------------------------------------------------------------- watering
static void dropAnim(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }
static void dropDone(lv_anim_t *a) { lv_obj_set_hidden((lv_obj_t *)a->var, true); }
static void playWaterDrops() {
  for (int i = 0; i < WATER_DROPS; ++i) {
    lv_obj_t *d = drops[i];
    lv_obj_set_pos(d, SCR_W / 2 - 44 + i * 11, SOIL_Y - 150 + (i % 3) * 12);
    lv_obj_set_hidden(d, false);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_exec_cb(&a, dropAnim);
    lv_anim_set_values(&a, lv_obj_get_y(d), SOIL_Y - 4);
    lv_anim_set_duration(&a, 520 + (i % 4) * 60);
    lv_anim_set_delay(&a, (i * 37) % 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, dropDone);
    lv_anim_start(&a);
  }
}

static void openPicker();
static void waterEvent(lv_event_t *) {
  if (!hasPlant(st) || !st.alive || stage(st) == STAGE_SEEDS) { openPicker(); return; }
  tick(false);
  water(st);
  saveState();
  playWaterDrops();
  renderInfo();
  renderPlant();
}

// ---------------------------------------------------------------- seed picker
static void closePicker() { lv_obj_set_hidden(picker, true); }
static void openPicker() {
  for (int i = 0; i < SPECIES_COUNT; ++i) lv_obj_set_hidden(pickerCheck[i], !(st.album & (1u << i)));
  const bool growing = hasPlant(st) && st.alive && stage(st) != STAGE_SEEDS;
  lv_obj_set_hidden(pickerWarn, !growing);
  lv_obj_set_hidden(picker, false);
  lv_obj_move_foreground(picker);
}
static void seedEvent(lv_event_t *e) {
  const uint8_t sp = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  const uint32_t now = epochNow();
  if (!now) { setLabelIfChanged(lblSub, T("Aguardando o rel\xC3\xB3gio da internet", "Waiting for the internet clock")); closePicker(); return; }
  plant(st, sp, now);
  markStageSeen(st);
  saveState();
  Log.printf("Jardim: plantou %s\n", kSpecies[sp].pt);
  closePicker();
  plantKey = -1;
  renderInfo();
  renderPlant();
}

static void buildPicker(lv_obj_t *parent) {
  picker = lv_obj_create(parent);
  lv_obj_remove_style_all(picker);
  lv_obj_set_size(picker, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(picker, lv_color_hex(0x0A1220), 0);
  lv_obj_set_style_bg_opa(picker, LV_OPA_90, 0);
  lv_obj_set_scrollable(picker, false);
  lv_obj_set_clickable(picker, true);                          // swallows taps behind it
  lv_obj_t *title = makeLabel(picker, T("Escolha uma semente", "Pick a seed"), &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(title, 16, 12);
  pickerWarn = makeLabel(picker, T("A planta atual ser\xC3\xA1 substitu\xC3\xAD" "da", "The current plant will be replaced"),
                         &lv_font_weather_14, 0xF6C45A);
  lv_obj_set_pos(pickerWarn, 16, 40);
  lv_obj_t *x = lv_button_create(picker);
  lv_obj_set_size(x, 40, 34); lv_obj_set_pos(x, SCR_W - 48, 8);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x243247), 0);
  lv_obj_set_style_radius(x, 12, 0); lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_add_event_cb(x, [](lv_event_t *) { closePicker(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *xl = makeLabel(x, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, 0xFFFFFF); lv_obj_center(xl);

  for (int i = 0; i < SPECIES_COUNT; ++i) {
    const Species &sp = kSpecies[i];
    lv_obj_t *c = lv_button_create(picker);
    lv_obj_set_size(c, 108, 112);
    lv_obj_set_pos(c, 12 + (i % 4) * 116, 66 + (i / 4) * 122);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1B283A), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x2A3C55), LV_STATE_PRESSED);
    lv_obj_set_style_radius(c, 16, 0); lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_add_event_cb(c, seedEvent, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *ball = shape(c, 32, 12, 44, 44, mix(sp.petal, 0xFFFFFF, 50), sp.petalDark, LV_GRAD_DIR_VER);
    shape(ball, 15, 15, 14, 14, sp.center);
    lv_obj_t *n = makeLabel(c, gLang == LANG_EN ? sp.en : sp.pt, &lv_font_weather_14, 0xEAF2FF);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 64);
    char d[24]; snprintf(d, sizeof d, T("%d dias", "%d days"), sp.bloomDays);
    lv_obj_t *dl = makeLabel(c, d, &lv_font_weather_14, 0x8FA3BC);
    lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, 84);
    pickerCheck[i] = makeLabel(c, LV_SYMBOL_OK, &lv_font_montserrat_14, 0x5ACB6A);   // already bloomed (album)
    lv_obj_set_pos(pickerCheck[i], 84, 8);
  }
  lv_obj_set_hidden(picker, true);
}

// ---------------------------------------------------------------- build
static lv_obj_t *glassCard(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y); lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x0A1424), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_50, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_border_color(c, lv_color_white(), 0);
  lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_set_scrollable(c, false);
  return c;
}
static lv_obj_t *makeBar(lv_obj_t *p, int x, int y) {
  lv_obj_t *b = lv_bar_create(p);
  lv_obj_set_size(b, 120, 6); lv_obj_set_pos(b, x, y);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 3, LV_PART_MAIN); lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
  return b;
}

static void buildGardenScreen(lv_obj_t *scr) {
  gardenView = lv_obj_create(scr);
  lv_obj_remove_style_all(gardenView);
  lv_obj_set_size(gardenView, SCR_W, SCR_H); lv_obj_set_pos(gardenView, 0, 0);
  lv_obj_set_style_bg_color(gardenView, lv_color_hex(0x5E9AD0), 0);       // fallback sky (no photo yet)
  lv_obj_set_style_bg_grad_color(gardenView, lv_color_hex(0xBFDDF0), 0);
  lv_obj_set_style_bg_grad_dir(gardenView, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(gardenView, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(gardenView, false);
  lv_obj_set_hidden(gardenView, true);

  skyImg = lv_image_create(gardenView);
  lv_obj_set_pos(skyImg, 0, 0); lv_obj_set_clickable(skyImg, false);
  lv_obj_set_hidden(skyImg, true);

  // far: pre-rendered out-of-focus tree line, hedge and bokeh (decoded once)
  backBuf = decodeArt(kGardenBackdrop, LOOK_FRESH);
  lv_obj_t *back = lv_image_create(gardenView);
  lv_obj_set_pos(back, 0, BACK_Y); lv_obj_set_clickable(back, false);
  if (backBuf) lv_image_set_src(back, backBuf);
  // lawn, lit at the back, darker in front
  shape(gardenView, 0, BACK_Y + kGardenBackdrop.h - 2, SCR_W, SCR_H - BACK_Y - kGardenBackdrop.h + 2, 0x5A9A3E, 0x1E4A1C, LV_GRAD_DIR_VER, LV_OPA_COVER, 0);

  potBuf = decodeArt(kGardenPot, LOOK_FRESH);                   // pot: decoded once
  potImg = lv_image_create(gardenView);
  lv_obj_set_pos(potImg, (SCR_W - kGardenPot.w) / 2, POT_Y);
  lv_obj_set_clickable(potImg, false);
  if (potBuf) lv_image_set_src(potImg, potBuf);
  plantImg = lv_image_create(gardenView);
  lv_obj_set_clickable(plantImg, false);
  lv_image_set_antialias(plantImg, true);
  lv_obj_set_hidden(plantImg, true);

  // near: blurred leaves in the corners (depth of field)
  shape(gardenView, -60, 262, 150, 80, 0x1A3A18, 0x0A1A0A, LV_GRAD_DIR_VER, LV_OPA_70);
  shape(gardenView, 380, 276, 160, 70, 0x1A3A18, 0x0A1A0A, LV_GRAD_DIR_VER, LV_OPA_60);

  nightShade = shape(gardenView, 0, 0, SCR_W, SCR_H, 0x020814, 0, LV_GRAD_DIR_NONE, LV_OPA_TRANSP, 0);

  for (int i = 0; i < RAIN_DROPS; ++i) {
    rain[i] = shape(gardenView, (i * 53) % SCR_W, (i * 37) % SCR_H, 2, 12 + (i % 3) * 4, 0xD6E6F6, 0, LV_GRAD_DIR_NONE, LV_OPA_50, 1);
    lv_obj_set_hidden(rain[i], true);
  }
  for (int i = 0; i < WATER_DROPS; ++i) {
    drops[i] = shape(gardenView, 0, 0, 7, 11, 0xBFE2FF, 0x3A8AD8, LV_GRAD_DIR_VER);
    lv_obj_set_hidden(drops[i], true);
  }

  addBackButton(gardenView);
  infoCard = glassCard(gardenView, 50, 4, 300, 50);
  lv_obj_set_clickable(infoCard, true);
  lv_obj_add_event_cb(infoCard, [](lv_event_t *) { openPicker(); }, LV_EVENT_CLICKED, NULL);
  lblName = makeLabel(infoCard, T("Jardim", "Garden"), &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(lblName, 12, 4);
  lblSub = makeLabel(infoCard, "", &lv_font_weather_14, 0xCFDCEA);
  lv_obj_set_pos(lblSub, 12, 28);
  lv_obj_set_width(lblSub, 280);
  lv_label_set_long_mode(lblSub, LV_LABEL_LONG_DOT);

  lv_obj_t *stats = glassCard(gardenView, 8, SCR_H - 70, 140, 62);
  lblWater = makeLabel(stats, "", &lv_font_weather_14, 0xEAF2FF); lv_obj_set_pos(lblWater, 10, 4);
  waterBar = makeBar(stats, 10, 23);
  lblHealth = makeLabel(stats, "", &lv_font_weather_14, 0xEAF2FF); lv_obj_set_pos(lblHealth, 10, 32);
  healthBar = makeBar(stats, 10, 51);

  waterBtn = lv_button_create(gardenView);
  lv_obj_set_size(waterBtn, 62, 62); lv_obj_set_pos(waterBtn, SCR_W - 72, SCR_H - 72);
  lv_obj_set_style_radius(waterBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(waterBtn, lv_color_hex(0x2A9BF0), 0);
  lv_obj_set_style_bg_grad_color(waterBtn, lv_color_hex(0x1462B0), 0);
  lv_obj_set_style_bg_grad_dir(waterBtn, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_color(waterBtn, lv_color_white(), 0);
  lv_obj_set_style_border_opa(waterBtn, LV_OPA_30, 0);
  lv_obj_set_style_border_width(waterBtn, 1, 0);
  lv_obj_set_style_shadow_width(waterBtn, 12, 0);
  lv_obj_set_style_shadow_opa(waterBtn, LV_OPA_40, 0);
  lv_obj_set_style_shadow_offset_y(waterBtn, 4, 0);
  lv_obj_add_event_cb(waterBtn, waterEvent, LV_EVENT_CLICKED, NULL);
  waterIcon = makeLabel(waterBtn, LV_SYMBOL_TINT, &lv_font_montserrat_28, 0xFFFFFF);
  lv_obj_center(waterIcon);

  // butterfly (two frames, decoded once) and the stage-up celebration
  for (int i = 0; i < 2; ++i) { flyBuf[i] = decodeArt(kGardenFly[i], LOOK_FRESH); flyDsc[i] = flyBuf[i]; }
  flyImg = lv_image_create(gardenView);
  lv_obj_set_clickable(flyImg, false);
  if (flyBuf[0]) lv_image_set_src(flyImg, flyBuf[0]);
  lv_obj_set_hidden(flyImg, true);
  for (int i = 0; i < SPARKS; ++i) {
    spark[i] = shape(gardenView, 0, 0, 7, 7, i & 1 ? 0xFFF3B0 : 0xFFD24A);
    lv_obj_set_style_shadow_color(spark[i], lv_color_hex(0xFFE27A), 0);
    lv_obj_set_style_shadow_width(spark[i], 8, 0);
    lv_obj_set_user_data(spark[i], (void *)(intptr_t)i);
    lv_obj_set_hidden(spark[i], true);
  }
  toast = glassCard(gardenView, 0, 62, 10, 34);
  lv_obj_set_style_bg_color(toast, lv_color_hex(0x1E5A2A), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_80, 0);
  lv_obj_set_style_radius(toast, 17, 0);
  lv_obj_set_size(toast, LV_SIZE_CONTENT, 34);
  lv_obj_set_style_pad_hor(toast, 16, 0);
  lv_obj_align(toast, LV_ALIGN_TOP_LEFT, 8, 62);         // left: clear of the flowers
  toastLbl = makeLabel(toast, "", &lv_font_weather_14, 0xFFFFFF);
  lv_obj_align(toastLbl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_hidden(toast, true);

  buildPicker(gardenView);
  gustTimer = lv_timer_create(gustCb, 9000, NULL);
  lv_timer_pause(gustTimer);
  flyTimer = lv_timer_create(flyCb, 120, NULL);
  lv_timer_pause(flyTimer);
  rainTimer = lv_timer_create(rainCb, 40, NULL);
  lv_timer_pause(rainTimer);
}

// ---------------------------------------------------------------- lifecycle
static void readWeather() {
  xSemaphoreTake(wxMtx, portMAX_DELAY);
  const bool have = wx.haveWeather;
  const float temp = wx.temp;
  const int code = wx.code;
  const uint32_t v = wx.weatherVersion;
  xSemaphoreGive(wxMtx);
  if (v == seenWxVersion) return;
  seenWxVersion = v;
  weather.known = have;
  weather.tempC = temp;
  weather.raining = have && isRainCode(code);
  wxCode = have ? code : -1;
}

static void gardenOnEnter() {
  loadState();
  readWeather();
  tick(false);
  plantKey = -1;
  skyKey = -2;
  renderSky();
  renderInfo();
  renderPlant();
  closePicker();
  if (!hasPlant(st)) openPicker();
  flyPhase = FLY_IDLE; flyNextMs = millis() + 6000;                 // first visit soon after opening
  lv_timer_resume(gustTimer); lv_timer_resume(flyTimer);
  if (newStageReached(st)) celebrate();
}

void gardenReload() {
  stLoaded = false;
  st = State();
  loadState();
  plantKey = -1;
  badgeDirty = true;
  if (gardenView && !lv_obj_is_hidden(gardenView)) { renderInfo(); renderPlant(); }
}

static void gardenOnExit() {
  lv_timer_pause(gustTimer); lv_timer_pause(flyTimer);
  lv_anim_delete(plantImg, nullptr);
  lv_image_set_rotation(plantImg, 0); lv_image_set_scale(plantImg, 256);
  flyPhase = FLY_IDLE; lv_obj_set_hidden(flyImg, true);
  lv_obj_set_hidden(toast, true);
  lv_timer_pause(rainTimer);
  raining = false;
  for (lv_obj_t *d : rain) lv_obj_set_hidden(d, true);
  if (hasPlant(st) && epochNow()) { tick(true); }
}

static void gardenRender() {
  const uint32_t v0 = seenWxVersion;
  readWeather();
  const bool wxChanged = v0 != seenWxVersion;
  if (wxChanged || millis() - lastTickMs > TICK_MS) {
    tick(false);
    renderInfo();
    renderPlant();
    if (newStageReached(st)) celebrate();
  }
  renderSky();
  if (toastHideMs && (int32_t)(millis() - toastHideMs) >= 0) { toastHideMs = 0; lv_obj_set_hidden(toast, true); }
}

// Menu badge. The saved plant moved to now on a copy (nothing written); recomputed at
// most every 10 minutes, or right after the plant was saved (watered, planted).
bool gardenNeedsWater() {
  static uint32_t atMs = 0;
  static bool value = false;
  if (!badgeDirty && atMs && millis() - atMs < SAVE_MS) return value;
  loadState();
  readWeather();
  State s = st;
  const uint32_t now = epochNow();
  if (now && hasPlant(s)) {
    const uint32_t offline = s.lastTick && s.lastTick < bootEpoch ? bootEpoch - s.lastTick : 0;
    advance(s, now, offline, weather);
  }
  value = needsWater(s);
  atMs = millis(); badgeDirty = false;
  return value;
}

static const NetJobDef kGardenJobs[] = {{"weather", weatherFetch, true, true}};
const ScreenLifecycle kGardenScreen = {
  SCREEN_GARDEN, "garden", &gardenView, buildGardenScreen, gardenOnEnter, gardenOnExit, gardenRender,
  nullptr, WEATHER_UPDATE_INTERVAL_MS, WEATHER_RETRY_INTERVAL_MS, kGardenJobs, 1, nullptr,
};
