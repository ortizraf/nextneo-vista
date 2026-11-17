// Telas desta placa (ESP32-S3 3,5"). O screen_manager compartilhado indexa por id;
// a ordem aqui não importa. Tela nova: declare em core/screen_registry.h e liste aqui.
#include "core/screen_registry.h"

const ScreenLifecycle *const kScreenTable[] = {
  &kMenuScreen,
  &kWeatherScreen,
  &kFinanceScreen,
  &kCalendarScreen,
  &kFlightRadarScreen,
  &kSettingsScreen,
  &kServersScreen,
  &kClockScreen,
  &kAirportScreen,
  &kPhotosScreen,
  &kSkyScreen,
  &kF1Screen,
  &kArtScreen,
  &kFootballScreen,
  &kGardenScreen,
};
const uint8_t kScreenTableCount = sizeof(kScreenTable) / sizeof(kScreenTable[0]);
