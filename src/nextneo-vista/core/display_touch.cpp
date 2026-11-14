// Display + touch driver glue: AXS15231B QSPI panel (portrait) shown landscape through
// LVGL, fast framebuffer copy, async panel send on core 0 and the I2C touch reader.
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "app_screens.h"
#include "core/display_touch.h"
#include "core/screen_manager.h"
#include "services/app_log.h"

// Guition JC3248W535C: AXS15231B 320x480 QSPI LCD, shown landscape (480x320).
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x3B
#define PANEL_W 320
#define PANEL_H 480

Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, PANEL_W, PANEL_H);
// Keep the driver's native portrait geometry. Rotate the canvas instead of
// rotating the panel: AXS15231B full-frame QSPI writes can go blank when the
// panel is rotated independently from a landscape canvas.
Arduino_Canvas *gfx = new Arduino_Canvas(PANEL_W, PANEL_H, panel, 0, 0, 0);

// ---------------------------------------------------------------- display + touch glue
// Multi-touch (pinch on the Airport screen): number of fingers and the 2nd finger
// in landscape coordinates. LVGL itself only gets the first finger.
volatile uint8_t gTouchCount = 0;
volatile int16_t gTouch2X = 0, gTouch2Y = 0;

static bool readTouch(int &x, int &y) {
  // read 14 bytes: header + 2 points of 6 bytes (AXS15231B report)
  // If the controller refuses the longer read, fall back to the original 8-byte read
  // (single touch) so touch never stops working.
  static uint8_t len = 14;
  uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, len};
  uint8_t b[14] = {};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 8);
  if (Wire.endTransmission() != 0) { gTouchCount = 0; return false; }
  if (Wire.requestFrom(TOUCH_ADDR, (int)len) != len) {
    if (len == 14) { len = 8; Log.println("Touch: leitura de 2 dedos indisponível, usando 1 dedo"); }
    gTouchCount = 0; return false;
  }
  for (int i = 0; i < len; i++) b[i] = Wire.read();
  if (b[1] == 0 || b[1] > 4) { gTouchCount = 0; return false; }
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  gTouchCount = b[1];
  if (b[1] >= 2 && len == 14) {
    const int x2 = ((b[8] & 0x0F) << 8) | b[9], y2 = ((b[10] & 0x0F) << 8) | b[11];
    gTouch2X = constrain(y2, 0, SCR_W - 1);                     // same rotation as touch_cb
    gTouch2Y = constrain(PANEL_W - 1 - x2, 0, SCR_H - 1);
  }
  return true;
}

// ---------------------------------------------------------------- fast flush
// The canvas is rotated in software. Arduino_Canvas::draw16bitRGBBitmap only has a
// fast path for rotation 0, otherwise it writes pixel by pixel through virtual
// calls. We copy LVGL's landscape area straight into the portrait framebuffer
// instead, column by column, so PSRAM writes are contiguous.
// The mapping is detected once at boot (fbMapping); unknown -> old slow path.
static int fbMapping = -1;          // -1 unknown, 1: idx = x*W + (W-1-y), 3: idx = (H-1-x)*W + y
static uint32_t perfFrames = 0, perfFlushUs = 0, perfCopyUs = 0, perfFrameUs = 0, perfLastLog = 0, perfRefrStart = 0;
static uint32_t qspiHz = 40000000;
static const char *perfScreenName() { return screenLifecycle(activeScreenId())->name; }

// whole refresh (render + copy + send), measured from LVGL display events
static void perfRefrEvent(lv_event_t *e) {
  const lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_REFR_START) perfRefrStart = 0;              // never span two refresh cycles
  else if (c == LV_EVENT_RENDER_START) perfRefrStart = micros();
  else if (perfRefrStart) { perfFrameUs += micros() - perfRefrStart; perfRefrStart = 0; }
}

// ---------------------------------------------------------------- async panel send
// Sending the 300 KB frame to the AXS15231B takes ~40 ms of CPU (byte swap + QSPI
// chunks) no matter the bus clock. A task on core 0 does it from a second buffer,
// so the LVGL thread only pays a PSRAM memcpy (~3-5 ms) and keeps rendering.
static uint16_t *sendBuf = nullptr;
static TaskHandle_t sendTaskHandle = nullptr;
static SemaphoreHandle_t sendIdle = nullptr;
static volatile uint32_t perfPanelUs = 0, perfPanelN = 0;

static void panelSendTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t t = micros();
    panel->draw16bitRGBBitmap(0, 0, sendBuf, PANEL_W, PANEL_H);
    perfPanelUs += micros() - t; perfPanelN = perfPanelN + 1;
    xSemaphoreGive(sendIdle);
  }
}

