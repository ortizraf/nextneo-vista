// Photos screen: slideshow of up to 8 photos sent from the web page (/photos).
// The browser crops/resizes each photo to 480x320 JPEG before upload, so the board
// only stores it in LittleFS (/photos/N.jpg) and decodes it. Decoding is RGB8888 +
// 4x4 ordered dither down to RGB565, which hides the banding of smooth gradients.
// No network at all.
#include "app_screens.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <JPEGDEC.h>
#include <new>

lv_obj_t *photosView;
static lv_obj_t *img, *backBtn, *emptyLbl, *dots[PHOTO_SLOTS];
static uint16_t *pix;                     // 480x320 RGB565 in PSRAM
static lv_image_dsc_t dsc;
static int current = -1;                  // slot on screen
static lv_timer_t *slideTimer, *hideBackTimer;
static volatile bool changed = false;     // set by the web upload/delete
static lv_point_t pressAt;

static bool fsReady() {
  static int state = -1;
  if (state < 0) state = LittleFS.begin(true) ? 1 : 0;
  if (state == 1 && !LittleFS.exists("/photos")) LittleFS.mkdir("/photos");
  return state == 1;
}
void photoPath(int slot, char *out, size_t n) { snprintf(out, n, "/photos/%d.jpg", slot); }
bool photoExists(int slot) {
  if (slot < 0 || slot >= PHOTO_SLOTS || !fsReady()) return false;
  char p[24]; photoPath(slot, p, sizeof p);
  return LittleFS.exists(p);
}
void photosChanged() { changed = true; }
bool photosFsReady() { return fsReady(); }

