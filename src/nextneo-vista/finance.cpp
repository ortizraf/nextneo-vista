#include "app_screens.h"
#include <WiFi.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include "services/psram_alloc.h"

// Same cards/layout as before; there is simply one card per configured asset
// (default USD/BRL, EUR/BRL, BTC/USD). Extra assets continue in the same
// horizontally scrolling strip.
static lv_obj_t *financeSub[FINANCE_MAX_ASSETS];
static uint8_t builtCount = 0;
static uint32_t builtConfigVersion = UINT32_MAX;
static uint32_t renderedFinanceVersion = UINT32_MAX;
static uint32_t lastFinanceAgeRender = 0;

// ---- upgrade: sparkline fill + previous-close line + last-point dot, detail overlay, showcase
static constexpr int PLOT_W = 164, PLOT_H = 56;
static constexpr uint32_t CARD_BG = 0x151719;
static lv_obj_t *financeFillImg[FINANCE_MAX_ASSETS], *financeBase[FINANCE_MAX_ASSETS], *financeDot[FINANCE_MAX_ASSETS];
static uint16_t *financeFillBuf[FINANCE_MAX_ASSETS];          // PSRAM, allocated once per slot
static lv_image_dsc_t financeFillDsc[FINANCE_MAX_ASSETS];
static lv_point_precise_t financeBasePts[FINANCE_MAX_ASSETS][2];
static void openDetail(int index);
static void detailRender(bool financeChanged);
static void closeDetail();
static bool detailOpen();
static void showcaseTick();
static void buildDetailPanel();

