#include "modules/servers/servers_retry.h"

uint8_t ServerRetry::plan(uint32_t nowMs, uint8_t *order) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < count_; ++i) if (!failed_[i]) order[n++] = i;
  for (uint8_t i = 0; i < count_; ++i)                       // unsigned math: millis() wrap safe
    if (failed_[i] && (uint32_t)(nowMs - failedAtMs_[i]) >= retryMs_) order[n++] = i;
  return n;
}

void ServerRetry::record(uint8_t i, bool ok, uint32_t nowMs) {
  if (i >= count_) return;
  failed_[i] = !ok;
  if (!ok) failedAtMs_[i] = nowMs;
}

void ServerRetry::reset(uint8_t i) {
  if (i < count_) failed_[i] = false;
}
