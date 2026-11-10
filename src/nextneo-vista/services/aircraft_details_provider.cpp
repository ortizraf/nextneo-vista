#include "app_log.h"
#include "aircraft_details_provider.h"
#include "aircraft_data_provider.h"
#include "https_fetch.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <stdio.h>
#include <string.h>

namespace {
PsramBuffer body(48 * 1024);

void copyText(char *dst, size_t size, const char *src) {
  if (!src) { dst[0] = 0; return; }
  snprintf(dst, size, "%s", src);
}

// "AIRBUS" + "A320 214" -> "Airbus A320 214"
void buildModel(char *dst, size_t size, const char *manufacturer, const char *type) {
  char maker[24] = {};
  if (manufacturer && *manufacturer) {
    size_t n = 0;
    bool startWord = true;
    for (const char *p = manufacturer; *p && n < sizeof maker - 1; ++p) {
      char c = *p;
      if (c == ' ' || c == '-') { if (n && maker[n-1] != ' ') maker[n++] = ' '; startWord = true; continue; }
      if (!startWord && c >= 'A' && c <= 'Z') c += 'a' - 'A';
      maker[n++] = c;
      startWord = false;
      if (n > 14) break;  // "The Boeing Company" -> keep it short
    }
    while (n && maker[n-1] == ' ') --n;
    maker[n] = 0;
  }
  if (maker[0] && type && *type) snprintf(dst, size, "%s %s", maker, type);
  else if (type && *type) snprintf(dst, size, "%s", type);
  else snprintf(dst, size, "%s", maker);
}

void readAirport(JsonObjectConst airport, char *code, size_t codeSize, char *city, size_t citySize) {
  const char *iata = airport["iata_code"] | "";
  const char *icao = airport["icao_code"] | "";
  copyText(code, codeSize, *iata ? iata : icao);
  const char *municipality = airport["municipality"] | "";
  copyText(city, citySize, *municipality ? municipality : (airport["name"] | ""));
}

void readRoute(JsonObjectConst route, AircraftDetails &out) {
  if (route.isNull()) return;
  copyText(out.airline, sizeof out.airline, route["airline"]["name"] | "");
  readAirport(route["origin"], out.originCode, sizeof out.originCode, out.originCity, sizeof out.originCity);
  readAirport(route["destination"], out.destCode, sizeof out.destCode, out.destCity, sizeof out.destCity);
}

JsonDocument &detailsFilter() {
  static JsonDocument filter(SpiRamJsonAllocator::instance());
  if (filter.isNull()) {
    JsonObject aircraft = filter["response"]["aircraft"].to<JsonObject>();
    for (const char *f : {"type", "icao_type", "manufacturer", "registration", "registered_owner"})
      aircraft[f] = true;
    JsonObject route = filter["response"]["flightroute"].to<JsonObject>();
    route["airline"]["name"] = true;
    for (const char *end : {"origin", "destination"})
      for (const char *f : {"iata_code", "icao_code", "municipality", "name"}) route[end][f] = true;
  }
  return filter;
}

int getJson(WiFiClientSecure &client, const char *url, JsonDocument &doc) {
  const int status = httpsFetch(client, url, body, 8000, false);
  if (status != HTTP_CODE_OK) return status;
  if (deserializeJson(doc, body.c_str(), body.length(),
                      DeserializationOption::Filter(detailsFilter()))) return -100;
  return status;
}
}  // namespace

void fetchAircraftDetails(AircraftDetails &out) {
  if (WiFi.status() != WL_CONNECTED) { out.state = DetailsState::FAILED; return; }
  // Different host from the radar poll: drop the ADSB.lol keep-alive session
  // first so two TLS contexts never coexist in internal RAM.
  adsbLolReleaseConnection();
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  char url[128];
  JsonDocument doc(SpiRamJsonAllocator::instance());
  int status = HTTP_CODE_NOT_FOUND;
  if (out.icao24[0]) {
    if (out.callsign[0])
      snprintf(url, sizeof url, "https://api.adsbdb.com/v0/aircraft/%s?callsign=%s", out.icao24, out.callsign);
    else
      snprintf(url, sizeof url, "https://api.adsbdb.com/v0/aircraft/%s", out.icao24);
    status = getJson(client, url, doc);
  }
  if (status == HTTP_CODE_OK) {
    JsonObjectConst aircraft = doc["response"]["aircraft"];
    buildModel(out.model, sizeof out.model, aircraft["manufacturer"] | "", aircraft["type"] | "");
    copyText(out.owner, sizeof out.owner, aircraft["registered_owner"] | "");
    readRoute(doc["response"]["flightroute"], out);
  }
  // Aircraft unknown to the database: the route may still be known by callsign.
  if (status == HTTP_CODE_NOT_FOUND && out.callsign[0]) {
    doc.clear();
    snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", out.callsign);
    status = getJson(client, url, doc);
    if (status == HTTP_CODE_OK) readRoute(doc["response"]["flightroute"], out);
  }
  client.stop();
  if (status == HTTP_CODE_OK) out.state = DetailsState::READY;
  else if (status == HTTP_CODE_NOT_FOUND) out.state = DetailsState::NOT_FOUND;
  else out.state = DetailsState::FAILED;
  Log.printf("adsbdb: %s/%s -> %d (%s %s>%s)\n", out.icao24, out.callsign, status,
                out.model, out.originCode, out.destCode);
}
