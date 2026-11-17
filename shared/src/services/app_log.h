#pragma once
// In-memory application log (PSRAM ring buffer) shown on the web page (/log).
//
// Captures two sources:
//  - our own messages, written with Log.printf()/Log.println() (also echoed to Serial);
//  - Arduino/ESP-IDF core messages that go through ets_printf, e.g.
//    "[E][ssl_client.cpp:41] _handle_error(): ...".
// Each line gets a timestamp and a level (info / aviso / erro) guessed from
// its text, so the page can colour errors red and warnings yellow.
#include <Arduino.h>

enum class LogLevel : uint8_t { INFO, WARN, ERROR, VERBOSE };

struct LogLine {
  uint32_t ms;        // millis() when the line was completed
  LogLevel level;
  char text[171];
};

class AppLog : public Print {
 public:
  explicit AppLog(bool verbose = false) : verbose_(verbose) {}
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buffer, size_t size) override;
 private:
  bool verbose_;
};
extern AppLog Log;    // always recorded (info / aviso / erro)
extern AppLog LogV;   // routine detail (cycles, timings, fps): recorded only when verbose is on
extern volatile bool gLogVerbose;   // toggled on the web page (/log), saved in prefs
extern volatile bool gLogSerialEcho;   // false: record only (on-device tests keep the serial for Unity)

void appLogBegin();     // allocate the buffer and hook ets_printf; call early in setup()
// Copies up to `max` lines newer than `sinceMs` (oldest first). Returns the count.
size_t appLogSnapshot(LogLine *out, size_t max, uint32_t sinceMs);
static constexpr size_t APP_LOG_CAPACITY = 500;
