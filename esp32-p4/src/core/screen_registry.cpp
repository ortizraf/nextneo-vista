// Telas desta placa (P4). O screen_manager compartilhado indexa por id; telas de ScreenID
// que ainda não existem aqui ficam vazias (switchScreen as ignora).
#include "core/screen_registry.h"

const ScreenLifecycle *const kScreenTable[] = {
  &kHomeScreen,   // ocupa SCREEN_MENU até o menu ser portado
};
const uint8_t kScreenTableCount = sizeof(kScreenTable) / sizeof(kScreenTable[0]);
