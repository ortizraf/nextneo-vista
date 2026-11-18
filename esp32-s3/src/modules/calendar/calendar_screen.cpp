// Calendar screen: cat over a Starry-Night style background (calendar_art.c; the cat is
// chosen on the web page, card "Calendário") with the month in a translucent card on the
// right. Swipe left/right changes the month; entering always shows the current one.
// National holidays (BrasilAPI, holidays.cpp) are painted gold once they arrive.
#include "app_screens.h"
#include "core/screen_registry.h"
#include "services/psram_alloc.h"
#include "modules/calendar/holidays.h"
#include "modules/calendar/calendar_cats.h"
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <new>
#include <time.h>
#include <stdlib.h>

#include "modules/calendar/calendar_art.h"

static lv_obj_t *calendarView;
static int calendarOffset = 0;
static lv_obj_t *calendarTitle;
static lv_obj_t *calendarGrid;
static lv_obj_t *calArt, *calCard;
static lv_point_t calTouchStart;
static uint32_t artCatVersion = 0;      // calendarCatVersion() of the decoded art

// ---------------------------------------------------------------- background art
static uint16_t *artPixels;             // 480x320 RGB565 in PSRAM: what is on screen
static uint16_t *bgPixels;              // decoded background (no cat), kept: opening is instant
static lv_image_dsc_t artDsc;

// JPEG -> RGB565 buffer (the screen, or a sprite buffer while compositing)
static uint16_t *drawDst; static int drawW, drawH;
static int artDraw(JPEGDRAW *d) {
  for (int row = 0; row < d->iHeight; ++row) {
    const int y = d->y + row;
    if (y >= drawH) break;
    const int w = min(d->iWidth, drawW - d->x);
    if (w > 0) memcpy(drawDst + y * drawW + d->x, d->pPixels + row * d->iWidth, w * 2);
  }
  return 1;
}
static bool decodeJpeg(JPEGDEC *jpeg, const uint8_t *data, size_t size, uint16_t *dst, int w, int h) {
  drawDst = dst; drawW = w; drawH = h;
  if (!data || !jpeg->openFLASH((uint8_t *)data, (int)size, artDraw)) return false;
  jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
  const bool ok = jpeg->decode(0, 0, 0) == 1;
  jpeg->close();
  return ok;
}
static inline uint16_t mix565(uint16_t bg, uint16_t fg, int a /*0..16*/) {
  const int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (16 - a)) >> 4;
  const int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (16 - a)) >> 4;
  const int b = ((fg & 31) * a + (bg & 31) * (16 - a)) >> 4;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