// ---------------------------------------------------------------- decode
static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static int drawCb(JPEGDRAW *d) {
  const uint32_t *src = (const uint32_t *)d->pPixels;
  for (int row = 0; row < d->iHeight; ++row) {
    const int y = d->y + row;
    if (y >= SCR_H) break;
    const int w = min(d->iWidthUsed ? d->iWidthUsed : d->iWidth, SCR_W - d->x);
    uint16_t *dst = pix + y * SCR_W + d->x;
    const uint32_t *s = src + row * d->iWidth;
    for (int x = 0; x < w; ++x) {
      const uint32_t c = s[x];
      const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;   // JPEGDEC RGB8888 = 0xAABBGGRR
      const int t = kBayer[y & 3][(d->x + x) & 3];          // 0..15
      // add a sub-step offset before truncating: 5 bits -> step 8, 6 bits -> step 4
      const int r5 = min(31, (r + (t >> 1)) >> 3), g6 = min(63, (g + (t >> 2)) >> 2), b5 = min(31, (b + (t >> 1)) >> 3);
      dst[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
  }
  return 1;
}

static File jf;
static void *jOpen(const char *name, int32_t *size) {
  jf = LittleFS.open(name, "r");
  if (!jf) return nullptr;
  *size = jf.size();
  return &jf;
}
static void jClose(void *) { if (jf) jf.close(); }
static int32_t jRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  File *h = (File *)f->fHandle; const int32_t n = h->read(buf, len); f->iPos = h->position(); return n;
}
static int32_t jSeek(JPEGFILE *f, int32_t pos) {
  File *h = (File *)f->fHandle; h->seek(pos); f->iPos = h->position(); return f->iPos;
}

static bool showSlot(int slot) {
  char p[24]; photoPath(slot, p, sizeof p);
  if (!pix) pix = (uint16_t *)psramMalloc(SCR_W * SCR_H * 2);
  void *mem = psramMalloc(sizeof(JPEGDEC));
  if (!pix || !mem) { if (mem) heap_caps_free(mem); return false; }
  const uint32_t t0 = millis();
  JPEGDEC *jpeg = new (mem) JPEGDEC();
  bool ok = false;
  if (jpeg->open(p, jOpen, jClose, jRead, jSeek, drawCb)) {
    jpeg->setPixelType(RGB8888);
    memset(pix, 0, SCR_W * SCR_H * 2);
    // centre smaller images (the page always sends 480x320, this is just in case)
    const int ox = max(0, (SCR_W - jpeg->getWidth()) / 2), oy = max(0, (SCR_H - jpeg->getHeight()) / 2);
    ok = jpeg->decode(ox, oy, 0);
    jpeg->close();
  }
  jpeg->~JPEGDEC();
  heap_caps_free(mem);
  if (!ok) { LogV.printf("Fotos: falha ao abrir %s\n", p); return false; }
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC; dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = SCR_W; dsc.header.h = SCR_H; dsc.header.stride = SCR_W * 2;
  dsc.data_size = SCR_W * SCR_H * 2; dsc.data = (const uint8_t *)pix;
  lv_image_set_src(img, &dsc);
  lv_obj_invalidate(img);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
  current = slot;
  LogV.printf("Fotos: %s em %lums\n", p, (unsigned long)(millis() - t0));
  return true;
}

static void updateDots() {
  int n = 0;
  for (int i = 0; i < PHOTO_SLOTS; ++i) {
    const bool has = photoExists(i);
    if (!has) { lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(dots[i], i == current ? LV_OPA_90 : LV_OPA_40, 0);
    ++n;
  }
  (void)n;
}

// step = +1 next, -1 previous, 0 = current or first available
static void go(int step) {
  int any = 0;
  for (int i = 0; i < PHOTO_SLOTS; ++i) any += photoExists(i);
  if (!any) {
    current = -1;
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
    updateDots();
    return;
  }
  lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
  int s = current < 0 ? (step < 0 ? 0 : PHOTO_SLOTS - 1) : current;
  if (step == 0 && current >= 0 && photoExists(current)) { showSlot(current); updateDots(); return; }
  const int dir = step < 0 ? -1 : 1;
  for (int k = 0; k < PHOTO_SLOTS; ++k) {
    s = (s + dir + PHOTO_SLOTS) % PHOTO_SLOTS;
    if (photoExists(s)) { showSlot(s); break; }
  }
  updateDots();
  if (slideTimer) lv_timer_reset(slideTimer);              // full 30 s after a manual swipe
}

static void slideCb(lv_timer_t *) {
  if (changed) { changed = false; go(0); return; }
  go(+1);
}
static void watchCb(lv_timer_t *) {                         // web changes show up within 1 s
  if (changed) { changed = false; if (current >= 0 && !photoExists(current)) current = -1; go(current < 0 ? 1 : 0); }
}
static lv_timer_t *watchTimer;

static void showBack(bool on) {
  if (on) { lv_obj_remove_flag(backBtn, LV_OBJ_FLAG_HIDDEN); lv_timer_reset(hideBackTimer); lv_timer_resume(hideBackTimer); }
  else lv_obj_add_flag(backBtn, LV_OBJ_FLAG_HIDDEN);
}
static void hideBackCb(lv_timer_t *t) { showBack(false); lv_timer_pause(t); }

// swipe left/right = next/previous; tap = show the back button for 3 s
static void touchEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p; lv_indev_get_point(indev, &p);
  if (code == LV_EVENT_PRESSED) { pressAt = p; return; }
  if (code != LV_EVENT_RELEASED) return;
  const int dx = p.x - pressAt.x, dy = p.y - pressAt.y;
  if (abs(dx) > 50 && abs(dx) > abs(dy)) go(dx < 0 ? +1 : -1);
  else if (abs(dx) < 15 && abs(dy) < 15) showBack(lv_obj_has_flag(backBtn, LV_OBJ_FLAG_HIDDEN));
}

void buildPhotosScreen(lv_obj_t *scr) {
  photosView = lv_obj_create(scr);
  lv_obj_set_size(photosView, SCR_W, SCR_H); lv_obj_set_pos(photosView, 0, 0);
  lv_obj_set_style_radius(photosView, 0, 0);
  lv_obj_set_style_border_width(photosView, 0, 0); lv_obj_set_style_pad_all(photosView, 0, 0);
  lv_obj_set_style_bg_color(photosView, lv_color_hex(0x000000), 0); lv_obj_set_style_bg_opa(photosView, LV_OPA_COVER, 0);
  lv_obj_remove_flag(photosView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(photosView, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(photosView, touchEvent, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(photosView, touchEvent, LV_EVENT_RELEASED, NULL);
  lv_obj_add_flag(photosView, LV_OBJ_FLAG_HIDDEN);

  img = lv_image_create(photosView);
  lv_obj_set_pos(img, 0, 0); lv_obj_set_size(img, SCR_W, SCR_H);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

  emptyLbl = lv_label_create(photosView);
  lv_label_set_text(emptyLbl, "Nenhuma foto ainda\n\nEnvie até 8 fotos em\nhttp://<ip da placa>/photos");
  lv_obj_set_style_text_align(emptyLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(emptyLbl, lv_color_hex(0xB8C6D8), 0);
  lv_obj_set_style_text_font(emptyLbl, &lv_font_weather_14, 0);
  lv_obj_center(emptyLbl);
  lv_obj_remove_flag(emptyLbl, LV_OBJ_FLAG_CLICKABLE);

  for (int i = 0; i < PHOTO_SLOTS; ++i) {                  // position dots at the bottom
    dots[i] = lv_obj_create(photosView);
    lv_obj_set_size(dots[i], 7, 7);
    lv_obj_set_pos(dots[i], SCR_W / 2 - PHOTO_SLOTS * 7 + i * 14, SCR_H - 14);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dots[i], 0, 0);
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
  }

  backBtn = addBackButton(photosView);                    // standard back button (main.cpp)
  lv_obj_add_flag(backBtn, LV_OBJ_FLAG_HIDDEN);

  slideTimer = lv_timer_create(slideCb, PHOTO_SLIDE_MS, NULL);   lv_timer_pause(slideTimer);
  watchTimer = lv_timer_create(watchCb, 1000, NULL);             lv_timer_pause(watchTimer);
  hideBackTimer = lv_timer_create(hideBackCb, 3000, NULL);       lv_timer_pause(hideBackTimer);
}

void photosOnEnter() {
  // set the label with the real IP
  char b[96];
  snprintf(b, sizeof b, "Nenhuma foto ainda\n\nEnvie até %d fotos em\nhttp://%s/photos", PHOTO_SLOTS, WiFi.localIP().toString().c_str());
  lv_label_set_text(emptyLbl, b);
  changed = false;
  showBack(true);                                           // visible for 3 s on entry
  go(0);
  lv_timer_reset(slideTimer); lv_timer_resume(slideTimer);
  lv_timer_resume(watchTimer);
}

void photosOnExit() {
  lv_timer_pause(slideTimer); lv_timer_pause(watchTimer); lv_timer_pause(hideBackTimer);
}
