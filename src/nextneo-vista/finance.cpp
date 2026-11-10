#include "app_screens.h"
#include <WiFi.h>
#include <time.h>
#include <stdint.h>

// Same cards/layout as before; there is simply one card per configured asset
// (default USD/BRL, EUR/BRL, BTC/USD). Extra assets continue in the same
// horizontally scrolling strip.
static lv_obj_t *financeSub[FINANCE_MAX_ASSETS];
static uint8_t builtCount = 0;
static uint32_t builtConfigVersion = UINT32_MAX;
static uint32_t renderedFinanceVersion = UINT32_MAX;
static uint32_t lastFinanceAgeRender = 0;

static void refreshQuoteEvent(lv_event_t *event) {
  const intptr_t index = (intptr_t)lv_event_get_user_data(event);
  if (index < 0 || index >= builtCount) return;
  financeSingleIndex = (int8_t)index;
  postScreenJob(CMD_FINANCE_ONE);
}

// Price with sensible precision: 106543 | 229.87 | 5.4312
static void formatPrice(float v, char *out, size_t size) {
  if (v >= 10000) snprintf(out, size, "%.0f", v);
  else if (v >= 1) snprintf(out, size, "%.2f", v);
  else snprintf(out, size, "%.4f", v);
}

static void buildCard(int i) {
  lv_obj_t *c = financeCard[i] = lv_obj_create(financeStrip);
  lv_obj_set_size(c, 210, 240);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x151719), 0); lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0); lv_obj_set_style_border_width(c, 2, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(0x45484D), 0);
  // no blurred shadow: it cost 150-700 ms per frame while scrolling (see [perf]); a lighter top edge keeps the depth
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c, refreshQuoteEvent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  financeHeader[i] = makeLabel(c, "", &lv_font_weather_14, 0xAEB0B8);
  lv_obj_set_pos(financeHeader[i], 14, 12);
  lv_obj_set_width(financeHeader[i], 96);
  lv_label_set_long_mode(financeHeader[i], LV_LABEL_LONG_DOT);
  financeSub[i] = makeLabel(c, "", &lv_font_weather_14, 0xAEB0B8);
  lv_obj_set_pos(financeSub[i], 14, 36);
  lv_obj_set_width(financeSub[i], 170);
  lv_label_set_long_mode(financeSub[i], LV_LABEL_LONG_DOT);
  financeAgeLabel[i] = makeLabel(c, "hora n/d", &lv_font_weather_14, 0x9299A5);
  lv_obj_set_size(financeAgeLabel[i], 72, 18); lv_obj_set_pos(financeAgeLabel[i], 110, 12);
  lv_obj_set_style_text_align(financeAgeLabel[i], LV_TEXT_ALIGN_RIGHT, 0);
  financePlot[i] = lv_obj_create(c);
  lv_obj_set_size(financePlot[i], 164, 56); lv_obj_set_pos(financePlot[i], 14, 62);
  lv_obj_set_style_bg_opa(financePlot[i], LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(financePlot[i], 0, 0);
  lv_obj_set_style_pad_all(financePlot[i], 0, 0); lv_obj_remove_flag(financePlot[i], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(financePlot[i], LV_OBJ_FLAG_CLICKABLE);
  financeLine[i] = lv_line_create(financePlot[i]);
  lv_obj_set_size(financeLine[i], 164, 56); lv_obj_set_pos(financeLine[i], 0, 0);
  lv_obj_set_style_line_width(financeLine[i], 2, 0); lv_obj_set_style_line_rounded(financeLine[i], true, 0);
  lv_obj_set_style_line_color(financeLine[i], lv_color_hex(0x35D879), 0);
  financeDelta[i] = makeLabel(c, "", &lv_font_weather_14, 0x30C875);
  lv_obj_set_pos(financeDelta[i], 14, 128);
  financeValue[i] = makeLabel(c, "", &lv_font_montserrat_48, 0xEEEEEE);
  lv_obj_align(financeValue[i], LV_ALIGN_BOTTOM_RIGHT, -10, -5);
}

// UI thread: rebuild the cards when the asset list changed.
static void rebuildFinanceCards() {
  FinanceAsset assets[FINANCE_MAX_ASSETS];
  char currency[FINANCE_MAX_ASSETS][4];
  uint8_t count;
  xSemaphoreTake(mtx, portMAX_DELAY);
  count = sh.financeAssetCount;
  memcpy(assets, sh.financeAssets, sizeof assets);
  memcpy(currency, sh.financeCurrency, sizeof currency);
  builtConfigVersion = sh.financeConfigVersion;
  xSemaphoreGive(mtx);
  lv_obj_clean(financeStrip);
  for (int i = 0; i < FINANCE_MAX_ASSETS; ++i) financeCard[i] = nullptr;
  for (int i = 0; i < count; ++i) buildCard(i);
  builtCount = count;
  renderedFinanceVersion = UINT32_MAX;
  lastFinanceAgeRender = 0;
}

void buildFinanceScreen(lv_obj_t *scr) {
  financeView=lv_obj_create(scr); lv_obj_set_size(financeView,SCR_W,SCR_H); lv_obj_set_pos(financeView,0,0);
  lv_obj_add_flag(financeView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(financeView,lv_color_hex(0x231C2A),0); lv_obj_set_style_bg_grad_color(financeView,lv_color_hex(0x563A45),0); lv_obj_set_style_bg_grad_dir(financeView,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(financeView,LV_OPA_COVER,0); lv_obj_set_style_border_width(financeView,0,0); lv_obj_set_style_pad_all(financeView,0,0); lv_obj_remove_flag(financeView,LV_OBJ_FLAG_SCROLLABLE);
  financeStrip=lv_obj_create(financeView);lv_obj_set_size(financeStrip,SCR_W,266);lv_obj_set_pos(financeStrip,0,54);lv_obj_set_style_bg_opa(financeStrip,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financeStrip,0,0);lv_obj_set_style_pad_all(financeStrip,0,0);lv_obj_set_style_pad_column(financeStrip,16,0);lv_obj_set_scroll_dir(financeStrip,LV_DIR_HOR);lv_obj_set_flex_flow(financeStrip,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(financeStrip,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  // Keep the 240px cards inside the scroll viewport so the colored top border never gets clipped.
  lv_obj_set_style_pad_bottom(financeStrip,14,0);
  rebuildFinanceCards();
  lv_obj_t *financeTitle=makeLabel(financeView,"Finance",&lv_font_weather_20,0xFFFFFF); lv_obj_align(financeTitle,LV_ALIGN_TOP_LEFT,62,8);
  financeStatusLabel=makeLabel(financeView,"Aguardando cotações da API",&lv_font_weather_14,0xAEB0B8); lv_obj_set_size(financeStatusLabel,250,20); lv_obj_set_pos(financeStatusLabel,218,13); lv_obj_set_style_text_align(financeStatusLabel,LV_TEXT_ALIGN_RIGHT,0);
  addBackButton(financeView); lv_obj_move_foreground(lv_obj_get_child(financeView,lv_obj_get_child_cnt(financeView)-1));
}

// On-Enter: repaint cached quotes and ages immediately.
void financeOnEnter() {
  renderedFinanceVersion = UINT32_MAX;
  lastFinanceAgeRender = 0;
}

// UI thread, active Finance screen only.
void updateFinanceScreen() {
  static float fx[FINANCE_MAX_ASSETS], delta[FINANCE_MAX_ASSETS], hist[FINANCE_MAX_ASSETS][24];
  static bool fxok[FINANCE_MAX_ASSETS], deltaOk[FINANCE_MAX_ASSETS];
  static uint8_t histN[FINANCE_MAX_ASSETS];
  static uint32_t financeQuoteEpoch[FINANCE_MAX_ASSETS];
  static FinanceAsset assets[FINANCE_MAX_ASSETS];
  static char currency[FINANCE_MAX_ASSETS][4];
  uint8_t financeSuccessCount, count;
  bool financeRefreshing;
  uint32_t financeLastSuccessMs, financeVersion, configVersion;
  xSemaphoreTake(mtx, portMAX_DELAY);
  configVersion = sh.financeConfigVersion;
  xSemaphoreGive(mtx);
  if (configVersion != builtConfigVersion) rebuildFinanceCards();

  xSemaphoreTake(mtx, portMAX_DELAY);
  count = sh.financeAssetCount;
  memcpy(assets, sh.financeAssets, sizeof assets);
  memcpy(currency, sh.financeCurrency, sizeof currency);
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
  if (count > builtCount) count = builtCount;

  bool anyOk = false;
  for (int i = 0; i < count; ++i) anyOk |= fxok[i];

  if (renderedFinanceVersion != financeVersion) {
    for (int i = 0; i < count; i++) {
      char name[24], header[32], text[24];
      financeDisplayName(assets[i].symbol, name, sizeof name);
      const char *sym = currencySymbol(currency[i]);
      if (*sym) snprintf(header, sizeof header, "%s  %s", sym, name);
      else snprintf(header, sizeof header, "%s", name);
      setLabelIfChanged(financeHeader[i], header);
      if (assets[i].label[0]) setLabelIfChanged(financeSub[i], assets[i].label);
      else if (currency[i][0]) { snprintf(text, sizeof text, "Cotação em %s", currency[i]); setLabelIfChanged(financeSub[i], text); }
      else setLabelIfChanged(financeSub[i], assets[i].symbol);
      if (fxok[i]) { formatPrice(fx[i], text, sizeof text); setLabelIfChanged(financeValue[i], text); }
      else setLabelIfChanged(financeValue[i], "");
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
  else if (!anyOk) snprintf(financeStatus, sizeof financeStatus, "Aguardando cotações da API");
  else if (financeSuccessCount < count) snprintf(financeStatus, sizeof financeStatus, "Falha parcial da API · dados mantidos");
  else if (financeAgeMin == 0) snprintf(financeStatus, sizeof financeStatus, "Consulta à API: agora");
  else snprintf(financeStatus, sizeof financeStatus, "Consulta à API: há %lu min", (unsigned long)financeAgeMin);
  setLabelIfChanged(financeStatusLabel, financeStatus);
  const uint32_t statusColor = financeRefreshing ? 0x78D7FF : (financeSuccessCount < count && anyOk ? 0xFFD477 : 0xAEB0B8);
  static uint32_t renderedStatusColor = 0xFFFFFFFF;
  if (statusColor != renderedStatusColor) {
    lv_obj_set_style_text_color(financeStatusLabel, lv_color_hex(statusColor), 0);
    renderedStatusColor = statusColor;
  }
  if (lastFinanceAgeRender == 0 || millis() - lastFinanceAgeRender >= 30000UL) {
    lastFinanceAgeRender = millis();
    for (int i=0; i<count; ++i) {
      char ageText[20];
      uint32_t quoteEpoch = financeQuoteEpoch[i];
      if (!fxok[i]) snprintf(ageText, sizeof ageText, "sem cotação");
      else if (!quoteEpoch || nowEpoch < 1700000000 || nowEpoch < (time_t)quoteEpoch) snprintf(ageText, sizeof ageText, "hora n/d");
      else {
        uint32_t ageSec = (uint32_t)(nowEpoch - (time_t)quoteEpoch);
        if (ageSec < 60) snprintf(ageText, sizeof ageText, "agora");
        else if (ageSec < 3600) snprintf(ageText, sizeof ageText, "há %lum", (unsigned long)(ageSec/60));
        else if (ageSec < 86400) snprintf(ageText, sizeof ageText, "há %luh%02lum", (unsigned long)(ageSec/3600), (unsigned long)((ageSec%3600)/60));
        else snprintf(ageText, sizeof ageText, "há %lud", (unsigned long)(ageSec/86400));
      }
      setLabelIfChanged(financeAgeLabel[i], ageText);
      uint32_t ageSec = quoteEpoch && nowEpoch > (time_t)quoteEpoch ? (uint32_t)(nowEpoch - (time_t)quoteEpoch) : 0;
      lv_obj_set_style_text_color(financeAgeLabel[i], ageSec >= 900 ? lv_color_hex(0xFFD477) : lv_color_hex(0x9299A5), 0);
    }
  }
}