// Cat sprite over the background: colours from its JPEG, 4-bit alpha from RLE pairs.
static void drawSprite(JPEGDEC *jpeg, const CalendarSprite &sp) {
  uint16_t *buf = (uint16_t *)psramMalloc((size_t)sp.w * sp.h * 2);
  if (!buf) return;
  if (decodeJpeg(jpeg, sp.jpg, sp.jpgSize, buf, sp.w, sp.h)) {
    int px = 0;
    const int total = sp.w * sp.h;
    for (size_t i = 0; i + 1 < sp.alphaSize && px < total; i += 2) {
      const int run = sp.alpha[i], v = sp.alpha[i + 1];
      const int a = v + (v >= 8);                    // 0..15 -> 0..16
      for (int k = 0; k < run && px < total; ++k, ++px) {
        if (!a) continue;
        const int x = sp.x + px % sp.w, y = sp.y + px / sp.w;
        if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) continue;
        uint16_t &d = artPixels[y * SCR_W + x];
        d = a >= 16 ? buf[px] : mix565(d, buf[px], a);
      }
    }
  }
  heap_caps_free(buf);
}
static int shownCat = -1;                // cat composited in artPixels
// Decode the background once (kept in PSRAM), then for each opening only restore the
// cat area and draw the chosen cat: a few ms instead of a full JPEG decode.
static bool loadArt() {
  const size_t bytes = SCR_W * SCR_H * 2;
  if (!bgPixels || !artPixels) {
    if (!bgPixels) bgPixels = (uint16_t *)psramMalloc(bytes);
    if (!artPixels) artPixels = (uint16_t *)psramMalloc(bytes);
    if (!bgPixels || !artPixels) return false;
    void *mem = psramMalloc(sizeof(JPEGDEC));
    if (!mem) return false;
    JPEGDEC *jpeg = new (mem) JPEGDEC();
    const bool ok = decodeJpeg(jpeg, kCalendarBg, kCalendarBgSize, bgPixels, SCR_W, SCR_H);
    jpeg->~JPEGDEC(); heap_caps_free(mem);
    if (!ok) { heap_caps_free(bgPixels); bgPixels = nullptr; heap_caps_free(artPixels); artPixels = nullptr; return false; }
    shownCat = -1;
    artDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    artDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    artDsc.header.w = SCR_W; artDsc.header.h = SCR_H; artDsc.header.stride = SCR_W * 2;
    artDsc.data_size = bytes; artDsc.data = (const uint8_t *)artPixels;
  }
  static uint8_t lastCat = CAL_CAT_ART_COUNT;          // none yet
  const uint8_t cat = calendarCatResolve(calendarCat(), lastCat, esp_random());
  lastCat = cat;
  artCatVersion = calendarCatVersion();
  if ((int)cat == shownCat) return true;               // same cat already composited
  memcpy(artPixels, bgPixels, bytes);
  void *mem = psramMalloc(sizeof(JPEGDEC));
  if (mem) {
    JPEGDEC *jpeg = new (mem) JPEGDEC();
    if (cat < CALENDAR_SPRITES) drawSprite(jpeg, kCalendarSprites[cat]);
    jpeg->~JPEGDEC(); heap_caps_free(mem);
  }
  shownCat = cat;
  if (calArt) lv_obj_invalidate(calArt);
  return true;
}

#include <time.h>

static int drawnYear = -1, drawnYday = -1;
// Day numbers of the month on screen, so holidays can be recoloured without a redraw.
static lv_obj_t *dayNum[32];
static bool daySunday[32], daySelected[32];
static int shownYear = 0, shownMonth = 0, shownDays = 0;
static uint32_t shownHolVersion = 0;
static constexpr uint32_t COL_SUNDAY = 0xF04B54, COL_HOLIDAY = 0xFFC94D, COL_DAY = 0xFFFFFF;

// Paint national holidays (gold). Only label colours change: no layout, no flicker.
static void applyHolidayColors() {
  shownHolVersion = holidaysVersion();
  for (int d = 1; d <= shownDays; ++d) {
    if (!dayNum[d] || daySelected[d]) continue;
    const uint32_t c = holidayIs(shownYear, shownMonth, d) ? COL_HOLIDAY : daySunday[d] ? COL_SUNDAY : COL_DAY;
    lv_obj_set_style_text_color(dayNum[d], lv_color_hex(c), 0);
  }
}
static const char *calMonth(int m) {
  static const char *const pt[] = {"JAN.","FEV.","MAR.","ABR.","MAI.","JUN.","JUL.","AGO.","SET.","OUT.","NOV.","DEZ."};
  static const char *const en[] = {"JAN.","FEB.","MAR.","APR.","MAY","JUN.","JUL.","AUG.","SEP.","OCT.","NOV.","DEC."};
  return gLang == LANG_EN ? en[m] : pt[m];
}

