#include "app_screens.h"
#include <time.h>

void buildFinanceScreen(lv_obj_t *scr) {
  financeView=lv_obj_create(scr); lv_obj_set_size(financeView,SCR_W,SCR_H); lv_obj_set_pos(financeView,0,0);
  lv_obj_add_flag(financeView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(financeView,lv_color_hex(0x231C2A),0); lv_obj_set_style_bg_grad_color(financeView,lv_color_hex(0x563A45),0); lv_obj_set_style_bg_grad_dir(financeView,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(financeView,LV_OPA_COVER,0); lv_obj_set_style_border_width(financeView,0,0); lv_obj_set_style_pad_all(financeView,0,0); lv_obj_remove_flag(financeView,LV_OBJ_FLAG_SCROLLABLE);
  financeStrip=lv_obj_create(financeView);lv_obj_set_size(financeStrip,SCR_W,266);lv_obj_set_pos(financeStrip,0,54);lv_obj_set_style_bg_opa(financeStrip,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financeStrip,0,0);lv_obj_set_style_pad_all(financeStrip,0,0);lv_obj_set_style_pad_column(financeStrip,16,0);lv_obj_set_scroll_dir(financeStrip,LV_DIR_HOR);lv_obj_set_flex_flow(financeStrip,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(financeStrip,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  // Keep the 240px cards inside the scroll viewport so the colored top border never gets clipped.
  lv_obj_set_style_pad_bottom(financeStrip,14,0);
  const char *ft[] = {"$  USD/BRL", "€  EUR/BRL", "$  BTC/USD"}; const char *fs[] = {"Dólar para real", "Euro para real", "Bitcoin em dólar"};
  for(int i=0;i<3;i++){ lv_obj_t *c=financeCard[i]=lv_obj_create(financeStrip); lv_obj_set_size(c,210,240); lv_obj_set_style_bg_color(c,lv_color_hex(0x151719),0); lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0); lv_obj_set_style_radius(c,22,0); lv_obj_set_style_border_width(c,2,0); lv_obj_set_style_border_color(c,lv_color_hex(0x45484D),0); lv_obj_set_style_shadow_width(c,12,0); lv_obj_set_style_shadow_opa(c,LV_OPA_40,0); lv_obj_remove_flag(c,LV_OBJ_FLAG_SCROLLABLE); financeHeader[i]=makeLabel(c,ft[i],&lv_font_weather_14,0xAEB0B8); lv_obj_set_pos(financeHeader[i],14,12); lv_obj_t *s=makeLabel(c,fs[i],&lv_font_weather_14,0xAEB0B8); lv_obj_set_pos(s,14,36); financeAgeLabel[i]=makeLabel(c,"hora n/d",&lv_font_weather_14,0x9299A5);lv_obj_set_size(financeAgeLabel[i],72,18);lv_obj_set_pos(financeAgeLabel[i],110,12);lv_obj_set_style_text_align(financeAgeLabel[i],LV_TEXT_ALIGN_RIGHT,0); financePlot[i]=lv_obj_create(c);lv_obj_set_size(financePlot[i],164,56);lv_obj_set_pos(financePlot[i],14,62);lv_obj_set_style_bg_opa(financePlot[i],LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financePlot[i],0,0);lv_obj_set_style_pad_all(financePlot[i],0,0);lv_obj_remove_flag(financePlot[i],LV_OBJ_FLAG_SCROLLABLE); financeLine[i]=lv_line_create(financePlot[i]);lv_obj_set_size(financeLine[i],164,56);lv_obj_set_pos(financeLine[i],0,0);lv_obj_set_style_bg_opa(financeLine[i],LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financeLine[i],0,0);lv_obj_set_style_pad_all(financeLine[i],0,0);lv_obj_set_style_line_width(financeLine[i],2,0);lv_obj_set_style_line_rounded(financeLine[i],true,0);lv_obj_set_style_line_color(financeLine[i],lv_color_hex(0x35D879),0);lv_obj_set_style_line_opa(financeLine[i],LV_OPA_COVER,0); financeDelta[i]=makeLabel(c,"",&lv_font_weather_14,0x30C875);lv_obj_set_pos(financeDelta[i],14,128); financeValue[i]=makeLabel(c,"",&lv_font_montserrat_48,0xEEEEEE); lv_obj_align(financeValue[i],LV_ALIGN_BOTTOM_RIGHT,-10,-5); }
  lv_obj_t *financeTitle=makeLabel(financeView,"Finance",&lv_font_weather_20,0xFFFFFF); lv_obj_align(financeTitle,LV_ALIGN_TOP_LEFT,62,8);
  financeStatusLabel=makeLabel(financeView,"Aguardando cotações da API",&lv_font_weather_14,0xAEB0B8); lv_obj_set_size(financeStatusLabel,250,20); lv_obj_set_pos(financeStatusLabel,218,13); lv_obj_set_style_text_align(financeStatusLabel,LV_TEXT_ALIGN_RIGHT,0);
  addBackButton(financeView); lv_obj_move_foreground(lv_obj_get_child(financeView,lv_obj_get_child_cnt(financeView)-1));

}

void updateFinanceScreen() {
  float fx[3], delta[3], hist[3][24];
  bool fxok[3], deltaOk[3];
  uint8_t histN[3], financeSuccessCount;
  bool financeRefreshing;
  uint32_t financeLastSuccessMs, financeVersion, financeQuoteEpoch[3];
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy(fx, sh.finance, sizeof fx);
  memcpy(fxok, sh.haveFinance, sizeof fxok);
  memcpy(delta, sh.financeChange, sizeof delta);
  memcpy(deltaOk, sh.haveChange, sizeof deltaOk);
  memcpy(hist, sh.history, sizeof hist);
  memcpy(histN, sh.historyCount, sizeof histN);
  financeRefreshing = sh.financeRefreshing;
  financeSuccessCount = sh.financeSuccessCount;
  financeLastSuccessMs = sh.financeLastSuccessMs;
  financeVersion = sh.financeVersion;
  memcpy(financeQuoteEpoch, sh.financeQuoteEpoch, sizeof financeQuoteEpoch);
  xSemaphoreGive(mtx);
  static uint32_t renderedFinanceVersion = UINT32_MAX;
  if (renderedFinanceVersion != financeVersion) {
    for(int i=0;i<3;i++){
      if(fxok[i]) lv_label_set_text_fmt(financeValue[i], i==2?"%.0f":"%.2f", fx[i]);
      else lv_label_set_text(financeValue[i], "");
      lv_color_t trend=deltaOk[i]?lv_color_hex(delta[i]>=0?0x35D879:0xFF5555):lv_color_hex(0xAEB0B8);
      lv_obj_set_style_text_color(financeHeader[i],trend,0);
      lv_obj_set_style_border_color(financeCard[i],deltaOk[i]?trend:lv_color_hex(0x45484D),0);
      if(deltaOk[i]){
        lv_label_set_text_fmt(financeDelta[i], "%s %+.2f%%",delta[i]>=0?LV_SYMBOL_UP:LV_SYMBOL_DOWN,delta[i]);
        lv_obj_set_style_text_color(financeDelta[i],lv_color_hex(delta[i]>=0?0x35D879:0xFF5555),0);
      } else lv_label_set_text(financeDelta[i],"");
      lv_color_t lineColor=deltaOk[i]&&delta[i]<0?lv_color_hex(0xFF5555):lv_color_hex(0x35D879);
      lv_obj_set_style_line_color(financeLine[i],lineColor,0);
      if(histN[i]>1){
        float lo=hist[i][0],hi=lo;
        for(int j=1;j<histN[i];j++){lo=min(lo,hist[i][j]);hi=max(hi,hist[i][j]);}
        if(hi-lo<0.0001f) hi=lo+0.0001f;
        for(int j=0;j<histN[i];j++){
          financeLinePoints[i][j].x=j*163/(histN[i]-1);
          financeLinePoints[i][j].y=52-(int)((hist[i][j]-lo)*44/(hi-lo));
        }
        lv_line_set_points(financeLine[i],financeLinePoints[i],histN[i]);
      } else lv_line_set_points(financeLine[i],NULL,0);
    }
    renderedFinanceVersion = financeVersion;
  }
  char financeStatus[48];
  uint32_t financeAgeMin = financeLastSuccessMs ? (millis() - financeLastSuccessMs) / 60000UL : 0;
  time_t nowEpoch = time(nullptr);
  if (financeRefreshing) snprintf(financeStatus, sizeof financeStatus, "Atualizando cotações...");
  else if (!fxok[0] && !fxok[1] && !fxok[2]) snprintf(financeStatus, sizeof financeStatus, "Aguardando cotações da API");
  else if (financeSuccessCount < 3) snprintf(financeStatus, sizeof financeStatus, "Falha parcial da API · dados mantidos");
  else if (financeAgeMin == 0) snprintf(financeStatus, sizeof financeStatus, "Consulta à API: agora");
  else snprintf(financeStatus, sizeof financeStatus, "Consulta à API: há %lu min", (unsigned long)financeAgeMin);
  lv_label_set_text(financeStatusLabel, financeStatus);
  lv_obj_set_style_text_color(financeStatusLabel, financeRefreshing ? lv_color_hex(0x78D7FF) : (financeSuccessCount < 3 && (fxok[0] || fxok[1] || fxok[2]) ? lv_color_hex(0xFFD477) : lv_color_hex(0xAEB0B8)), 0);
  static uint32_t lastFinanceAgeRender = 0;
  if (lastFinanceAgeRender == 0 || millis() - lastFinanceAgeRender >= 30000UL) {
    lastFinanceAgeRender = millis();
    for (int i=0; i<3; ++i) {
      char ageText[20];
      uint32_t quoteEpoch = financeQuoteEpoch[i];
      if (!fxok[i]) snprintf(ageText, sizeof ageText, "sem cotação");
      else if (!quoteEpoch || nowEpoch < 1700000000 || nowEpoch < (time_t)quoteEpoch) snprintf(ageText, sizeof ageText, "hora n/d");
      else {
        uint32_t ageSec = (uint32_t)(nowEpoch - (time_t)quoteEpoch);
        if (ageSec < 60) snprintf(ageText, sizeof ageText, "agora");
        else if (ageSec < 3600) snprintf(ageText, sizeof ageText, "há %lum", (unsigned long)(ageSec/60));
        else snprintf(ageText, sizeof ageText, "há %luh%02lum", (unsigned long)(ageSec/3600), (unsigned long)((ageSec%3600)/60));
      }
      lv_label_set_text(financeAgeLabel[i], ageText);
      uint32_t ageSec = quoteEpoch && nowEpoch > (time_t)quoteEpoch ? (uint32_t)(nowEpoch - (time_t)quoteEpoch) : 0;
      lv_obj_set_style_text_color(financeAgeLabel[i], ageSec >= 900 ? lv_color_hex(0xFFD477) : lv_color_hex(0x9299A5), 0);
    }
  }

}
