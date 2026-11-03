#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

LV_FONT_DECLARE(lv_font_weather_14);
LV_FONT_DECLARE(lv_font_weather_20);

#define GFX_BL 1
#define SCR_W 480
#define SCR_H 320

struct Shared {
  String status = "Not connected";
  String ssidList;
  uint32_t scanVersion = 0;
  bool wifiUp = false;
  bool haveWeather = false;
  String cityName = "São Paulo";
  float temp = 0;
  int code = 0;
  int hourCount = 0;
  char hourLabel[8][6] = {{0}};
  float hourTemp[8] = {0};
  int hourCode[8] = {0};
  int hourRain[8] = {0};
  int humidity = 0;
  float wind = 0;
  char dayName[7][6] = {{0}};
  float tmax[7] = {0}, tmin[7] = {0};
  int dcode[7] = {0};
  float finance[3] = {0, 0, 0};
  float previousFinance[3] = {0,0,0}, financeChange[3] = {0,0,0};
  bool haveChange[3] = {false,false,false};
  float history[3][24] = {};
  uint8_t historyCount[3] = {0,0,0};
  bool haveFinance[3] = {false, false, false};
  bool financeRefreshing = false;
  uint8_t financeSuccessCount = 0;
  bool financeApiKnown[3] = {false, false, false};
  bool financeApiOk[3] = {false, false, false};
  uint32_t financeLastSuccessMs = 0;
  uint32_t financeVersion = 0;
  uint32_t financeQuoteEpoch[3] = {0,0,0};
  String pendingSsid, pendingPass, pendingCity;
};

struct Config {
  float lat = -23.5505, lon = -46.6333;
  int utcOffset = -10800;
  bool h24 = true;
  int bright = 100;
};

extern Shared sh;
extern Config cfg;
extern SemaphoreHandle_t mtx;
extern Preferences prefs;
extern QueueHandle_t cmdQ;

enum Cmd { CMD_SCAN, CMD_CONNECT, CMD_GEOCODE, CMD_WEATHER, CMD_FINANCE, CMD_FINANCE_USD, CMD_FINANCE_EUR, CMD_FINANCE_BTC };

extern lv_obj_t *mainMenu, *weatherView, *settingsView, *weatherTabs, *financeView, *calendarView;
extern lv_obj_t *lblClock, *lblDate, *lblCity, *lblTemp, *lblCond, *lblStatus, *lblWifiDot;
extern lv_obj_t *lblRange, *lblHumidity, *lblWind, *weatherArt, *homeTab;
extern lv_obj_t *fcDay[7], *fcCond[7], *fcTemp[7];
extern lv_obj_t *hourTime[8], *hourCond[8], *hourTemp[8], *hourRain[8];
extern lv_obj_t *hourArt[8], *dayArt[7];
extern lv_obj_t *ddSsid, *taPass, *taCity, *kb, *setStatusLbl, *wifiInfoLbl, *swH24;
extern lv_obj_t *financeStrip, *financeStatusLabel, *financeAgeLabel[3], *financeValue[3];
extern lv_obj_t *financeDelta[3], *financePlot[3], *financeLine[3], *financeHeader[3], *financeCard[3];
extern lv_point_precise_t financeLinePoints[3][24];
extern lv_obj_t *calendarTitle, *calendarGrid;
extern int calendarOffset;
extern lv_point_t calTouchStart;

lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);
lv_obj_t *makeCard(lv_obj_t *parent, int w, int h);
lv_obj_t *addBackButton(lv_obj_t *parent);
void showView(lv_obj_t *view);
void post(int cmd);
const char *weatherText(int code);
bool isRainCode(int code);

void buildFinanceScreen(lv_obj_t *screen);
void buildCalendarScreen(lv_obj_t *screen);
void buildWeatherScreen(lv_obj_t *screen);
void buildConfigScreen(lv_obj_t *screen);
void buildConfigKeyboard();
void drawCalendar();
void updateFinanceScreen();
void updateWeatherScreen();