// Layout of the current look: art = compact grid inside the card, black = classic.
static void applyLayout() {
  const bool art = artPixels != nullptr;
  if (art) { lv_image_set_src(calArt, &artDsc); }
  lv_obj_set_hidden(calArt, !art);
  lv_obj_set_hidden(calCard, !art);
  if (art) {
    lv_obj_set_size(calendarGrid, 7 * 38, 250);
    lv_obj_align_to(calendarGrid, calCard, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_align_to(calendarTitle, calCard, LV_ALIGN_TOP_MID, 0, 8);
  } else {
    lv_obj_set_size(calendarGrid, 430, 265);
    lv_obj_align(calendarGrid, LV_ALIGN_CENTER, 0, 40);
    lv_obj_align(calendarTitle, LV_ALIGN_TOP_MID, 0, 30);
  }
}

static void drawCalendar() {
  lv_obj_clean(calendarGrid);
  const bool art = artPixels != nullptr;
  const int pitch = art ? 38 : 60, rowH = art ? 36 : 35, top = art ? 30 : 34;
  const int cellW = art ? 32 : 44, cellH = art ? 30 : 36, lblW = art ? 38 : 52;
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today); struct tm base=today;
  drawnYear = today.tm_year; drawnYday = today.tm_yday;
  base.tm_mday = 1; base.tm_mon += calendarOffset; mktime(&base);
  char title[20]; snprintf(title,sizeof(title),"%s %d",calMonth(base.tm_mon),base.tm_year+1900); lv_label_set_text(calendarTitle,title);
  const char *const wdPt[] = {"D","S","T","Q","Q","S","S"}, *const wdEn[] = {"S","M","T","W","T","F","S"};
  const char *const *wd = gLang == LANG_EN ? wdEn : wdPt;
  for(int i=0;i<7;i++){ auto l=makeLabel(calendarGrid,wd[i],&lv_font_weather_20,i==0?0xF04B54:0xFFFFFF); lv_obj_set_size(l,lblW,32); lv_obj_set_pos(l,i*pitch,0); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); }
  memset(dayNum,0,sizeof dayNum);
  shownYear=base.tm_year+1900; shownMonth=base.tm_mon+1;
  holidaysWant(shownYear);
  int first=base.tm_wday; struct tm next=base; next.tm_mon++; next.tm_mday=0; mktime(&next); int days=next.tm_mday;
  for(int d=1;d<=days;d++){int cell=first+d-1,row=cell/7,col=cell%7; bool selected=(d==today.tm_mday && calendarOffset==0); lv_obj_t *cellBox=lv_obj_create(calendarGrid); lv_obj_set_size(cellBox,cellW,cellH); lv_obj_set_pos(cellBox,col*pitch+(lblW-cellW)/2,row*rowH+top); lv_obj_set_style_bg_opa(cellBox,selected?LV_OPA_COVER:LV_OPA_TRANSP,0); lv_obj_set_style_bg_color(cellBox,lv_color_hex(0xF04B54),0); lv_obj_set_style_border_width(cellBox,0,0); lv_obj_set_style_radius(cellBox,7,0); lv_obj_set_style_pad_all(cellBox,0,0); lv_obj_set_scrollable(cellBox, false); lv_obj_set_clickable(cellBox, false); char n[4];snprintf(n,sizeof(n),"%d",d); lv_obj_t *num=lv_label_create(cellBox); lv_label_set_text(num,n); lv_obj_set_style_text_font(num,&lv_font_weather_20,0); lv_obj_set_style_text_color(num,lv_color_hex((col==0&&!selected)?0xF04B54:0xFFFFFF),0); lv_obj_center(num); dayNum[d]=num; daySunday[d]=col==0; daySelected[d]=selected;}
  shownDays=days;
  applyHolidayColors();
  if (holidaysNeedFetch()) postScreenJob(0);   // background fetch; colours arrive later
}

// Swipe left/right: change month.
static void calendarGesture(lv_event_t *e){
  lv_event_code_t c=lv_event_get_code(e); lv_indev_t *in=lv_indev_active(); if(!in)return;
  lv_point_t p; lv_indev_get_point(in,&p);
  if(c==LV_EVENT_PRESSED){calTouchStart=p;return;}
  if(c==LV_EVENT_RELEASED){
    const int dx=p.x-calTouchStart.x;
    if(abs(dx)>45){calendarOffset+=dx<0?1:-1;drawCalendar();}
  }
}

