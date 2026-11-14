// Sky network side (network task only): CelesTrak TLE lists, at most every 6 h.
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include "app_screens.h"
#include "modules/sky/sky_internal.h"
#include "services/app_log.h"
#include "services/https_fetch.h"
#include "services/psram_alloc.h"

namespace sky {
Sat *volatile satPending = nullptr; volatile int satPendingN = 0;   // handed over by the network task
volatile uint32_t satFetchedMs = 0; volatile bool satEverFetched = false, satFetching = false;
}  // namespace sky
using namespace sky;

bool skyFetch() {
  if (satEverFetched && millis() - satFetchedMs < 6UL * 3600UL * 1000UL) return true;
  static const char *kUrl[3] = {"https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle",
                                "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle",
                                "https://celestrak.org/NORAD/elements/gp.php?GROUP=last-30-days&FORMAT=tle"};
  satFetching = true;
  Sat *list = (Sat *)heap_caps_malloc(sizeof(Sat) * MAX_SATS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!list) { satFetching = false; return false; }
  static PsramBuffer body(600 * 1024);
  int n = 0, okCount = 0;
  // The 3 lists come from the same host: one keep-alive session (one TLS handshake).
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(8);
  for (int k = 0; k < 3; ++k) {
    if (!screenJobStillCurrent()) break;
    if (!client.connected() && !tlsMemoryAvailable("celestrak")) continue;
    body.clear();
    const uint32_t t0 = millis();
    const int status = httpsFetch(client, kUrl[k], body, 15000, true);
    const int before = n;
    if (status == 200 && !body.overflowed()) { n = parseTle(body.c_str(), list, n, MAX_SATS, k); ++okCount; }
    Log.printf("C\xC3\xA9u: CelesTrak %s HTTP %d, %u bytes, +%d sat\xC3\xA9lites (%lums)\n",
               k == 0 ? "stations" : k == 1 ? "visual" : "last-30-days", status, (unsigned)body.length(), n - before,
               (unsigned long)(millis() - t0));
  }
  client.stop();
  satFetching = false;
  if (!okCount || !n) { heap_caps_free(list); return false; }
  while (satPending) delay(20);                           // UI has not taken the previous list yet
  satPendingN = n; satPending = list;
  satFetchedMs = millis(); satEverFetched = true;
  return true;
}
