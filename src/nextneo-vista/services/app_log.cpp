#include "app_log.h"
#include "psram_alloc.h"
#include <esp_rom_sys.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

AppLog Log;

namespace {
LogLine *lines = nullptr;          // ring buffer in PSRAM
size_t head = 0, count = 0;        // next slot / lines stored
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// One partial line per source (0 = Log, 1 = core ets_printf output).
struct Partial { char buf[sizeof(LogLine::text)]; size_t len; };
Partial partial[2] = {};

bool containsI(const char *hay, const char *needle) {
  const size_t n = strlen(needle);
  for (const char *p = hay; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) ++i;
    if (i == n) return true;
  }
  return false;
}

LogLevel classify(const char *t) {
  if (!strncmp(t, "[E]", 3) || strstr(t, "][E]") || containsI(t, "error") || containsI(t, "falh") ||
      containsI(t, "erro ") || containsI(t, "erro:") || containsI(t, "inválid") || containsI(t, "failed"))
    return LogLevel::ERROR;
  if (!strncmp(t, "[W]", 3) || strstr(t, "][W]") || containsI(t, "warn") || containsI(t, "tentativa") ||
      containsI(t, "aguardando retry") || containsI(t, "ausente") || containsI(t, "memória baixa") ||
      containsI(t, "sem conexão") || containsI(t, "offline") || containsI(t, "http 4") || containsI(t, "http 5"))
    return LogLevel::WARN;
  return LogLevel::INFO;
}

// Called inside the critical section.
void commit(Partial &p) {
  while (p.len && (p.buf[p.len - 1] == '\r' || p.buf[p.len - 1] == ' ')) --p.len;
  p.buf[p.len] = 0;
  if (p.len && lines) {
    LogLine &l = lines[head];
    l.ms = millis();
    memcpy(l.text, p.buf, p.len + 1);
    l.level = classify(l.text);
    head = (head + 1) % APP_LOG_CAPACITY;
    if (count < APP_LOG_CAPACITY) ++count;
  }
  p.len = 0;
}

void feed(int source, char c) {
  Partial &p = partial[source];
  if (c == '\n') { commit(p); return; }
  if (c == '\r') return;
  if (p.len < sizeof(p.buf) - 1) p.buf[p.len++] = c;
  else commit(p);   // very long line: split it
}

// ets_printf channel 1 receives every character the core prints. It may run
// in an ISR or while the flash cache is off, so it only copies the character
// into a small internal-RAM ring (IRAM code, DRAM data). Lines are assembled
// later, in task context (drainCore).
constexpr size_t RAW_SIZE = 2048;
DRAM_ATTR char raw[RAW_SIZE];
DRAM_ATTR volatile size_t rawHead = 0, rawTail = 0;
DRAM_ATTR portMUX_TYPE rawMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR etsPutc(char c) {
  portENTER_CRITICAL_SAFE(&rawMux);
  const size_t next = (rawHead + 1) % RAW_SIZE;
  if (next != rawTail) { raw[rawHead] = c; rawHead = next; }   // drop when full
  portEXIT_CRITICAL_SAFE(&rawMux);
}

// Task context, called with `mux` held.
void drainCore() {
  for (;;) {
    char c;
    portENTER_CRITICAL_SAFE(&rawMux);
    const bool empty = rawTail == rawHead;
    if (!empty) { c = raw[rawTail]; rawTail = (rawTail + 1) % RAW_SIZE; }
    portEXIT_CRITICAL_SAFE(&rawMux);
    if (empty) return;
    feed(1, c);
  }
}
}  // namespace

size_t AppLog::write(uint8_t c) { return write(&c, 1); }

size_t AppLog::write(const uint8_t *buffer, size_t size) {
  Serial.write(buffer, size);
  portENTER_CRITICAL_SAFE(&mux);
  drainCore();
  for (size_t i = 0; i < size; ++i) feed(0, (char)buffer[i]);
  portEXIT_CRITICAL_SAFE(&mux);
  return size;
}

void appLogBegin() {
  if (lines) return;
  lines = (LogLine *)psramCalloc(APP_LOG_CAPACITY, sizeof(LogLine));
  // With USB-CDC on boot, putc2 carries the core log to USB and putc1 (UART)
  // is unused, so the copy for the web log goes on channel 1.
  esp_rom_install_channel_putc(1, etsPutc);
}

size_t appLogSnapshot(LogLine *out, size_t max, uint32_t sinceMs) {
  if (!lines) return 0;
  size_t n = 0;
  portENTER_CRITICAL_SAFE(&mux);
  drainCore();
  const size_t start = (head + APP_LOG_CAPACITY - count) % APP_LOG_CAPACITY;
  for (size_t i = 0; i < count && n < max; ++i) {
    const LogLine &l = lines[(start + i) % APP_LOG_CAPACITY];
    if ((int32_t)(l.ms - sinceMs) >= 0) out[n++] = l;
  }
  portEXIT_CRITICAL_SAFE(&mux);
  return n;
}