// On-Enter: always the current month; decode the art only while the screen is shown.
static void calendarOnEnter() {
  calendarOffset = 0;
  loadArt();
  applyLayout();
  drawCalendar();
}
static void calendarOnExit() {}   // art stays decoded: next opening is instant

// Periodic refresh while the calendar stays open: redraw only when the day
// changed (midnight rollover or NTP sync after boot).
static void calendarRequestUpdate() {
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today);
  if (today.tm_year != drawnYear || today.tm_yday != drawnYday) drawCalendar();
  if (holidaysNeedFetch()) postScreenJob(0);
}
// 500 ms UI tick: holidays just arrived -> recolour the numbers only.
static void calendarRender() {
  if (artPixels && calendarCatVersion() != artCatVersion) {   // cat changed on the web page
    loadArt(); applyLayout();
  }
  if (holidaysVersion() != shownHolVersion) applyHolidayColors();
}
static const NetJobDef kCalendarJobs[] = { {"holidays", holidaysFetch, true} };

static void buildCalendarScreen(lv_obj_t *scr) {
  calendarView = lv_obj_create(scr); lv_obj_set_size(calendarView,SCR_W,SCR_H); lv_obj_set_pos(calendarView,0,0); lv_obj_set_style_bg_color(calendarView,lv_color_hex(0x05091C),0); lv_obj_set_style_bg_opa(calendarView,LV_OPA_COVER,0); lv_obj_set_style_border_width(calendarView,0,0); lv_obj_set_style_pad_all(calendarView,0,0); lv_obj_set_scrollable(calendarView, false); lv_obj_set_hidden(calendarView, true); lv_obj_set_gesture_bubble(calendarView, true);
  calArt = lv_image_create(calendarView); lv_obj_set_pos(calArt,0,0); lv_obj_set_size(calArt,SCR_W,SCR_H); lv_obj_set_clickable(calArt,false); lv_obj_set_hidden(calArt,true);
  calCard = lv_obj_create(calendarView); lv_obj_set_pos(calCard,196,14); lv_obj_set_size(calCard,274,292);
  lv_obj_set_style_radius(calCard,12,0); lv_obj_set_style_bg_color(calCard,lv_color_hex(0x05091C),0); lv_obj_set_style_bg_opa(calCard,(lv_opa_t)190,0);
  lv_obj_set_style_border_width(calCard,0,0); lv_obj_set_style_pad_all(calCard,0,0); lv_obj_set_scrollable(calCard,false); lv_obj_set_clickable(calCard,false); lv_obj_set_hidden(calCard,true);
  calendarTitle=makeLabel(calendarView,"",&lv_font_weather_20,0xFFFFFF); lv_obj_align(calendarTitle,LV_ALIGN_TOP_MID,0,30);
  calendarGrid=lv_obj_create(calendarView); lv_obj_set_size(calendarGrid,430,265); lv_obj_align(calendarGrid,LV_ALIGN_CENTER,0,40); lv_obj_set_style_bg_opa(calendarGrid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(calendarGrid,0,0); lv_obj_set_style_pad_all(calendarGrid,0,0); lv_obj_set_scrollable(calendarGrid, false); lv_obj_set_clickable(calendarGrid, false);
  // the whole screen takes the touches (tap anywhere toggles, swipe anywhere changes month)
  lv_obj_add_event_cb(calendarView,calendarGesture,LV_EVENT_PRESSED,NULL); lv_obj_add_event_cb(calendarView,calendarGesture,LV_EVENT_RELEASED,NULL);
  addBackButton(calendarView); drawCalendar();
}

const ScreenLifecycle kCalendarScreen = {
  SCREEN_CALENDARIO, "calendario", &calendarView, buildCalendarScreen, calendarOnEnter, calendarOnExit, calendarRender,
  calendarRequestUpdate, CALENDARIO_UPDATE_INTERVAL_MS, 60UL * 1000UL, kCalendarJobs, 1, nullptr,
};