// Area under a polyline: vertical fade from `color` (alpha a0 at the curve) to the
// background at the bottom. Drawn into an opaque RGB565 buffer; the LVGL line (anti-
// aliased) is drawn on top of it. Only runs when the data changes.
static void drawAreaFill(uint16_t *buf, int w, int h, const lv_point_precise_t *pts, int n,
                         uint32_t bg, uint32_t color, uint8_t a0) {
  const int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
  const int fr = (color >> 16) & 255, fg = (color >> 8) & 255, fb = color & 255;
  auto px = [&](int a) -> uint16_t {
    const int r = br + (fr - br) * a / 255, g = bgc + (fg - bgc) * a / 255, b = bb + (fb - bb) * a / 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  };
  const uint16_t bgPx = px(0);
  for (int i = 0; i < w * h; ++i) buf[i] = bgPx;
  if (n < 2) return;
  int seg = 0;
  for (int x = 0; x < w; ++x) {
    while (seg < n - 2 && pts[seg + 1].x < x) ++seg;
    const float x0 = pts[seg].x, x1 = pts[seg + 1].x;
    if (x < x0 || x > pts[n - 1].x) continue;
    const float t = x1 > x0 ? (x - x0) / (x1 - x0) : 0;
    int top = (int)(pts[seg].y + (pts[seg + 1].y - pts[seg].y) * t);
    if (top < 0) top = 0;
    if (top >= h) continue;
    const int span = h - top;
    for (int y = top; y < h; ++y) buf[y * w + x] = px(a0 * (h - y) / span);
  }
}

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
  // tap = refresh this quote (as before); hold = detail screen
  lv_obj_add_event_cb(c, refreshQuoteEvent, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
  lv_obj_add_event_cb(c, [](lv_event_t *e) { openDetail((int)(intptr_t)lv_event_get_user_data(e)); },
                      LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
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
  if (!financeFillBuf[i]) financeFillBuf[i] = (uint16_t *)psramMalloc(PLOT_W * PLOT_H * 2);
  financeFillImg[i] = nullptr;
  if (financeFillBuf[i]) {
    lv_image_dsc_t &d = financeFillDsc[i];
    d = {};
    d.header.magic = LV_IMAGE_HEADER_MAGIC; d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = PLOT_W; d.header.h = PLOT_H; d.header.stride = PLOT_W * 2;
    d.data_size = PLOT_W * PLOT_H * 2; d.data = (const uint8_t *)financeFillBuf[i];
    for (int k = 0; k < PLOT_W * PLOT_H; ++k) financeFillBuf[i][k] = 0x10A3;   // ~CARD_BG
    financeFillImg[i] = lv_image_create(financePlot[i]);
    lv_image_set_src(financeFillImg[i], &d);
    lv_obj_set_pos(financeFillImg[i], 0, 0);
  }
  financeBase[i] = lv_line_create(financePlot[i]);           // previous close, dashed
  lv_obj_set_size(financeBase[i], PLOT_W, PLOT_H); lv_obj_set_pos(financeBase[i], 0, 0);
  lv_obj_set_style_line_width(financeBase[i], 1, 0);
  lv_obj_set_style_line_dash_width(financeBase[i], 4, 0); lv_obj_set_style_line_dash_gap(financeBase[i], 4, 0);
  lv_obj_set_style_line_color(financeBase[i], lv_color_hex(0x8A909C), 0);
  lv_obj_set_style_line_opa(financeBase[i], LV_OPA_60, 0);
  lv_obj_add_flag(financeBase[i], LV_OBJ_FLAG_HIDDEN);
  financeLine[i] = lv_line_create(financePlot[i]);
  lv_obj_set_size(financeLine[i], 164, 56); lv_obj_set_pos(financeLine[i], 0, 0);
  lv_obj_set_style_line_width(financeLine[i], 2, 0); lv_obj_set_style_line_rounded(financeLine[i], true, 0);
  lv_obj_set_style_line_color(financeLine[i], lv_color_hex(0x35D879), 0);
  financeDot[i] = lv_obj_create(financePlot[i]);             // last point
  lv_obj_set_size(financeDot[i], 7, 7); lv_obj_set_style_radius(financeDot[i], LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(financeDot[i], 0, 0); lv_obj_set_style_pad_all(financeDot[i], 0, 0);
  lv_obj_remove_flag(financeDot[i], LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(financeDot[i], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(financeDot[i], LV_OBJ_FLAG_HIDDEN);
  financeDelta[i] = makeLabel(c, "", &lv_font_weather_14, 0x30C875);
  lv_obj_set_pos(financeDelta[i], 14, 128);
  financeValue[i] = makeLabel(c, "", &lv_font_montserrat_48, 0xEEEEEE);
  lv_obj_align(financeValue[i], LV_ALIGN_BOTTOM_MID, 0, -5);   // centred; font shrinks for long prices (setFinanceValue)
}

// Price label: Montserrat 48 fits up to 5 digits in the card (~174 px inside); 6+ digits
// ("7592.40", "106543") use 42 so nothing spills over the card edge. Kept centred.
static void setFinanceValue(lv_obj_t *label, const char *text) {
  int digits = 0;
  for (const char *p = text; *p; ++p) if (*p >= '0' && *p <= '9') ++digits;
  const lv_font_t *font = digits > 5 ? &lv_font_montserrat_42 : &lv_font_montserrat_48;
  if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font) lv_obj_set_style_text_font(label, font, 0);
  setLabelIfChanged(label, text);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, digits > 5 ? -7 : -5);    // 42 is a bit shorter: same baseline area
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
  lv_obj_set_style_bg_color(financeView,lv_color_hex(0x050E24),0); lv_obj_set_style_bg_grad_color(financeView,lv_color_hex(0x0B2A6B),0); lv_obj_set_style_bg_grad_dir(financeView,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(financeView,LV_OPA_COVER,0); lv_obj_set_style_border_width(financeView,0,0); lv_obj_set_style_pad_all(financeView,0,0); lv_obj_remove_flag(financeView,LV_OBJ_FLAG_SCROLLABLE);
  financeStrip=lv_obj_create(financeView);lv_obj_set_size(financeStrip,SCR_W,266);lv_obj_set_pos(financeStrip,0,54);lv_obj_set_style_bg_opa(financeStrip,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(financeStrip,0,0);lv_obj_set_style_pad_all(financeStrip,0,0);lv_obj_set_style_pad_column(financeStrip,16,0);lv_obj_set_scroll_dir(financeStrip,LV_DIR_HOR);lv_obj_set_flex_flow(financeStrip,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(financeStrip,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  // Keep the 240px cards inside the scroll viewport so the colored top border never gets clipped.
  lv_obj_set_style_pad_bottom(financeStrip,14,0);
  rebuildFinanceCards();
  lv_obj_t *financeTitle=makeLabel(financeView,"Finance",&lv_font_weather_20,0xFFFFFF); lv_obj_align(financeTitle,LV_ALIGN_TOP_LEFT,62,8);
  financeStatusLabel=makeLabel(financeView,"Aguardando cotações da API",&lv_font_weather_14,0xAEB0B8); lv_obj_set_size(financeStatusLabel,250,20); lv_obj_set_pos(financeStatusLabel,218,13); lv_obj_set_style_text_align(financeStatusLabel,LV_TEXT_ALIGN_RIGHT,0);
  addBackButton(financeView); lv_obj_move_foreground(lv_obj_get_child(financeView,lv_obj_get_child_cnt(financeView)-1));
  buildDetailPanel();
}

// On-Enter: repaint cached quotes and ages immediately.
void financeOnEnter() {
  renderedFinanceVersion = UINT32_MAX;
  lastFinanceAgeRender = 0;
}

// UI thread, active Finance screen only.
void updateFinanceScreen() {
  static float fx[FINANCE_MAX_ASSETS], delta[FINANCE_MAX_ASSETS], hist[FINANCE_MAX_ASSETS][FINANCE_HISTORY], prevClose[FINANCE_MAX_ASSETS];
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
  memcpy(prevClose, sh.previousFinance, sizeof prevClose);
  financeRefreshing = sh.financeRefreshing;
  financeSuccessCount = sh.financeSuccessCount;
  financeLastSuccessMs = sh.financeLastSuccessMs;
  financeVersion = sh.financeVersion;
  memcpy(financeQuoteEpoch, sh.financeQuoteEpoch, sizeof financeQuoteEpoch);
  xSemaphoreGive(mtx);
  if (count > builtCount) count = builtCount;

  bool anyOk = false;
  for (int i = 0; i < count; ++i) anyOk |= fxok[i];

  const bool financeChanged = renderedFinanceVersion != financeVersion;
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
      if (fxok[i]) { formatPrice(fx[i], text, sizeof text); setFinanceValue(financeValue[i], text); }
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
      const uint32_t lineHex = deltaOk[i] && delta[i] < 0 ? 0xFF5555 : 0x35D879;
      if(histN[i]>1){
        float lo=hist[i][0],hi=lo;
        for(int j=1;j<histN[i];j++){lo=min(lo,hist[i][j]);hi=max(hi,hist[i][j]);}
        // the previous close joins the scale so its dashed line is always on the plot
        const bool base = prevClose[i] > 0 && isfinite(prevClose[i]);
        if (base) { lo = min(lo, prevClose[i]); hi = max(hi, prevClose[i]); }
        if(hi-lo<0.0001f) hi=lo+0.0001f;
        for(int j=0;j<histN[i];j++){
          financeLinePoints[i][j].x=j*163/(histN[i]-1);
          financeLinePoints[i][j].y=52-(int)((hist[i][j]-lo)*44/(hi-lo));
        }
        lv_line_set_points(financeLine[i],financeLinePoints[i],histN[i]);
        if (base) {
          const int by = 52 - (int)((prevClose[i] - lo) * 44 / (hi - lo));
          financeBasePts[i][0] = {0, by}; financeBasePts[i][1] = {PLOT_W - 1, by};
          lv_line_set_points(financeBase[i], financeBasePts[i], 2);
          lv_obj_remove_flag(financeBase[i], LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(financeBase[i], LV_OBJ_FLAG_HIDDEN);
        const lv_point_precise_t &last = financeLinePoints[i][histN[i] - 1];
        lv_obj_set_style_bg_color(financeDot[i], lv_color_hex(lineHex), 0);
        lv_obj_set_pos(financeDot[i], (int)last.x - 3, (int)last.y - 3);
        lv_obj_remove_flag(financeDot[i], LV_OBJ_FLAG_HIDDEN);
        if (financeFillImg[i]) {
          drawAreaFill(financeFillBuf[i], PLOT_W, PLOT_H, financeLinePoints[i], histN[i], CARD_BG, lineHex, 70);
          lv_obj_invalidate(financeFillImg[i]);
        }
      } else {
        lv_line_set_points(financeLine[i],NULL,0);
        lv_obj_add_flag(financeBase[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(financeDot[i], LV_OBJ_FLAG_HIDDEN);
        if (financeFillImg[i]) { drawAreaFill(financeFillBuf[i], PLOT_W, PLOT_H, nullptr, 0, CARD_BG, lineHex, 0); lv_obj_invalidate(financeFillImg[i]); }
      }
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
      if (!fxok[i]) snprintf(ageText, sizeof ageText, "n/d");   // "sem cotação" wrapped in 72 px and showed only "sem"
      else if (!quoteEpoch || nowEpoch < 1700000000 || nowEpoch < (time_t)quoteEpoch) snprintf(ageText, sizeof ageText, "hora n/d");
      else {
        uint32_t ageSec = (uint32_t)(nowEpoch - (time_t)quoteEpoch);
        if (ageSec < 60) snprintf(ageText, sizeof ageText, "agora");
        else if (ageSec < 3600) snprintf(ageText, sizeof ageText, "há %lum", (unsigned long)(ageSec/60));
        else if (ageSec < 86400) snprintf(ageText, sizeof ageText, "há %luh", (unsigned long)(ageSec/3600));   // short: fits the 72 px label
        else snprintf(ageText, sizeof ageText, "há %lud", (unsigned long)(ageSec/86400));
      }
      setLabelIfChanged(financeAgeLabel[i], ageText);
      uint32_t ageSec = quoteEpoch && nowEpoch > (time_t)quoteEpoch ? (uint32_t)(nowEpoch - (time_t)quoteEpoch) : 0;
      lv_obj_set_style_text_color(financeAgeLabel[i], ageSec >= 900 ? lv_color_hex(0xFFD477) : lv_color_hex(0x9299A5), 0);
    }
  }
  detailRender(financeChanged);
  showcaseTick();
}

// ================================================================ detail overlay
// Hold a card: price, change, market state, chart with 1D/5D/1M/6M/1A and key figures.
// Data: CMD_FINANCE_DETAIL (network task, Finance screen only, on demand, cached per range).
static constexpr int DCH_W = 300, DCH_H = 140;
static constexpr uint32_t DETAIL_BG = 0x0A1122;
static constexpr uint16_t DETAIL_BG565 = 0x0884;                      // DETAIL_BG in RGB565
static lv_obj_t *dPanel, *dName, *dSub, *dState, *dPrice, *dChange, *dTabs, *dChartImg, *dLine, *dBase, *dDot,
                *dLoading, *dT0, *dT1, *dPeriod, *dStatVal[8];
static uint16_t *dChartBuf;
static lv_image_dsc_t dChartDsc;
static lv_point_precise_t dPts[FIN_DETAIL_POINTS], dBasePts[2];
static int dIndex = -1;
static uint8_t dRange = 0;
static uint32_t dShownVersion = UINT32_MAX, dRequestedMs = 0;
static float dPrev = NAN;                                     // previous close of the asset (card data)
static FinanceDetail dData;                                   // UI copy
static const char *const kStatCaption[8] = {"Fech. anterior", "Abertura", "M\xC3\xA1x. do dia", "M\xC3\xADn. do dia",
                                            "M\xC3\xA1x. per\xC3\xADodo", "M\xC3\xADn. per\xC3\xADodo", "M\xC3\xA1x. 52 sem.", "M\xC3\xADn. 52 sem."};

static bool detailOpen() { return dPanel && !lv_obj_has_flag(dPanel, LV_OBJ_FLAG_HIDDEN); }

static void closeDetail() {
  if (!dPanel) return;
  lv_obj_add_flag(dPanel, LV_OBJ_FLAG_HIDDEN);
  dIndex = -1;
}

static void requestDetail() {
  if (dIndex < 0) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(financeDetailWant.symbol, sizeof financeDetailWant.symbol, "%s", sh.financeAssets[dIndex].symbol);
  financeDetailWant.range = dRange;
  xSemaphoreGive(mtx);
  dRequestedMs = millis();
  lv_label_set_text(dLoading, "carregando...");
  lv_obj_remove_flag(dLoading, LV_OBJ_FLAG_HIDDEN);
  postScreenJob(CMD_FINANCE_DETAIL);       // deduplicated; detailRender re-posts if the answer is for an older request
}

static void fmtTime(uint32_t epoch, uint8_t range, char *out, size_t n) {
  if (!epoch) { out[0] = 0; return; }
  const time_t t = (time_t)epoch;
  struct tm tm; localtime_r(&t, &tm);
  if (range == 0) strftime(out, n, "%H:%M", &tm);
  else if (range == 1) strftime(out, n, "%d/%m %H:%M", &tm);
  else strftime(out, n, "%d/%m/%y", &tm);
}

static void drawDetailChart(uint32_t lineHex) {
  const int n = dData.ok ? dData.count : 0;
  float lo = dData.periodLow, hi = dData.periodHigh;
  const float ref = dData.range == 0 ? dPrev : dData.rangeRef;
  const bool base = n > 1 && isfinite(ref) && ref > 0;
  if (base) { lo = min(lo, ref); hi = max(hi, ref); }
  if (hi - lo < 0.0001f) { hi += 0.00005f; lo -= 0.00005f; }
  const float pad = (hi - lo) * 0.08f; lo -= pad; hi += pad;
  auto yOf = [&](float v) { return (int32_t)((hi - v) * (DCH_H - 1) / (hi - lo)); };
  for (int j = 0; j < n; ++j) { dPts[j].x = n > 1 ? j * (DCH_W - 1) / (n - 1) : 0; dPts[j].y = yOf(dData.series[j]); }
  if (dChartBuf) {
    drawAreaFill(dChartBuf, DCH_W, DCH_H, dPts, n > 1 ? n : 0, DETAIL_BG, lineHex, 80);
    for (int g = 1; g <= 3; ++g) {                                       // faint dotted grid, only on plain background
      uint16_t *row = dChartBuf + (g * DCH_H / 4) * DCH_W;
      for (int x = 0; x < DCH_W; x += 2) if (row[x] == DETAIL_BG565) row[x] = 0x10E5;
    }
    lv_obj_invalidate(dChartImg);
  }
  lv_obj_set_style_line_color(dLine, lv_color_hex(lineHex), 0);
  if (n > 1) lv_line_set_points(dLine, dPts, n); else lv_line_set_points(dLine, NULL, 0);
  if (base) {
    const int32_t by = yOf(ref);
    dBasePts[0] = {0, by}; dBasePts[1] = {DCH_W - 1, by};
    lv_line_set_points(dBase, dBasePts, 2);
    lv_obj_remove_flag(dBase, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(dBase, LV_OBJ_FLAG_HIDDEN);
  if (n > 1) {
    lv_obj_set_style_bg_color(dDot, lv_color_hex(lineHex), 0);
    lv_obj_set_pos(dDot, lv_obj_get_x(dChartImg) + (int)dPts[n - 1].x - 4, lv_obj_get_y(dChartImg) + (int)dPts[n - 1].y - 4);
    lv_obj_remove_flag(dDot, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(dDot, LV_OBJ_FLAG_HIDDEN);
}

// Header (name, price, day change) from the card data: always current, even before the chart arrives.
static void detailHeader() {
  if (dIndex < 0) return;
  char name[24], sub[48], b[48];
  xSemaphoreTake(mtx, portMAX_DELAY);
  const FinanceAsset a = sh.financeAssets[dIndex];
  const float price = sh.finance[dIndex], prev = sh.previousFinance[dIndex], pct = sh.financeChange[dIndex];
  const bool have = sh.haveFinance[dIndex], haveCh = sh.haveChange[dIndex];
  char cur[4]; memcpy(cur, sh.financeCurrency[dIndex], sizeof cur);
  xSemaphoreGive(mtx);
  dPrev = prev > 0 ? prev : NAN;
  financeDisplayName(a.symbol, name, sizeof name);
  setLabelIfChanged(dName, a.label[0] ? a.label : name);
  if (cur[0]) snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", a.symbol, cur); else snprintf(sub, sizeof sub, "%s", a.symbol);
  setLabelIfChanged(dSub, sub);
  if (have) { formatPrice(price, b, sizeof b); setLabelIfChanged(dPrice, b); } else setLabelIfChanged(dPrice, "--");
  if (have && haveCh && prev > 0) {
    char v[20]; const float diff = price - prev;
    formatPrice(fabsf(diff), v, sizeof v);
    snprintf(b, sizeof b, "%s%s  (%+.2f%%)", diff >= 0 ? "+" : "-", v, pct);
    setLabelIfChanged(dChange, b);
    lv_obj_set_style_text_color(dChange, lv_color_hex(pct >= 0 ? 0x35D879 : 0xFF5555), 0);
  } else setLabelIfChanged(dChange, "");
  lv_obj_align_to(dChange, dPrice, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -6);
}

static void detailFill() {
  const bool up = dData.count > 1 ? dData.series[dData.count - 1] >= dData.series[0] : true;
  drawDetailChart(up ? 0x35D879 : 0xFF5555);
  char b[40];
  // market state chip
  static const char *const kState[] = {"", "Aberto", "Fechado", "Pr\xC3\xA9-mercado", "P\xC3\xB3s-mercado", "24h"};
  static const uint32_t kStateCol[] = {0, 0x35D879, 0x9299A5, 0xFFD477, 0xFFD477, 0x38D6F5};
  const uint8_t ms = dData.ok && dData.market <= FIN_MKT_24H ? dData.market : FIN_MKT_UNKNOWN;
  setLabelIfChanged(dState, kState[ms]);
  lv_obj_set_style_text_color(dState, lv_color_hex(kStateCol[ms]), 0);
  lv_obj_set_style_border_color(dState, lv_color_hex(kStateCol[ms]), 0);
  if (ms == FIN_MKT_UNKNOWN) lv_obj_add_flag(dState, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(dState, LV_OBJ_FLAG_HIDDEN);
  fmtTime(dData.ok ? dData.t0 : 0, dData.range, b, sizeof b); setLabelIfChanged(dT0, b);
  fmtTime(dData.ok ? dData.t1 : 0, dData.range, b, sizeof b); setLabelIfChanged(dT1, b);
  // change over the period (vs. the close before it)
  const float ref = dData.range == 0 ? dPrev : dData.rangeRef;
  if (dData.ok && dData.count && isfinite(ref) && ref > 0) {
    const float pc = (dData.series[dData.count - 1] - ref) * 100.0f / ref;
    static const char *const kRangeName[FIN_RANGES] = {"hoje", "5 dias", "1 m\xC3\xAAs", "6 meses", "1 ano"};
    snprintf(b, sizeof b, "%s: %+.2f%%", kRangeName[dData.range], pc);
    setLabelIfChanged(dPeriod, b);
    lv_obj_set_style_text_color(dPeriod, lv_color_hex(pc >= 0 ? 0x35D879 : 0xFF5555), 0);
  } else setLabelIfChanged(dPeriod, "");
  const float vals[8] = {dPrev, dData.open, dData.dayHigh, dData.dayLow,
                         dData.ok ? dData.periodHigh : NAN, dData.ok ? dData.periodLow : NAN, dData.wk52High, dData.wk52Low};
  for (int k = 0; k < 8; ++k) {
    if (!isfinite(vals[k]) || vals[k] <= 0) snprintf(b, sizeof b, "-");
    else formatPrice(vals[k], b, sizeof b);
    setLabelIfChanged(dStatVal[k], b);
  }
  if (dData.ok) lv_obj_add_flag(dLoading, LV_OBJ_FLAG_HIDDEN);
  else { lv_label_set_text(dLoading, "sem dados para este per\xC3\xADodo"); lv_obj_remove_flag(dLoading, LV_OBJ_FLAG_HIDDEN); }
}

// 500 ms UI tick (from updateFinanceScreen) while the detail is open.
static void detailRender(bool financeChanged) {
  if (!detailOpen()) return;
  if (financeChanged) detailHeader();
  bool fresh = false, matches = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (financeDetail.version != dShownVersion) {
    matches = !strcmp(financeDetail.symbol, financeDetailWant.symbol) && financeDetail.range == financeDetailWant.range;
    if (matches) { dData = financeDetail; dShownVersion = financeDetail.version; fresh = true; }
  }
  const bool answered = !strcmp(financeDetail.symbol, financeDetailWant.symbol) && financeDetail.range == financeDetailWant.range &&
                        financeDetail.version == dShownVersion;
  xSemaphoreGive(mtx);
  if (fresh) detailFill();
  // answer still for an older tab (dedup dropped our post while a job ran): ask again, at most every 2 s
  else if (!answered && millis() - dRequestedMs > 2000) { dRequestedMs = millis(); postScreenJob(CMD_FINANCE_DETAIL); }
}

static void openDetail(int index) {
  if (index < 0 || index >= builtCount || !dPanel) return;
  dIndex = index; dRange = 0;
  lv_buttonmatrix_clear_button_ctrl_all(dTabs, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_buttonmatrix_set_button_ctrl(dTabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
  dData = FinanceDetail();
  dShownVersion = UINT32_MAX;
  lv_obj_add_flag(dState, LV_OBJ_FLAG_HIDDEN);
  setLabelIfChanged(dT0, ""); setLabelIfChanged(dT1, ""); setLabelIfChanged(dPeriod, "");
  drawDetailChart(0x35D879);
  for (int k = 0; k < 8; ++k) setLabelIfChanged(dStatVal[k], "-");
  detailHeader();
  lv_obj_remove_flag(dPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(dPanel);
  xSemaphoreTake(mtx, portMAX_DELAY); dShownVersion = financeDetail.version; xSemaphoreGive(mtx);   // ignore older answers
  requestDetail();
}

static void tabEvent(lv_event_t *e) {
  const uint32_t i = lv_buttonmatrix_get_selected_button((lv_obj_t *)lv_event_get_target(e));
  if (i >= FIN_RANGES || i == dRange) return;
  dRange = (uint8_t)i;
  requestDetail();
}

static void buildDetailPanel() {
  dPanel = lv_obj_create(financeView);
  lv_obj_set_size(dPanel, SCR_W, SCR_H); lv_obj_set_pos(dPanel, 0, 0);
  lv_obj_set_style_bg_color(dPanel, lv_color_hex(DETAIL_BG), 0); lv_obj_set_style_bg_opa(dPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dPanel, 0, 0); lv_obj_set_style_radius(dPanel, 0, 0); lv_obj_set_style_pad_all(dPanel, 0, 0);
  lv_obj_remove_flag(dPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(dPanel, LV_OBJ_FLAG_CLICKABLE);                       // swallow taps behind it

  // back: same size, place and glass look as the app's back button (main.cpp), but it
  // closes the detail and stays on Finance
  lv_obj_t *back = lv_button_create(dPanel);
  lv_obj_set_size(back, 40, 34); lv_obj_set_pos(back, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x0A1220), 0); lv_obj_set_style_bg_grad_dir(back, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_40, 0); lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x0A1220), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(back, lv_color_hex(0xFFFFFF), 0); lv_obj_set_style_border_opa(back, LV_OPA_30, 0);
  lv_obj_set_style_border_width(back, 1, 0); lv_obj_set_style_radius(back, 12, 0);
  lv_obj_set_style_shadow_width(back, 0, 0); lv_obj_set_style_pad_all(back, 0, 0);
  lv_obj_add_event_cb(back, [](lv_event_t *) { closeDetail(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *arrow = makeLabel(back, LV_SYMBOL_LEFT, &lv_font_weather_14, 0xFFFFFF);
  lv_obj_center(arrow);

  dName = makeLabel(dPanel, "", &lv_font_weather_20, 0xFFFFFF);
  lv_obj_set_pos(dName, 54, 4); lv_obj_set_width(dName, 300); lv_label_set_long_mode(dName, LV_LABEL_LONG_DOT);
  dSub = makeLabel(dPanel, "", &lv_font_weather_14, 0x8FA3C4); lv_obj_set_pos(dSub, 54, 28);
  dState = makeLabel(dPanel, "", &lv_font_weather_14, 0x9299A5);
  lv_obj_set_style_border_width(dState, 1, 0); lv_obj_set_style_radius(dState, 10, 0);
  lv_obj_set_style_pad_hor(dState, 8, 0); lv_obj_set_style_pad_ver(dState, 2, 0);
  lv_obj_align(dState, LV_ALIGN_TOP_RIGHT, -10, 10);
  dPrice = makeLabel(dPanel, "--", &lv_font_montserrat_36, 0xEEEEEE); lv_obj_set_pos(dPrice, 14, 48);
  dChange = makeLabel(dPanel, "", &lv_font_weather_20, 0x35D879);

  static const char *kTabs[] = {"1D", "5D", "1M", "6M", "1A", ""};
  dTabs = lv_buttonmatrix_create(dPanel);
  lv_buttonmatrix_set_map(dTabs, kTabs);
  lv_buttonmatrix_set_button_ctrl_all(dTabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(dTabs, true);
  lv_buttonmatrix_set_button_ctrl(dTabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_size(dTabs, DCH_W, 30); lv_obj_set_pos(dTabs, 14, 96);
  lv_obj_set_style_bg_opa(dTabs, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(dTabs, 0, 0);
  lv_obj_set_style_pad_all(dTabs, 0, 0); lv_obj_set_style_pad_column(dTabs, 6, 0);
  lv_obj_set_style_bg_color(dTabs, lv_color_hex(0x13203D), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(dTabs, lv_color_hex(0x1E6BFF), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(dTabs, 0, LV_PART_ITEMS); lv_obj_set_style_shadow_width(dTabs, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(dTabs, 8, LV_PART_ITEMS);
  lv_obj_set_style_text_font(dTabs, &lv_font_weather_14, LV_PART_ITEMS);
  lv_obj_set_style_text_color(dTabs, lv_color_hex(0xDCEBFF), LV_PART_ITEMS);
  lv_obj_add_event_cb(dTabs, tabEvent, LV_EVENT_VALUE_CHANGED, NULL);

  dChartBuf = (uint16_t *)psramMalloc(DCH_W * DCH_H * 2);
  dChartImg = lv_image_create(dPanel);
  lv_obj_set_pos(dChartImg, 14, 136); lv_obj_set_size(dChartImg, DCH_W, DCH_H);
  if (dChartBuf) {
    dChartDsc = {};
    dChartDsc.header.magic = LV_IMAGE_HEADER_MAGIC; dChartDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dChartDsc.header.w = DCH_W; dChartDsc.header.h = DCH_H; dChartDsc.header.stride = DCH_W * 2;
    dChartDsc.data_size = DCH_W * DCH_H * 2; dChartDsc.data = (const uint8_t *)dChartBuf;
    for (int k = 0; k < DCH_W * DCH_H; ++k) dChartBuf[k] = DETAIL_BG565;
    lv_image_set_src(dChartImg, &dChartDsc);
  }
  dBase = lv_line_create(dPanel);
  lv_obj_set_pos(dBase, 14, 136); lv_obj_set_size(dBase, DCH_W, DCH_H);
  lv_obj_set_style_line_width(dBase, 1, 0);
  lv_obj_set_style_line_dash_width(dBase, 5, 0); lv_obj_set_style_line_dash_gap(dBase, 4, 0);
  lv_obj_set_style_line_color(dBase, lv_color_hex(0x8A909C), 0); lv_obj_set_style_line_opa(dBase, LV_OPA_70, 0);
  lv_obj_add_flag(dBase, LV_OBJ_FLAG_HIDDEN);
  dLine = lv_line_create(dPanel);
  lv_obj_set_pos(dLine, 14, 136); lv_obj_set_size(dLine, DCH_W, DCH_H);
  lv_obj_set_style_line_width(dLine, 2, 0); lv_obj_set_style_line_rounded(dLine, true, 0);
  dDot = lv_obj_create(dPanel);
  lv_obj_set_size(dDot, 9, 9); lv_obj_set_style_radius(dDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dDot, 0, 0); lv_obj_set_style_pad_all(dDot, 0, 0);
  lv_obj_remove_flag(dDot, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(dDot, LV_OBJ_FLAG_HIDDEN);
  dLoading = makeLabel(dPanel, "", &lv_font_weather_14, 0x8FA3C4);
  lv_obj_set_style_text_align(dLoading, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(dLoading, DCH_W); lv_obj_set_pos(dLoading, 14, 136 + DCH_H / 2 - 9);

  dT0 = makeLabel(dPanel, "", &lv_font_weather_14, 0x6F7F9C); lv_obj_set_pos(dT0, 14, 282);
  dT1 = makeLabel(dPanel, "", &lv_font_weather_14, 0x6F7F9C);
  lv_obj_set_width(dT1, 120); lv_obj_set_style_text_align(dT1, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(dT1, 14 + DCH_W - 120, 282);
  dPeriod = makeLabel(dPanel, "", &lv_font_weather_14, 0x35D879);
  lv_obj_set_width(dPeriod, 140); lv_obj_set_style_text_align(dPeriod, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(dPeriod, 14 + DCH_W / 2 - 70, 282);

  for (int k = 0; k < 8; ++k) {                                         // key figures column
    const int y = 98 + k * 26;
    lv_obj_t *cap = makeLabel(dPanel, kStatCaption[k], &lv_font_weather_14, 0x6F7F9C);
    lv_obj_set_pos(cap, 328, y);
    dStatVal[k] = makeLabel(dPanel, "-", &lv_font_weather_14, 0xEAF2FF);
    lv_obj_set_width(dStatVal[k], 146); lv_obj_set_style_text_align(dStatVal[k], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(dStatVal[k], 328, y);
  }
  lv_obj_add_flag(dPanel, LV_OBJ_FLAG_HIDDEN);
}

// ================================================================ showcase
// After 20 s without touching the panel, the strip moves one card every 8 s (only with
// 3+ assets, never while the detail is open). Any touch pauses it again.
static constexpr uint32_t SHOWCASE_IDLE_MS = 20000UL, SHOWCASE_STEP_MS = 8000UL;
static constexpr int CARD_PITCH = 210 + 16;                              // card width + strip pad_column
static uint32_t showcaseLastMs = 0;

static void showcaseTick() {
  if (detailOpen() || builtCount < 3) return;
  if (lv_display_get_inactive_time(NULL) < SHOWCASE_IDLE_MS) { showcaseLastMs = millis(); return; }
  if (millis() - showcaseLastMs < SHOWCASE_STEP_MS) return;
  showcaseLastMs = millis();
  const int maxX = builtCount * CARD_PITCH - 16 - SCR_W;
  if (maxX <= 0) return;
  const int cur = lv_obj_get_scroll_x(financeStrip);
  int next = (cur + CARD_PITCH / 2) / CARD_PITCH * CARD_PITCH + CARD_PITCH;
  if (cur >= maxX - 2) next = 0;                                        // end reached: back to the first card
  else if (next > maxX) next = maxX;
  lv_obj_scroll_to_x(financeStrip, next, LV_ANIM_ON);
}

void financeOnExit() {
  closeDetail();
  showcaseLastMs = millis();
}