static void startAsyncSend() {
  sendBuf = (uint16_t *)heap_caps_malloc(PANEL_W * PANEL_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  sendIdle = xSemaphoreCreateBinary();
  if (!sendBuf || !sendIdle) { Log.println("Display: envio assíncrono indisponível (memória)"); sendBuf = nullptr; return; }
  xSemaphoreGive(sendIdle);
  xTaskCreatePinnedToCore(panelSendTask, "panel", 4096, NULL, 2, &sendTaskHandle, 0);
  Log.println("Display: envio assíncrono ativo (core 0)");
}

static void sendFrame() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!sendBuf || !fb) { gfx->flush(); return; }
  xSemaphoreTake(sendIdle, portMAX_DELAY);       // previous frame still going out? wait for it
  memcpy(sendBuf, fb, PANEL_W * PANEL_H * 2);
  xTaskNotifyGive(sendTaskHandle);
}

static void detectFramebufferMapping() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  const int W = PANEL_W, H = PANEL_H;
  const int tx[2] = {5, 401}, ty[2] = {7, 250};
  const uint16_t tc[2] = {0x1234, 0x4321};
  for (int i = 0; i < 2; ++i) gfx->drawPixel(tx[i], ty[i], tc[i]);
  bool m1 = true, m3 = true;
  for (int i = 0; i < 2; ++i) {
    m1 &= fb[tx[i] * W + (W - 1 - ty[i])] == tc[i];
    m3 &= fb[(H - 1 - tx[i]) * W + ty[i]] == tc[i];
  }
  fbMapping = m1 ? 1 : m3 ? 3 : -1;
  for (int i = 0; i < 2; ++i) gfx->drawPixel(tx[i], ty[i], 0);
  Log.printf("Display: framebuffer mapping %d (%s)\n", fbMapping, fbMapping > 0 ? "fast flush" : "fallback");
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  const uint32_t t0 = micros();
  const int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  uint16_t *fb = gfx->getFramebuffer();
  const uint16_t *src = (const uint16_t *)px;
  if (fbMapping > 0 && fb) {
    const int W = PANEL_W, H = PANEL_H;
    for (int c = 0; c < w; ++c) {
      const int lx = a->x1 + c;
      if (fbMapping == 1) {                       // column lx -> fb row lx, y reversed
        uint16_t *dst = fb + lx * W + (W - 1 - a->y1);
        for (int r = 0; r < h; ++r) *(dst - r) = src[r * w + c];
      } else {                                    // column lx -> fb row (H-1-lx), y forward
        uint16_t *dst = fb + (H - 1 - lx) * W + a->y1;
        for (int r = 0; r < h; ++r) dst[r] = src[r * w + c];
      }
    }
  } else {
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px, w, h);
  }
  const uint32_t t1 = micros();
  perfCopyUs += t1 - t0;
  if (lv_display_flush_is_last(disp)) {
    sendFrame();
    perfFlushUs += micros() - t1;
    ++perfFrames;
    if (millis() - perfLastLog >= 10000) {        // one perf line every 10 s (only when drawing)
      const uint32_t el = millis() - perfLastLog;
      const uint32_t frame = perfFrameUs / perfFrames, copy = perfCopyUs / perfFrames, send = perfFlushUs / perfFrames;
      const uint32_t render = frame > copy + send ? frame - copy - send : 0;
      const uint32_t panelMs = perfPanelN ? perfPanelUs / perfPanelN / 1000 : 0;
      LogV.printf("[perf] %s: %lu fps | quadro %lu ms = desenho %lu + copia %lu + envio %lu ms | tela (core 0) %lu ms | QSPI %lu MHz | livre %u\n",
                 perfScreenName(), (unsigned long)(perfFrames * 1000 / (el ? el : 1)), (unsigned long)(frame / 1000),
                 (unsigned long)(render / 1000), (unsigned long)(copy / 1000), (unsigned long)(send / 1000),
                 (unsigned long)panelMs, (unsigned long)(qspiHz / 1000000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      perfFrames = perfFlushUs = perfCopyUs = perfFrameUs = 0; perfPanelUs = 0; perfPanelN = 0; perfLastLog = millis();
    }
  }
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  int rawX, rawY;
  if (readTouch(rawX, rawY)) {
    // The touch controller reports native portrait coordinates. Rotate them
    // into the landscape coordinates used by the LCD and LVGL.
    data->point.x = constrain(rawY, 0, SCR_W - 1);
    data->point.y = constrain(PANEL_W - 1 - rawX, 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

uint32_t displayQspiHz() { return qspiHz; }

void displayBegin() {
  gfx->setRotation(1); // Landscape UI in the canvas; keep AXS15231B native portrait mode.
  // QSPI clock is selectable on the web page (40/60/80 MHz); default 40 MHz.
  qspiHz = prefs.getUInt("qspiHz", 40000000);
  if (qspiHz != 40000000 && qspiHz != 60000000 && qspiHz != 80000000) qspiHz = 40000000;
  if (!gfx->begin(qspiHz)) {
    Log.println("gfx->begin FAILED (display or framebuffer allocation)");
  }
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);

  detectFramebufferMapping();
  startAsyncSend();
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
  const size_t buf_bytes = SCR_W * 40 * sizeof(uint16_t);
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(disp, perfRefrEvent, LV_EVENT_RENDER_READY, NULL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);
}
