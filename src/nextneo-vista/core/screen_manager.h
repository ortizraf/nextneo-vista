#pragma once
// Screen manager internals used by the boot code and the network task.
// Screens only need app_screens.h (switchScreen, postScreenJob, ...).
#include "app_screens.h"

void screenManagerBegin(lv_obj_t *root);   // build every screen, start the timers, show the menu

// ---- network task side of the job contract (core/net_worker.cpp)
void jobSetCurrent(const NetJob &job);     // job the network task is running now
void jobClearCurrent();
void jobClearPending(uint8_t cmd);         // the command may be queued again
void jobReportOutcome(ScreenID screen, bool ok);   // restarts that screen's refresh timer
NetJob jobForActiveScreen(uint8_t cmd);    // {cmd, active screen, current visit}
