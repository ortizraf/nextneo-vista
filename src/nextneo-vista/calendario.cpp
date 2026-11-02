#include "app_screens.h"
#include <time.h>
#include <stdlib.h>

static const char *calMonths[] = {"JAN.","FEV.","MAR.","ABR.","MAI.","JUN.","JUL.","AGO.","SET.","OUT.","NOV.","DEZ."};
void drawCalendar() {
  lv_obj_clean(calendarGrid);
  time_t now = time(nullptr); struct tm today; localtime_r(&now, &today); struct tm base=today;
  base.tm_mday = 1; base.tm_mon += calendarOffset; mktime(&base);
  char title[20]; snprintf(title,sizeof(title),"%s %d",calMonths[base.tm_mon],base.tm_year+1900); lv_label_set_text(calendarTitle,title);
  const char *wd[] = {"D","S","T","Q","Q","S","S"};
  for(int i=0;i<7;i++){ auto l=makeLabel(calendarGrid,wd[i],&lv_font_weather_20,i==0?0xF04B54:0xFFFFFF); lv_obj_set_size(l,52,32); lv_obj_set_pos(l,i*60,0); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); }
  int first=base.tm_wday; struct tm next=base; next.tm_mon++; next.tm_mday=0; mktime(&next); int days=next.tm_mday;
  for(int d=1;d<=days;d++){int cell=first+d-1,row=cell/7,col=cell%7; bool selected=(d==today.tm_mday && calendarOffset==0); lv_obj_t *cellBox=lv_obj_create(calendarGrid); lv_obj_set_size(cellBox,44,36); lv_obj_set_pos(cellBox,col*60+4,row*35+34); lv_obj_set_style_bg_opa(cellBox,selected?LV_OPA_COVER:LV_OPA_TRANSP,0); lv_obj_set_style_bg_color(cellBox,lv_color_hex(0xF04B54),0); lv_obj_set_style_border_width(cellBox,0,0); lv_obj_set_style_radius(cellBox,7,0); lv_obj_set_style_pad_all(cellBox,0,0); lv_obj_remove_flag(cellBox,LV_OBJ_FLAG_SCROLLABLE); char n[4];snprintf(n,sizeof(n),"%d",d); lv_obj_t *num=lv_label_create(cellBox); lv_label_set_text(num,n); lv_obj_set_style_text_font(num,&lv_font_weather_20,0); lv_obj_set_style_text_color(num,lv_color_hex((col==0&&!selected)?0xF04B54:0xFFFFFF),0); lv_obj_center(num);}
}
static void calendarGesture(lv_event_t *e){lv_event_code_t c=lv_event_get_code(e);lv_indev_t *in=lv_indev_active();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);if(c==LV_EVENT_PRESSED){calTouchStart=p;return;}if(c==LV_EVENT_RELEASED){int dx=p.x-calTouchStart.x;if(abs(dx)>45){calendarOffset+=dx<0?1:-1;drawCalendar();}}}

void buildCalendarScreen(lv_obj_t *scr) {
  calendarView = lv_obj_create(scr); lv_obj_set_size(calendarView,SCR_W,SCR_H); lv_obj_set_pos(calendarView,0,0); lv_obj_set_style_bg_color(calendarView,lv_color_hex(0x05091C),0); lv_obj_set_style_bg_opa(calendarView,LV_OPA_COVER,0); lv_obj_set_style_border_width(calendarView,0,0); lv_obj_set_style_pad_all(calendarView,0,0); lv_obj_remove_flag(calendarView,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(calendarView,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(calendarView,LV_OBJ_FLAG_GESTURE_BUBBLE);
  calendarTitle=makeLabel(calendarView,"",&lv_font_weather_20,0xFFFFFF); lv_obj_align(calendarTitle,LV_ALIGN_TOP_MID,0,30);
  calendarGrid=lv_obj_create(calendarView); lv_obj_set_size(calendarGrid,430,265); lv_obj_align(calendarGrid,LV_ALIGN_CENTER,0,40); lv_obj_set_style_bg_opa(calendarGrid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(calendarGrid,0,0); lv_obj_set_style_pad_all(calendarGrid,0,0); lv_obj_remove_flag(calendarGrid,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(calendarGrid,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_PRESSED,NULL); lv_obj_add_event_cb(calendarGrid,calendarGesture,LV_EVENT_RELEASED,NULL); addBackButton(calendarView); drawCalendar();

}
