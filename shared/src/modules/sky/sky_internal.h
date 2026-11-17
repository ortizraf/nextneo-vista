#pragma once
// Sky module internals shared by the network side (sky_fetch.cpp) and the screen.
#include "modules/sky/sky_orbit.h"

namespace sky {

// Network task -> UI hand-over of a freshly downloaded list (PSRAM, MAX_SATS entries).
extern Sat *volatile satPending;
extern volatile int satPendingN;
extern volatile uint32_t satFetchedMs;
extern volatile bool satEverFetched, satFetching;

}  // namespace sky

bool skyFetch();          // network task: CelesTrak TLEs, at most every 6 h
