// Screen manager: one active screen at a time. It shows/hides the views, runs each
// screen's lifecycle and owns the single refresh timer of the active screen. Network
// jobs are tagged with the screen and its visit (epoch) so stale results are dropped.
#include <Arduino.h>
#include "app_screens.h"
#include "core/screen_manager.h"
#include "core/screen_registry.h"
#include "services/app_log.h"

// ---------------------------------------------------------------- registry
static const ScreenLifecycle kNoScreen = {SCREEN_NONE, "none", nullptr, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, 0, 0, nullptr, 0, nullptr};

// In ScreenID order; screenManagerBegin() checks it.
static const ScreenLifecycle *const kScreens[SCREEN_COUNT] = {
  &kNoScreen,
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
};

const ScreenLifecycle *screenLifecycle(ScreenID id) {
  return kScreens[id < SCREEN_COUNT ? id : SCREEN_NONE];
}

lv_obj_t *screenView(ScreenID id) {
  const ScreenLifecycle *s = screenLifecycle(id);
  return s->view ? *s->view : nullptr;
}

// ---------------------------------------------------------------- context state
// Written only by the UI thread, read by the network task.
static volatile ScreenID gActiveScreen = SCREEN_NONE;
static volatile uint32_t gScreenEpoch = 0;
// Dedup: a command already queued/in flight is not queued again.
static volatile bool gCmdPending[CMD_COUNT] = {};
// Result of the last job per screen (network task -> UI thread).
enum JobOutcome : uint8_t { JOB_NONE = 0, JOB_OK, JOB_FAILED };
static volatile uint8_t gJobOutcome[SCREEN_COUNT] = {};
// Job currently executed by the network task.
static NetJob gCurrentJob = {CMD_COUNT, SCREEN_NONE, 0};

ScreenID activeScreenId() { return gActiveScreen; }

bool screenJobStillCurrent() {
  if (gCurrentJob.screen == SCREEN_NONE) return true;
  return gCurrentJob.screen == gActiveScreen && gCurrentJob.epoch == gScreenEpoch;
}

void jobSetCurrent(const NetJob &job) { gCurrentJob = job; }
void jobClearCurrent() { gCurrentJob = {CMD_COUNT, SCREEN_NONE, 0}; }
void jobClearPending(uint8_t cmd) { if (cmd < CMD_COUNT) gCmdPending[cmd] = false; }
void jobReportOutcome(ScreenID screen, bool ok) {
  if (screen < SCREEN_COUNT) gJobOutcome[screen] = ok ? JOB_OK : JOB_FAILED;
}
NetJob jobForActiveScreen(uint8_t cmd) { return {cmd, gActiveScreen, gScreenEpoch}; }

void post(int cmd) {
  NetJob job = {(uint8_t)cmd, SCREEN_NONE, 0};
  xQueueSend(cmdQ, &job, 0);
}

// Queues an API job tagged with the active screen and its visit epoch.
// Never blocks the UI thread; duplicates of an in-flight command are dropped.
void postScreenJob(int cmd) {
  if (cmd < 0 || cmd >= CMD_COUNT || gActiveScreen == SCREEN_NONE) return;
  if (gCmdPending[cmd]) return;
  NetJob job = {(uint8_t)cmd, gActiveScreen, gScreenEpoch};
  gCmdPending[cmd] = true;
  if (xQueueSend(cmdQ, &job, 0) != pdTRUE) gCmdPending[cmd] = false;
}

// ---------------------------------------------------------------- refresh timer
static lv_timer_t *contextTimer = nullptr;   // the single API timer, owned by the active screen

static bool hasPeriodicUpdate(const ScreenLifecycle &s) { return s.requestUpdate || s.jobCount; }

static void requestUpdate(const ScreenLifecycle &s) {
  if (s.requestUpdate) s.requestUpdate();
  else if (s.jobCount) postScreenJob(s.jobs[0].cmd);
}

static void restartContextTimer(uint32_t period) {
  lv_timer_set_period(contextTimer, period);
  lv_timer_reset(contextTimer);
  lv_timer_resume(contextTimer);
}

