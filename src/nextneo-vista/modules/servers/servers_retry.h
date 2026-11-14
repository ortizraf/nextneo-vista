#pragma once
// Order of the Servers checks in one cycle. One offline hub must never hold the
// others up: healthy servers go first, a failed one is retried only every retryMs,
// and after the healthy ones. Pure C++ (no Arduino): unit-tested on the computer.
#include <stdint.h>

class ServerRetry {
 public:
  static constexpr uint8_t kMax = 8;
  ServerRetry(uint8_t count, uint32_t retryMs) : count_(count < kMax ? count : kMax), retryMs_(retryMs) {}

  // Servers to check now, in order (healthy first). Returns how many were written.
  uint8_t plan(uint32_t nowMs, uint8_t *order) const;
  void record(uint8_t i, bool ok, uint32_t nowMs);   // result of a check
  void reset(uint8_t i);                             // config changed: check it at once
  bool failed(uint8_t i) const { return i < count_ && failed_[i]; }

 private:
  uint8_t count_;
  uint32_t retryMs_;
  bool failed_[kMax] = {};
  uint32_t failedAtMs_[kMax] = {};
};
