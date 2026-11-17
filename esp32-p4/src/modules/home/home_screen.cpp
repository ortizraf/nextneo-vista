// Tela inicial do esqueleto da P4: placa, versão do firmware, Wi-Fi e IP. É o primeiro
// teste quando a placa chegar (tela + LVGL + Wi-Fi via ESP32-C6). Ocupa SCREEN_MENU até
// o menu da S3 ser portado.
#include <WiFi.h>
#include "app_screens.h"
#include "core/build_info.h"
#include "core/screen_registry.h"

static lv_obj_t *homeView, *wifiLabel, *ipLabel, *wifiDot;

static void buildHome(lv_obj_t *root) {
  homeView = lv_obj_create(root);
  lv_obj_set_size(homeView, SCR_W, SCR_H);
  lv_obj_set_style_bg_opa(homeView, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(homeView, 0, 0);
  lv_obj_set_flex_flow(homeView, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(homeView, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(homeView, dp(10), 0);

  makeLabel(homeView, "NextNeo Vista", &lv_font_montserrat_48, 0xEDF4FF);
  makeLabel(homeView, kBoard.name, &lv_font_montserrat_28, 0x9FE3FF);
  makeLabel(homeView, (String(kBoard.width) + "x" + kBoard.height + " · " + kBoard.dpi + " dpi").c_str(),
            &lv_font_montserrat_24, 0xB4C7DF);
  makeLabel(homeView, (String(T("Versão ", "Version ")) + kBuildVersion).c_str(), &lv_font_montserrat_24, 0xB4C7DF);

  lv_obj_t *card = makeCard(homeView, SCR_W - dp(40), dp(80));
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(card, dp(10), 0);
  wifiDot = makeLabel(card, LV_SYMBOL_WIFI, &lv_font_montserrat_28, 0xF07878);
  wifiLabel = makeLabel(card, "", &lv_font_montserrat_24, 0xEDF4FF);
  ipLabel = makeLabel(homeView, "", &lv_font_montserrat_24, 0x9FB6D0);
}

static void renderHome() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const String status = sh.status;
  const bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  setLabelIfChanged(wifiLabel, status.c_str());
  renderWifiIndicator(wifiDot, up);
  setLabelIfChanged(ipLabel, up ? (String("http://") + WiFi.localIP().toString()).c_str() : "");
}

const ScreenLifecycle kHomeScreen = {
  SCREEN_MENU, "home", &homeView, buildHome, nullptr, nullptr, renderHome, nullptr,
  0, 0, nullptr, 0, nullptr,
};