// Fires once SCREEN_ENTER_DEBOUNCE_MS after entering a screen, then every
// updateIntervalMs while the user stays there.
static void contextTimerCb(lv_timer_t *timer) {
  const ScreenLifecycle &screen = *screenLifecycle(gActiveScreen);
  if (!hasPeriodicUpdate(screen)) { lv_timer_pause(timer); return; }
  requestUpdate(screen);
  if (screen.updateIntervalMs) lv_timer_set_period(timer, screen.updateIntervalMs);
  else lv_timer_pause(timer);
}

// After a job finishes, restart the countdown from its completion: normal
// interval on success, a shorter retry on failure.
static void applyJobOutcome() {
  const ScreenID id = gActiveScreen;
  const uint8_t outcome = gJobOutcome[id];
  if (outcome == JOB_NONE) return;
  gJobOutcome[id] = JOB_NONE;
  const ScreenLifecycle &screen = *screenLifecycle(id);
  if (!screen.updateIntervalMs || !contextTimer) return;
  uint32_t period = screen.updateIntervalMs;
  if (outcome == JOB_FAILED && screen.retryIntervalMs) period = min(period, screen.retryIntervalMs);
  restartContextTimer(period);
}

// ---------------------------------------------------------------- switching
void switchScreen(ScreenID id) {
  if (id == SCREEN_NONE || id >= SCREEN_COUNT) return;
  const ScreenID previous = gActiveScreen;
  if (previous == id) return;

  // ---- On-Exit: stop the previous screen's timer and invalidate its jobs.
  if (contextTimer) lv_timer_pause(contextTimer);
  const ScreenLifecycle &old = *screenLifecycle(previous);
  if (old.onExit) old.onExit();
  gScreenEpoch = gScreenEpoch + 1;  // queued/in-flight jobs of the old visit become stale
  gActiveScreen = id;
  for (int i = 0; i < SCREEN_COUNT; ++i) gJobOutcome[i] = JOB_NONE;

  // ---- Swap the visible container.
  for (const ScreenLifecycle *screen : kScreens) {
    if (!screen->view || !*screen->view) continue;
    if (screen->id == id) lv_obj_remove_flag(*screen->view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(*screen->view, LV_OBJ_FLAG_HIDDEN);
  }

  // ---- On-Enter: paint cached data now, request fresh data after the debounce.
  const ScreenLifecycle &next = *screenLifecycle(id);
  LogV.printf("[ctx] %s -> %s\n", old.name, next.name);
  if (next.onEnter) next.onEnter();
  if (next.render) next.render();
  if (hasPeriodicUpdate(next) && contextTimer) restartContextTimer(SCREEN_ENTER_DEBOUNCE_MS);
}

// 500 ms UI tick: renders ONLY the active screen.
static bool lastWifiUp = false;
static void uiTick(lv_timer_t *) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const bool up = sh.wifiUp;
  xSemaphoreGive(mtx);
  const ScreenLifecycle &screen = *screenLifecycle(gActiveScreen);
  // Wi-Fi just came back: refresh the active screen right away (after debounce).
  if (up && !lastWifiUp && hasPeriodicUpdate(screen) && contextTimer) restartContextTimer(SCREEN_ENTER_DEBOUNCE_MS);
  lastWifiUp = up;
  applyJobOutcome();
  if (screen.render) screen.render();
}

void screenManagerBegin(lv_obj_t *root) {
  for (int i = 0; i < SCREEN_COUNT; ++i)
    if (kScreens[i]->id != i) Log.printf("ERRO: kScreens[%d] é a tela %s (ordem errada)\n", i, kScreens[i]->name);
  for (const ScreenLifecycle *screen : kScreens) if (screen->build) screen->build(root);
  for (const ScreenLifecycle *screen : kScreens)
    if (screen->view && *screen->view) lv_obj_add_flag(*screen->view, LV_OBJ_FLAG_HIDDEN);
  contextTimer = lv_timer_create(contextTimerCb, SCREEN_ENTER_DEBOUNCE_MS, NULL);
  lv_timer_pause(contextTimer);
  lv_timer_create(uiTick, 500, NULL);
  switchScreen(SCREEN_MENU);
}
