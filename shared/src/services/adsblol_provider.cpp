#include "core/i18n.h"
#include "services/app_log.h"
#include "services/aircraft_data_provider.h"
#include "services/https_fetch.h"
#include "modules/flight_radar/flight_parser.h"
#include "modules/flight_radar/flight_filter.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <stdio.h>

namespace {
// A 50 km radius over GRU/CGH can return 100+ aircraft (~1 KB of JSON each).
// The body lives in PSRAM, so this limit costs no internal RAM.
constexpr size_t MAX_RESPONSE_BYTES = 384 * 1024;

// Keep the TLS session alive between radar polls: one handshake per visit to
// the radar instead of one every 12 s.
WiFiClientSecure secureClient;
bool secureClientConfigured = false;
bool connectionOpen = false;
const char *openHost = nullptr;   // host the keep-alive session belongs to (one TLS context at a time)
PsramBuffer body(MAX_RESPONSE_BYTES);

JsonDocument &aircraftFilter() {
  static JsonDocument filter(SpiRamJsonAllocator::instance());
  if (filter.isNull()) {
    const char *fields[] = {"hex", "flight", "t", "r", "category", "squawk", "emergency",
                            "lat", "lon", "alt_baro", "gs", "track", "true_heading",
                            "baro_rate", "geom_rate", "seen_pos"};
    for (const char *key : {"ac", "aircraft"})
      for (const char *field : fields) filter[key][0][field] = true;
    filter["msg"] = true;
  }
  return filter;
}

// Same code for ADSB.lol and adsb.fi: both serve readsb-style JSON ("ac" / "aircraft").
class ReadsbProvider final : public AircraftDataProvider {
 public:
  ReadsbProvider(const char *name, const char *host, const char *urlFormat)
      : name_(name), host_(host), urlFormat_(urlFormat) {}
  bool fetch(double centerLat, double centerLon, float radiusNm,
             Aircraft *aircraft, uint8_t capacity, uint8_t &count,
             char *error, size_t errorCapacity,
             bool (*shouldContinue)()) override {
    count = 0;
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(error, errorCapacity, T("Sem conexão Wi-Fi", "No Wi-Fi connection"));
      return false;
    }
    char url[128];
    const unsigned int queryRadiusNm = (unsigned int)ceilf(radiusNm);
    snprintf(url, sizeof url, urlFormat_, centerLat, centerLon, queryRadiusNm);
    if (!secureClientConfigured) {
      secureClient.setInsecure();  // public read-only data, no secrets sent
      secureClient.setHandshakeTimeout(12);
      secureClientConfigured = true;
    }
    // Server closed the keep-alive socket: free the stale mbedTLS context
    // before reconnecting so its internal-RAM buffers are released first.
    if (!secureClient.connected() || openHost != host_) { secureClient.stop(); connectionOpen = false; }
    openHost = host_;
    const uint32_t started = millis();
    connectionOpen = true;
    const int status = httpsFetch(secureClient, url, body, 9000, true);
    if (status != HTTP_CODE_OK) {
      if (status == HTTPC_ERROR_TOO_LESS_RAM) snprintf(error, errorCapacity, T("Memória baixa p/ TLS", "Low memory for TLS"));
      else if (status < 0) snprintf(error, errorCapacity, "%s", HTTPClient::errorToString(status).c_str());
      else snprintf(error, errorCapacity, "HTTP %d", status);
      Log.printf("%s: %s\n", name_, error);
      secureClient.stop();
      connectionOpen = false;
      return false;
    }
    const uint32_t downloaded = millis();
    if (shouldContinue && !shouldContinue()) {
      snprintf(error, errorCapacity, T("Cancelado", "Cancelled"));
      return false;
    }
    JsonDocument doc(SpiRamJsonAllocator::instance());
    DeserializationError jsonError = deserializeJson(doc, body.c_str(), body.length(),
        DeserializationOption::Filter(aircraftFilter()), DeserializationOption::NestingLimit(8));
    if (jsonError) {
      snprintf(error, errorCapacity, T("JSON inválido: %s", "invalid JSON: %s"), jsonError.c_str());
      return false;
    }
    // ADSB.lol's v2 response uses "ac" (like readsb); accept "aircraft" too.
    JsonArrayConst list = doc["ac"].as<JsonArrayConst>();
    if (list.isNull()) list = doc["aircraft"].as<JsonArrayConst>();
    if (list.isNull()) {
      const char *message = doc["msg"] | T("Resposta sem lista ac", "Answer without an ac list");
      snprintf(error, errorCapacity, "API: %.36s", message);
      return false;
    }
    // Parse, filter (radius / stale positions) and keep the nearest `capacity`
    // aircraft in one pass, sorted by distance.
    unsigned total = 0;
    Aircraft a;
    for (JsonVariantConst item : list) {
      if (!item.is<JsonObjectConst>()) continue;
      if (!parseAdsbLolAircraftObject(item.as<JsonObjectConst>(), a)) continue;
      ++total;
      if (a.hasPositionAge && a.positionAgeSeconds > 120.0f) continue;
      a.distanceNm = aircraftDistanceNm(centerLat, centerLon, a.latitude, a.longitude);
      if (a.distanceNm > radiusNm) continue;
      count = insertNearest(aircraft, count, capacity, a);
    }
    Log.printf("%s: %u bytes, %u aeronaves (%u na área) em %lums + parse %lums\n",
                  name_, (unsigned)body.length(), total, count,
                  (unsigned long)(downloaded - started), (unsigned long)(millis() - downloaded));
    error[0] = '\0';
    return true;
  }

 private:
  const char *name_, *host_, *urlFormat_;
};
}  // namespace

void adsbLolReleaseConnection() {
  if (!connectionOpen) return;
  secureClient.stop();
  connectionOpen = false;
  Log.printf("%s: conexão TLS liberada\n", openHost ? openHost : "radar");
}

AircraftDataProvider &adsbLolProvider() {
  static ReadsbProvider provider("ADSB.lol", "api.adsb.lol", "https://api.adsb.lol/v2/point/%.5f/%.5f/%u");
  return provider;
}

AircraftDataProvider &adsbFiProvider() {
  static ReadsbProvider provider("adsb.fi", "opendata.adsb.fi", "https://opendata.adsb.fi/api/v2/lat/%.5f/lon/%.5f/dist/%u");
  return provider;
}
