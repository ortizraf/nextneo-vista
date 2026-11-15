#include "app_screens.h"
#include "core/screen_registry.h"

static lv_obj_t *calendarView;
static int calendarOffset = 0;
static lv_obj_t *calendarTitle;
static lv_obj_t *calendarGrid;
static lv_point_t calTouchStart;

#include <time.h>
#include <stdlib.h>

static int drawnYear = -1, drawnYday = -1;
static const char *calMonth(int m) {
  static const char *const pt[] = {"JAN.","FEV.","MAR.","ABR.","MAI.","JUN.","JUL.","AGO.","SET.","OUT.","NOV.","DEZ."};
  static const char *const en[] = {"JAN.","FEB.","MAR.","APR.","MAY","JUN.","JUL.","AUG.","SEP.","OCT.","NOV.","DEC."};
  return gLang == LANG_EN ? en[m] : pt[m];
}
static void drawCalendar() {
  lv_obj_clean(calendarGrid);
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today); struct tm base=today;
  drawnYear = today.tm_year; drawnYday = today.tm_yday;
  base.tm_mday = 1; base.tm_mon += calendarOffset; mktime(&base);
  char title[20]; snprintf(title,sizeof(title),"%s %d",calMonth(base.tm_mon),base.tm_year+1900); lv_label_set_text(calendarTitle,title);
  const char *const wdPt[] = {"D","S","T","Q","Q","S","S"}, *const wdEn[] = {"S","M","T","W","T","F","S"};
  const char *const *wd = gLang == LANG_EN ? wdEn : wdPt;
  for(int i=0;i<7;i++){ auto l=makeLabel(calendarGrid,wd[i],&lv_font_weather_20,i==0?0xF04B54:0xFFFFFF); lv_obj_set_size(l,52,32); lv_obj_set_pos(l,i*60,0); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); }
  int first=base.tm_wday; struct tm next=base; next.tm_mon++; next.tm_mday=0; mktime(&next); int days=next.tm_mday;
  for(int d=1;d<=days;d++){int cell=first+d-1,row=cell/7,col=cell%7; bool selected=(d==today.tm_mday && calendarOffset==0); lv_obj_t *cellBox=lv_obj_create(calendarGrid); lv_obj_set_size(cellBox,44,36); lv_obj_set_pos(cellBox,col*60+4,row*35+34); lv_obj_set_style_bg_opa(cellBox,selected?LV_OPA_COVER:LV_OPA_TRANSP,0); lv_obj_set_style_bg_color(cellBox,lv_color_hex(0xF04B54),0); lv_obj_set_style_border_width(cellBox,0,0); lv_obj_set_style_radius(cellBox,7,0); lv_obj_set_style_pad_all(cellBox,0,0); lv_obj_set_scrollable(cellBox, false); char n[4];snprintf(n,sizeof(n),"%d",d); lv_obj_t *num=lv_label_create(cellBox); lv_label_set_text(num,n); lv_obj_set_style_text_font(num,&lv_font_weather_20,0); lv_obj_set_style_text_color(num,lv_color_hex((col==0&&!selected)?0xF04B54:0xFFFFFF),0); lv_obj_center(num);}
}
static void calendarGesture(lv_event_t *e){lv_event_code_t c=lv_event_get_code(e);lv_indev_t *in=lv_indev_active();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);if(c==LV_EVENT_PRESSED){calTouchStart=p;return;}if(c==LV_EVENT_RELEASED){int dx=p.x-calTouchStart.x;if(abs(dx)>45){calendarOffset+=dx<0?1:-1;drawCalendar();}}}

// On-Enter: redraw the month (the calendar has no remote API).
static void calendarOnEnter() { drawCalendar(); }

// Periodic refresh while the calendar stays open: redraw only when the day
// changed (midnight rollover or NTP sync after boot).
static void calendarRequestUpdate() {
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today);
  if (today.tm_year != drawnYear || today.tm_yday != drawnYday) drawCalendar();
}

static void buildCalendarScreen(lv_obj_t *scr) {
  calendarView = lv_obj_create(scr); lv_obj_set_size(calendarView,SCR_W,SCR_H); lv_obj_set_pos(calendarView,0,0); lv_obj_set_style_bg_color(calendarView,lv_color_hex(0x05091C),0); lv_obj_set_style_bg_opa(calendarView,LV_OPA_COVER,0); lv_obj_set_style_border_width(calendarView,0,0); lv_obj_set_style_pad_all(calendarView,0,0); lv_obj_set_scrollable(calendarView, false); lv_obj_set_hidden(calendarView, true); lv_obj_set_gesture_bubble(calendarView, true);
  calendarTitle=makeLabel(calendarView,"",&lv_font_weather_20,0xFFFFFF); lv_obj_align(calendarTitle,LV_ALIGN_TOP_MID,0,30);
  calendarGrid=lv_obj_create(calendarView); lv_obj_set_size(calendarGrid,430,265); lv_obj_align(calendarGrid,LV_ALIGN_CENTER,0,40); lv_obj_set_style_bg_opa(calendarGrid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(calendarGrid,0,0); lv_obj_set_style_pad_all(calendarGrid,0,0); lv_obj_set_scrollable(calendarGrid, false); lv_obj_set_clickable(calendarGrid, true); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_PRESSED,NULL); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_RELEASED,NULL); addBackButton(calendarView); drawCalendar();

}

const ScreenLifecycle kCalendarScreen = {
  SCREEN_CALENDARIO, "calendario", &calendarView, buildCalendarScreen, calendarOnEnter, nullptr, nullptr,
  calendarRequestUpdate, CALENDARIO_UPDATE_INTERVAL_MS, CALENDARIO_UPDATE_INTERVAL_MS, nullptr, 0, nullptr,
};
