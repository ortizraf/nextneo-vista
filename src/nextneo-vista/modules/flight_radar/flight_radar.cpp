// Flight radar screen (480x320 landscape).
//
//  +-------------------------------+--------------------+
//  | [<]                    [10km] |  08:32        [↻]  |
//  |        .-~~~~~~~~-.           |  CGH · Congonhas   |
//  |      /  map + rings \         |  12 aviões · 3 s   |
//  |     |   sweep  ✈ TAM3 |       | +----------------+ |
//  |      \     ✈        /         | | list / details | |
//  |        '-________-'           | |                | |
//  | [● AO VIVO]           [MAPA]  | +----------------+ |
//  +-------------------------------+--------------------+
//
// Threads: the network task writes `snapshot`, map buffers and the details
// cache under `mtx`; the UI thread copies what changed and renders. Big
// buffers are in PSRAM so the internal heap stays free for TLS.
#include "flight_radar.h"
#include "../../app_screens.h"
#include "../../services/aircraft_data_provider.h"
#include "../../services/aircraft_details_provider.h"
#include "../../services/radar_map_provider.h"
#include "../../services/psram_alloc.h"
#include "flight_projection.h"
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

extern "C" {
extern const lv_image_dsc_t radar_icon_jet;
extern const lv_image_dsc_t radar_icon_light;
extern const lv_image_dsc_t radar_icon_heli;
}

namespace {
// ---------------------------------------------------------------- layout
constexpr int SCOPE_CX = 160, SCOPE_CY = 160, SCOPE_R = 148;
constexpr int MAP_SIZE = SCOPE_R * 2;
constexpr int PANEL_X = 322, PANEL_W = 152;
constexpr int ICON_PX = 28;
constexpr uint8_t TAG_COUNT = 4;        // callsign tags drawn on the scope
constexpr uint8_t LIST_ROWS = 5;        // "nearest" rows in the side panel
constexpr uint8_t TRAIL_POINTS = 10;
constexpr uint8_t DETAILS_CACHE = 16;
constexpr bool SWEEP_ENABLED = true;
constexpr uint32_t SWEEP_PERIOD_MS = 60;  // ~16 fps, 4 deg per frame
constexpr uint32_t MAP_RETRY_MS = 60UL * 1000UL;

// ---------------------------------------------------------------- palette
constexpr uint32_t C_BG = 0x041012, C_SCOPE = 0x071D1E, C_RING = 0x2F8F80;
constexpr uint32_t C_ACCENT = 0x3EE6C8, C_TEXT = 0xE6FFF9, C_MUTED = 0x7FA8A0;
constexpr uint32_t C_PANEL = 0x0B1C1E, C_PANEL_BORDER = 0x1F4542;
constexpr uint32_t C_SELECT = 0xFFD54A, C_GROUND = 0xF2B66D, C_ALERT = 0xFF5A5A;

constexpr uint16_t hexTo565(uint32_t c) {
  return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

// ---------------------------------------------------------------- shared state (mtx)
FlightRadarSnapshot *snapshot = nullptr;       // network -> UI
Aircraft *freshAircraft = nullptr;             // network scratch
AircraftDetails *detailsCache = nullptr;       // DETAILS_CACHE entries
uint32_t detailsVersion = 0;
uint16_t *mapBuffer[2] = {nullptr, nullptr};   // PSRAM, MAP_SIZE^2 RGB565
volatile int8_t mapFront = -1;                 // buffer the UI should show
volatile uint32_t mapVersion = 0;
volatile int8_t mapShownByUi = -1;             // buffer the UI is showing now
volatile bool mapAreaValid = false;            // front buffer matches the current area
bool mapEnabled = true;
// map key the network task has loaded (network task only)
double mapLat = 0, mapLon = 0;
float mapRadius = 0;
uint32_t mapRetryAfterMs = 0;
volatile bool radarActive = false;
void (*requestRefresh)() = nullptr;
void (*requestDetails)() = nullptr;

// ---------------------------------------------------------------- UI state
FlightRadarSnapshot *uiCopy = nullptr;
struct Trail { char hex[7]; uint8_t n; float lat[TRAIL_POINTS], lon[TRAIL_POINTS]; uint32_t seen; };
Trail *trails = nullptr;                       // one per aircraft slot, PSRAM
uint32_t renderedVersion = UINT32_MAX, renderedDetailsVersion = UINT32_MAX;
uint32_t renderedMapVersion = UINT32_MAX;
uint32_t renderedAgeSeconds = UINT32_MAX;
char selectedHex[7] = {};
lv_image_dsc_t mapDsc[2];

lv_obj_t *mapImg, *scopeFill, *sweepArc[3], *sweepLine, *trailLine, *selRing;
lv_obj_t *planeImg[FLIGHT_RADAR_MAX_AIRCRAFT];
lv_obj_t *tagLabel[TAG_COUNT];
lv_obj_t *ringLabel[3];
lv_obj_t *rangePill, *rangeText, *statusPill, *statusDot, *statusText, *mapPill, *mapText, *attribution;
lv_obj_t *clockLabel, *airportLabel, *countLabel;
lv_obj_t *listView, *listTitle, *listEmpty;
lv_obj_t *listRow[LIST_ROWS], *listDot[LIST_ROWS], *listCall[LIST_ROWS], *listSub[LIST_ROWS];
lv_obj_t *detailView, *dTitle, *dAirline, *dRoute, *dCities, *dModel, *dFooter;
lv_obj_t *dValue[6];
lv_timer_t *sweepTimer = nullptr;
int sweepAngle = 0;
lv_point_precise_t sweepPoints[2];
lv_point_precise_t trailPoints[TRAIL_POINTS + 1];

void ensureState() {
  if (snapshot) return;
  snapshot = new (psramCalloc(1, sizeof(FlightRadarSnapshot))) FlightRadarSnapshot();
  uiCopy = new (psramCalloc(1, sizeof(FlightRadarSnapshot))) FlightRadarSnapshot();
  freshAircraft = (Aircraft *)psramCalloc(FLIGHT_RADAR_MAX_AIRCRAFT, sizeof(Aircraft));
  detailsCache = (AircraftDetails *)psramCalloc(DETAILS_CACHE, sizeof(AircraftDetails));
  for (uint8_t i = 0; i < DETAILS_CACHE; ++i) new (&detailsCache[i]) AircraftDetails();
  trails = (Trail *)psramCalloc(FLIGHT_RADAR_MAX_AIRCRAFT, sizeof(Trail));
}

// ---------------------------------------------------------------- helpers
const char *statusName(FlightRadarStatus status) {
  switch (status) {
    case FlightRadarStatus::LOADING: return "BUSCANDO";
    case FlightRadarStatus::ONLINE: return "AO VIVO";
    case FlightRadarStatus::STALE: return "ATRASADO";
    case FlightRadarStatus::OFFLINE: return "OFFLINE";
    case FlightRadarStatus::EMPTY: return "SEM VOOS";
  }
  return "--";
}

uint32_t statusColor(FlightRadarStatus status) {
  switch (status) {
    case FlightRadarStatus::ONLINE: return 0x35E08F;
    case FlightRadarStatus::EMPTY: return C_MUTED;
    case FlightRadarStatus::LOADING: return C_ACCENT;
    default: return C_GROUND;
  }
}

// FR24-like altitude colouring: easy to tell climbing traffic from overflights.
uint32_t altitudeColor(const Aircraft &a) {
  if (a.emergency) return C_ALERT;
  if (a.onGround) return C_GROUND;
  if (!a.hasAltitude) return 0xB8C7C4;
  if (a.altitudeFeet < 2000) return 0x4ADE80;
  if (a.altitudeFeet < 10000) return 0x6DE8FF;
  if (a.altitudeFeet < 25000) return 0x7AA2FF;
  return 0xC79BFF;
}

bool typeIn(const char *type, const char *const *list, size_t n) {
  for (size_t i = 0; i < n; ++i) if (!strncmp(type, list[i], strlen(list[i]))) return true;
  return false;
}

const lv_image_dsc_t *iconFor(const Aircraft &a) {
  static const char *const helis[] = {"R22", "R44", "R66", "EC", "AS3", "AS5", "B06", "B407", "B412",
                                      "S76", "S92", "A109", "A119", "A139", "H1", "H2"};
  static const char *const lights[] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "PA",
                                       "BE", "SR2", "TBM", "PC12", "P28", "DA4", "M20", "BL8"};
  if (!strcmp(a.category, "A7") || typeIn(a.type, helis, sizeof helis / sizeof *helis)) return &radar_icon_heli;
  // Type prefixes like "C1" also match big types (C130), so only trust them
  // when the transponder doesn't report a medium/heavy category.
  const bool bigCategory = a.category[0] == 'A' && a.category[1] >= '3' && a.category[1] <= '6';
  if (!strcmp(a.category, "A1") || (!bigCategory && typeIn(a.type, lights, sizeof lights / sizeof *lights)))
    return &radar_icon_light;
  return &radar_icon_jet;
}

uint32_t iconScale(const Aircraft &a) {
  static const char *const heavy[] = {"A30", "A31", "A33", "A34", "A35", "A38", "B74", "B76", "B77",
                                      "B78", "IL96", "MD11", "B2", "C5", "A124", "A400"};
  if (!strcmp(a.category, "A5") || typeIn(a.type, heavy, sizeof heavy / sizeof *heavy)) return 256;
  if (!strcmp(a.category, "A1") || !strcmp(a.category, "A7")) return 180;
  if (!strcmp(a.category, "A2")) return 196;
  return 216;
}

void formatAltitude(const Aircraft &a, char *out, size_t size) {
  if (a.onGround) snprintf(out, size, "solo");
  else if (!a.hasAltitude) snprintf(out, size, "--");
  else if (a.altitudeFeet >= 10000) snprintf(out, size, "FL%03d", (int)lroundf(a.altitudeFeet / 100.0f));
  else snprintf(out, size, "%d ft", (int)(lroundf(a.altitudeFeet / 50.0f) * 50));
}

void formatKm(float km, char *out, size_t size) {
  if (km < 10) snprintf(out, size, "%.1f km", km);
  else snprintf(out, size, "%.0f km", km);
  for (char *p = out; *p; ++p) if (*p == '.') *p = ',';
}

int findSelected(const FlightRadarSnapshot &data) {
  if (!selectedHex[0]) return -1;
  for (uint8_t i = 0; i < data.count; ++i)
    if (!strcmp(data.aircraft[i].icao24, selectedHex)) return i;
  return -1;
}

// Dead reckoning: polls arrive every ~12 s, so between them each aircraft is
// moved along its track at ground speed. Positions are also corrected for the
// age of the last ADS-B position (seen_pos).
void extrapolate(const Aircraft &a, uint32_t fetchedMs, double &lat, double &lon) {
  lat = a.latitude;
  lon = a.longitude;
  if (a.onGround || !a.hasSpeed || !a.hasHeading || a.speedKnots < 30 || !fetchedMs) return;
  float dt = (millis() - fetchedMs) / 1000.0f + (a.hasPositionAge ? a.positionAgeSeconds : 0);
  if (dt > 45) dt = 45;
  const double distNm = a.speedKnots * dt / 3600.0;
  const double h = a.headingDegrees * 0.017453292519943295;
  lat += distNm * cos(h) / 60.0;
  lon += distNm * sin(h) / (60.0 * cos(a.latitude * 0.017453292519943295));
}

void project(double lat, double lon, const FlightRadarSnapshot &data, int &x, int &y) {
  Aircraft p;
  p.latitude = lat;
  p.longitude = lon;
  projectAircraft(p, data.centerLat, data.centerLon, data.radiusKm / 1.852f,
                  SCOPE_CX, SCOPE_CY, SCOPE_R, x, y);
}

bool insideScope(int x, int y, int margin = 0) {
  const int dx = x - SCOPE_CX, dy = y - SCOPE_CY, r = SCOPE_R - margin;
  return dx * dx + dy * dy <= r * r;
}

lv_obj_t *makeBox(lv_obj_t *parent, int w, int h, uint32_t bg, lv_opa_t opa, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *makePill(lv_obj_t *parent, int x, int y, int w, lv_event_cb_t cb) {
  lv_obj_t *pill = makeBox(parent, w, 26, 0x0A2224, LV_OPA_80, 13);
  lv_obj_set_pos(pill, x, y);
  lv_obj_set_style_border_color(pill, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_opa(pill, LV_OPA_60, 0);
  if (cb) {
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pill, 6);
    lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x16484A), LV_STATE_PRESSED);
  }
  return pill;
}

lv_obj_t *makeText(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *label = makeLabel(parent, text, font, color);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

// ---------------------------------------------------------------- details cache
AircraftDetails *findDetails(const char *hex, const char *callsign) {
  for (uint8_t i = 0; i < DETAILS_CACHE; ++i) {
    AircraftDetails &d = detailsCache[i];
    if (d.state != DetailsState::NONE && !strcmp(d.icao24, hex) && !strcmp(d.callsign, callsign)) return &d;
  }
  return nullptr;
}

// UI thread: make sure the route/model of the selected aircraft is known or queued.
void requestSelectedDetails(const Aircraft &a) {
  if (!a.icao24[0]) return;
  bool post = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  AircraftDetails *d = findDetails(a.icao24, a.callsign);
  if (!d || (d->state == DetailsState::FAILED && millis() - d->requestedMs > 30000UL)) {
    if (!d) {
      // Reuse the oldest entry.
      d = &detailsCache[0];
      for (uint8_t i = 1; i < DETAILS_CACHE; ++i)
        if (detailsCache[i].requestedMs < d->requestedMs) d = &detailsCache[i];
      *d = AircraftDetails();
      snprintf(d->icao24, sizeof d->icao24, "%s", a.icao24);
      snprintf(d->callsign, sizeof d->callsign, "%s", a.callsign);
    }
    d->state = DetailsState::LOADING;
    d->requestedMs = millis();
    ++detailsVersion;
    post = true;
  }
  xSemaphoreGive(mtx);
  if (post && requestDetails) requestDetails();
}

// ---------------------------------------------------------------- rendering
void renderClock() {
  struct tm ti;
  char text[12];
  if (getLocalTime(&ti, 0)) strftime(text, sizeof text, cfg.h24 ? "%H:%M" : "%I:%M", &ti);
  else snprintf(text, sizeof text, "--:--");
  setLabelIfChanged(clockLabel, text);
}

void renderAge(const FlightRadarSnapshot &data) {
  const uint32_t age = data.lastSuccessMs ? (millis() - data.lastSuccessMs) / 1000UL : UINT32_MAX - 1;
  if (age == renderedAgeSeconds) return;
  renderedAgeSeconds = age;
  char text[40];
  if (data.lastSuccessMs) snprintf(text, sizeof text, "%u %s  •  há %lus", data.count,
                                   data.count == 1 ? "avião" : "aviões", (unsigned long)age);
  else snprintf(text, sizeof text, "%s", data.status == FlightRadarStatus::LOADING ? "Consultando..." : "Sem dados");
  setLabelIfChanged(countLabel, text);
  renderClock();
}

void updateTrails(const FlightRadarSnapshot &data) {
  static uint32_t generation = 0;
  ++generation;
  for (uint8_t i = 0; i < data.count; ++i) {
    const Aircraft &a = data.aircraft[i];
    if (!a.icao24[0]) continue;
    Trail *t = nullptr, *spare = nullptr;
    for (uint8_t j = 0; j < FLIGHT_RADAR_MAX_AIRCRAFT; ++j) {
      if (!strcmp(trails[j].hex, a.icao24)) { t = &trails[j]; break; }
      if (!spare && trails[j].seen + 1 < generation) spare = &trails[j];
    }
    if (!t) {
      if (!spare) continue;
      t = spare;
      snprintf(t->hex, sizeof t->hex, "%s", a.icao24);
      t->n = 0;
    }
    t->seen = generation;
    const bool moved = !t->n || fabsf(t->lat[t->n - 1] - (float)a.latitude) > 0.0004f ||
                       fabsf(t->lon[t->n - 1] - (float)a.longitude) > 0.0004f;
    if (!moved) continue;
    if (t->n == TRAIL_POINTS) {
      memmove(t->lat, t->lat + 1, sizeof(float) * (TRAIL_POINTS - 1));
      memmove(t->lon, t->lon + 1, sizeof(float) * (TRAIL_POINTS - 1));
      --t->n;
    }
    t->lat[t->n] = a.latitude;
    t->lon[t->n] = a.longitude;
    ++t->n;
  }
}

void placeTrail(const FlightRadarSnapshot &data, int selected, int headX, int headY) {
  const Trail *t = nullptr;
  if (selected >= 0)
    for (uint8_t j = 0; j < FLIGHT_RADAR_MAX_AIRCRAFT; ++j)
      if (!strcmp(trails[j].hex, selectedHex)) { t = &trails[j]; break; }
  if (!t || t->n < 1) { lv_obj_add_flag(trailLine, LV_OBJ_FLAG_HIDDEN); return; }
  uint8_t n = 0;
  for (uint8_t k = 0; k < t->n; ++k) {
    int x, y;
    project(t->lat[k], t->lon[k], data, x, y);
    trailPoints[n].x = x;
    trailPoints[n].y = y;
    ++n;
  }
  trailPoints[n].x = headX;
  trailPoints[n].y = headY;
  ++n;
  lv_line_set_points(trailLine, trailPoints, n);
  lv_obj_remove_flag(trailLine, LV_OBJ_FLAG_HIDDEN);
}

// Moves every marker to its dead-reckoned position. Cheap: called each UI tick.
void placeAircraft(const FlightRadarSnapshot &data) {
  static int16_t px[FLIGHT_RADAR_MAX_AIRCRAFT], py[FLIGHT_RADAR_MAX_AIRCRAFT];
  static bool visible[FLIGHT_RADAR_MAX_AIRCRAFT];
  const int selected = findSelected(data);
  for (uint8_t i = 0; i < FLIGHT_RADAR_MAX_AIRCRAFT; ++i) {
    lv_obj_t *img = planeImg[i];
    visible[i] = false;
    if (i >= data.count) { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); continue; }
    double lat, lon;
    extrapolate(data.aircraft[i], data.lastSuccessMs, lat, lon);
    int x, y;
    project(lat, lon, data, x, y);
    if (!insideScope(x, y, 4)) { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); continue; }
    visible[i] = true;
    px[i] = x; py[i] = y;
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(img, x - ICON_PX / 2, y - ICON_PX / 2);
  }

  // Callsign tags: selected aircraft first, then the nearest airborne ones,
  // skipping any tag that would overlap one already placed (no clutter).
  lv_area_t placed[TAG_COUNT];
  uint8_t tags = 0;
  auto overlaps = [&](const lv_area_t &r) {
    for (uint8_t k = 0; k < tags; ++k)
      if (r.x1 <= placed[k].x2 + 2 && r.x2 + 2 >= placed[k].x1 && r.y1 <= placed[k].y2 + 1 && r.y2 + 1 >= placed[k].y1) return true;
    return false;
  };
  for (int pass = 0; pass < 2 && tags < TAG_COUNT; ++pass) {
    for (uint8_t i = 0; i < data.count && tags < TAG_COUNT; ++i) {
      if (!visible[i]) continue;
      const bool isSel = (int)i == selected;
      if (pass == 0 ? !isSel : (isSel || data.aircraft[i].onGround || i >= 16)) continue;
      const Aircraft &a = data.aircraft[i];
      lv_obj_t *tag = tagLabel[tags];
      const char *name = a.callsign[0] ? a.callsign : (a.registration[0] ? a.registration : a.icao24);
      setLabelIfChanged(tag, name);
      lv_obj_update_layout(tag);
      const int w = lv_obj_get_width(tag), h = lv_obj_get_height(tag);
      lv_area_t r = {px[i] + 12, py[i] - h / 2, px[i] + 12 + w, py[i] + h / 2};
      if (!insideScope(r.x2, r.y1, 2) || !insideScope(r.x2, r.y2, 2) || overlaps(r)) {
        r.x1 = px[i] - 12 - w; r.x2 = px[i] - 12;                // try the left side
        if (!insideScope(r.x1, r.y1, 2) || !insideScope(r.x1, r.y2, 2) || (overlaps(r) && !isSel)) continue;
      }
      lv_obj_set_style_text_color(tag, lv_color_hex(isSel ? C_SELECT : C_TEXT), 0);
      lv_obj_set_style_border_color(tag, lv_color_hex(isSel ? C_SELECT : altitudeColor(a)), 0);
      lv_obj_set_pos(tag, r.x1, r.y1);
      lv_obj_remove_flag(tag, LV_OBJ_FLAG_HIDDEN);
      placed[tags++] = r;
    }
  }
  for (uint8_t t = tags; t < TAG_COUNT; ++t) lv_obj_add_flag(tagLabel[t], LV_OBJ_FLAG_HIDDEN);

  if (selected >= 0 && visible[selected]) {
    lv_obj_set_pos(selRing, px[selected] - 19, py[selected] - 19);
    lv_obj_remove_flag(selRing, LV_OBJ_FLAG_HIDDEN);
    placeTrail(data, selected, px[selected], py[selected]);
  } else {
    lv_obj_add_flag(selRing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(trailLine, LV_OBJ_FLAG_HIDDEN);
  }
}

void styleMarkers(const FlightRadarSnapshot &data) {
  const int selected = findSelected(data);
  for (uint8_t i = 0; i < data.count; ++i) {
    const Aircraft &a = data.aircraft[i];
    lv_obj_t *img = planeImg[i];
    const lv_image_dsc_t *icon = iconFor(a);
    if (lv_image_get_src(img) != icon) lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(i == selected ? C_SELECT : altitudeColor(a)), 0);
    lv_image_set_scale(img, iconScale(a) + (i == selected ? 40 : 0));
    lv_image_set_rotation(img, a.hasHeading ? (int32_t)lroundf(a.headingDegrees * 10.0f) : 0);
  }
}

void renderList(const FlightRadarSnapshot &data) {
  const bool showList = data.count > 0;
  lv_obj_add_flag(detailView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(listView, LV_OBJ_FLAG_HIDDEN);
  for (uint8_t r = 0; r < LIST_ROWS; ++r) {
    if (!showList || r >= data.count) { lv_obj_add_flag(listRow[r], LV_OBJ_FLAG_HIDDEN); continue; }
    const Aircraft &a = data.aircraft[r];
    lv_obj_remove_flag(listRow[r], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(listDot[r], lv_color_hex(altitudeColor(a)), 0);
    setLabelIfChanged(listCall[r], a.callsign[0] ? a.callsign : (a.registration[0] ? a.registration : a.icao24));
    char alt[12], km[12], sub[40];
    formatAltitude(a, alt, sizeof alt);
    formatKm(a.distanceNm * 1.852f, km, sizeof km);
    snprintf(sub, sizeof sub, "%s  •  %s", alt, km);
    setLabelIfChanged(listSub[r], sub);
  }
  if (showList) {
    lv_obj_add_flag(listEmpty, LV_OBJ_FLAG_HIDDEN);
    setLabelIfChanged(listTitle, "MAIS PRÓXIMOS");
  } else {
    lv_obj_remove_flag(listEmpty, LV_OBJ_FLAG_HIDDEN);
    setLabelIfChanged(listTitle, "RADAR");
    char msg[96];
    if (data.status == FlightRadarStatus::LOADING) snprintf(msg, sizeof msg, "Buscando aeronaves perto de %s...", data.airportCode);
    else if (data.status == FlightRadarStatus::EMPTY) snprintf(msg, sizeof msg, "Nenhuma aeronave em %.0f km de %s.", data.radiusKm, data.airportCode);
    else snprintf(msg, sizeof msg, "%s", data.message);
    setLabelIfChanged(listEmpty, msg);
  }
}

void renderDetails(const Aircraft &a) {
  lv_obj_add_flag(listView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(detailView, LV_OBJ_FLAG_HIDDEN);
  setLabelIfChanged(dTitle, a.callsign[0] ? a.callsign : (a.registration[0] ? a.registration : a.icao24));

  AircraftDetails details;
  xSemaphoreTake(mtx, portMAX_DELAY);
  const AircraftDetails *cached = findDetails(a.icao24, a.callsign);
  if (cached) details = *cached;
  xSemaphoreGive(mtx);

  char line[64];
  const bool ready = details.state == DetailsState::READY;
  if (details.state == DetailsState::LOADING || details.state == DetailsState::NONE)
    setLabelIfChanged(dAirline, "Buscando rota...");
  else if (ready && details.airline[0]) setLabelIfChanged(dAirline, details.airline);
  else if (ready && details.owner[0]) setLabelIfChanged(dAirline, details.owner);
  else setLabelIfChanged(dAirline, details.state == DetailsState::FAILED ? "Rota indisponível" : "Voo sem rota cadastrada");

  if (ready && details.originCode[0] && details.destCode[0]) {
    snprintf(line, sizeof line, "%s " LV_SYMBOL_RIGHT " %s", details.originCode, details.destCode);
    setLabelIfChanged(dRoute, line);
    snprintf(line, sizeof line, "%s - %s", details.originCity, details.destCity);
    setLabelIfChanged(dCities, line);
  } else {
    setLabelIfChanged(dRoute, "--- " LV_SYMBOL_RIGHT " ---");
    setLabelIfChanged(dCities, "");
  }
  if (ready && details.model[0]) setLabelIfChanged(dModel, details.model);
  else setLabelIfChanged(dModel, a.type[0] ? a.type : "Modelo desconhecido");

  char v[24];
  formatAltitude(a, v, sizeof v);
  setLabelIfChanged(dValue[0], v);
  if (a.hasSpeed) snprintf(v, sizeof v, "%.0f kt", a.speedKnots); else snprintf(v, sizeof v, "--");
  setLabelIfChanged(dValue[1], v);
  if (a.hasHeading) snprintf(v, sizeof v, "%03.0f°", a.headingDegrees); else snprintf(v, sizeof v, "--");
  setLabelIfChanged(dValue[2], v);
  if (a.hasVerticalRate && fabsf(a.verticalRateFpm) >= 100)
    snprintf(v, sizeof v, "%s%.0f", a.verticalRateFpm > 0 ? LV_SYMBOL_UP " " : LV_SYMBOL_DOWN " ", fabsf(a.verticalRateFpm));
  else snprintf(v, sizeof v, "%s", a.hasVerticalRate ? "nivelado" : "--");
  setLabelIfChanged(dValue[3], v);
  formatKm(a.distanceNm * 1.852f, v, sizeof v);
  setLabelIfChanged(dValue[4], v);
  setLabelIfChanged(dValue[5], a.squawk[0] ? a.squawk : "--");
  lv_obj_set_style_text_color(dValue[5], lv_color_hex(a.emergency ? C_ALERT : C_TEXT), 0);

  snprintf(line, sizeof line, "%s%s%s", a.registration[0] ? a.registration : a.icao24,
           a.type[0] ? "  •  " : "", a.type);
  setLabelIfChanged(dFooter, line);
}

void renderPanel(const FlightRadarSnapshot &data) {
  const int selected = findSelected(data);
  if (selected >= 0) renderDetails(data.aircraft[selected]);
  else renderList(data);
}

void renderMap() {
  int8_t front;
  uint32_t version;
  xSemaphoreTake(mtx, portMAX_DELAY);
  front = mapFront;
  version = mapVersion;
  xSemaphoreGive(mtx);
  if (version == renderedMapVersion) return;
  renderedMapVersion = version;
  if (!mapEnabled || !mapAreaValid || front < 0 || !mapBuffer[front]) {
    lv_obj_add_flag(mapImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(attribution, LV_OBJ_FLAG_HIDDEN);
    mapShownByUi = -1;
    return;
  }
  // Same descriptor may be reused with new pixels: force a redraw.
  lv_image_set_src(mapImg, &mapDsc[front]);
  lv_obj_invalidate(mapImg);
  lv_obj_remove_flag(mapImg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(attribution, LV_OBJ_FLAG_HIDDEN);
  mapShownByUi = front;
}

void renderRange(const FlightRadarSnapshot &data) {
  char text[16];
  snprintf(text, sizeof text, "%.0f km", data.radiusKm);
  setLabelIfChanged(rangeText, text);
  for (int r = 0; r < 3; ++r) {
    formatKm(data.radiusKm * (r + 1) / 3.0f, text, sizeof text);
    if (r < 2) { char *sp = strchr(text, ' '); if (sp) *sp = 0; }   // only the outer ring shows "km"
    setLabelIfChanged(ringLabel[r], text);
  }
  char airport[40];
  snprintf(airport, sizeof airport, "%s  •  %s", data.airportCode, data.airportName);
  setLabelIfChanged(airportLabel, airport);
}

void renderSnapshot(const FlightRadarSnapshot &data) {
  setLabelIfChanged(statusText, statusName(data.status));
  lv_obj_set_style_bg_color(statusDot, lv_color_hex(statusColor(data.status)), 0);
  renderRange(data);
  styleMarkers(data);
  placeAircraft(data);
  renderPanel(data);
  renderedAgeSeconds = UINT32_MAX;
  renderAge(data);
}

// ---------------------------------------------------------------- events
void selectHex(const char *hex) {
  snprintf(selectedHex, sizeof selectedHex, "%s", hex ? hex : "");
  const int sel = findSelected(*uiCopy);
  if (sel >= 0) requestSelectedDetails(uiCopy->aircraft[sel]);
  styleMarkers(*uiCopy);
  placeAircraft(*uiCopy);
  renderPanel(*uiCopy);
}

void planeClickEvent(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < uiCopy->count) selectHex(uiCopy->aircraft[i].icao24);
}

void rowClickEvent(lv_event_t *e) {
  const int r = (int)(intptr_t)lv_event_get_user_data(e);
  if (r < uiCopy->count) selectHex(uiCopy->aircraft[r].icao24);
}

void closeDetailsEvent(lv_event_t *) { selectHex(nullptr); }

void scopeClickEvent(lv_event_t *) { if (selectedHex[0]) selectHex(nullptr); }

void manualRefreshEvent(lv_event_t *) {
  if (requestRefresh) requestRefresh();
  setLabelIfChanged(statusText, "ATUALIZANDO");
}

void rangeClickEvent(lv_event_t *) {
  static const uint8_t ranges[] = {5, 10, 25, 50};
  uint8_t current = (uint8_t)lroundf(uiCopy->radiusKm), next = ranges[0];
  for (size_t i = 0; i < sizeof ranges; ++i)
    if (ranges[i] == current) { next = ranges[(i + 1) % sizeof ranges]; break; }
  setFlightRadarRadiusKm(next);
}

void mapClickEvent(lv_event_t *) {
  mapEnabled = !mapEnabled;
  prefs.putBool("radarMap", mapEnabled);
  lv_obj_set_style_text_color(mapText, lv_color_hex(mapEnabled ? C_ACCENT : C_MUTED), 0);
  renderedMapVersion = UINT32_MAX;
  xSemaphoreTake(mtx, portMAX_DELAY);
  mapRetryAfterMs = 0;
  ++mapVersion;
  xSemaphoreGive(mtx);
  renderMap();
  if (mapEnabled && requestRefresh) requestRefresh();
}

void sweepTimerCb(lv_timer_t *) {
  sweepAngle = (sweepAngle + 4) % 360;
  static const int spans[3][2] = {{10, 0}, {26, 10}, {50, 26}};
  for (int k = 0; k < 3; ++k)
    lv_arc_set_angles(sweepArc[k], (sweepAngle - spans[k][0] + 360) % 360, (sweepAngle - spans[k][1] + 360) % 360);
  const float a = sweepAngle * 0.017453292f;
  sweepPoints[1].x = SCOPE_CX + (int)lroundf(cosf(a) * (SCOPE_R - 1));
  sweepPoints[1].y = SCOPE_CY + (int)lroundf(sinf(a) * (SCOPE_R - 1));
  lv_line_set_points(sweepLine, sweepPoints, 2);
}

// ---------------------------------------------------------------- build
void buildScope(lv_obj_t *screen) {
  // Plain scope disc (visible when the map is off or still loading).
  scopeFill = makeBox(screen, SCOPE_R * 2, SCOPE_R * 2, C_SCOPE, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(scopeFill, SCOPE_CX - SCOPE_R, SCOPE_CY - SCOPE_R);
  lv_obj_set_style_bg_grad_color(scopeFill, lv_color_hex(0x0C2B2A), 0);
  lv_obj_set_style_bg_grad_dir(scopeFill, LV_GRAD_DIR_VER, 0);
  lv_obj_add_flag(scopeFill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scopeFill, scopeClickEvent, LV_EVENT_CLICKED, nullptr);

  for (int i = 0; i < 2; ++i) {
    lv_memzero(&mapDsc[i], sizeof mapDsc[i]);
    mapDsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
    mapDsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
    mapDsc[i].header.w = MAP_SIZE;
    mapDsc[i].header.h = MAP_SIZE;
    mapDsc[i].header.stride = MAP_SIZE * 2;
    mapDsc[i].data_size = MAP_SIZE * MAP_SIZE * 2;
  }
  mapImg = lv_image_create(screen);
  lv_obj_set_pos(mapImg, SCOPE_CX - SCOPE_R, SCOPE_CY - SCOPE_R);
  lv_obj_set_size(mapImg, MAP_SIZE, MAP_SIZE);
  lv_obj_remove_flag(mapImg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(mapImg, LV_OBJ_FLAG_HIDDEN);

  // Sweep: three fading wedges + a bright leading edge.
  static const lv_opa_t sweepOpa[3] = {LV_OPA_40, LV_OPA_20, LV_OPA_10};
  for (int k = 0; k < 3; ++k) {
    lv_obj_t *arc = lv_arc_create(screen);
    lv_obj_set_size(arc, SCOPE_R * 2, SCOPE_R * 2);
    lv_obj_set_pos(arc, SCOPE_CX - SCOPE_R, SCOPE_CY - SCOPE_R);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, SCOPE_R, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, sweepOpa[k], LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_angles(arc, 0, 1);
    if (!SWEEP_ENABLED) lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    sweepArc[k] = arc;
  }
  sweepPoints[0].x = SCOPE_CX;
  sweepPoints[0].y = SCOPE_CY;
  sweepPoints[1] = sweepPoints[0];
  sweepLine = lv_line_create(screen);
  lv_obj_set_pos(sweepLine, 0, 0);
  lv_obj_set_style_line_color(sweepLine, lv_color_hex(0x9FFFEA), 0);
  lv_obj_set_style_line_width(sweepLine, 2, 0);
  lv_obj_set_style_line_opa(sweepLine, LV_OPA_80, 0);
  lv_obj_remove_flag(sweepLine, LV_OBJ_FLAG_CLICKABLE);
  lv_line_set_points(sweepLine, sweepPoints, 2);
  if (!SWEEP_ENABLED) lv_obj_add_flag(sweepLine, LV_OBJ_FLAG_HIDDEN);

  // Range rings + crosshair.
  for (int n = 1; n <= 3; ++n) {
    const int d = SCOPE_R * 2 * n / 3;
    lv_obj_t *ring = makeBox(screen, d, d, 0, LV_OPA_TRANSP, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(ring, SCOPE_CX - d / 2, SCOPE_CY - d / 2);
    lv_obj_set_style_border_color(ring, lv_color_hex(C_RING), 0);
    lv_obj_set_style_border_width(ring, n == 3 ? 2 : 1, 0);
    lv_obj_set_style_border_opa(ring, n == 3 ? LV_OPA_COVER : LV_OPA_60, 0);
    ringLabel[n - 1] = makeText(screen, "", &lv_font_weather_14, C_MUTED);
    lv_obj_set_pos(ringLabel[n - 1], SCOPE_CX + d / 2 * 707 / 1000 + 2, SCOPE_CY + d / 2 * 707 / 1000 - 4);
  }
  static lv_point_precise_t crossH[2] = {{SCOPE_CX - SCOPE_R, SCOPE_CY}, {SCOPE_CX + SCOPE_R, SCOPE_CY}};
  static lv_point_precise_t crossV[2] = {{SCOPE_CX, SCOPE_CY - SCOPE_R}, {SCOPE_CX, SCOPE_CY + SCOPE_R}};
  for (lv_point_precise_t *pts : {crossH, crossV}) {
    lv_obj_t *line = lv_line_create(screen);
    lv_obj_set_pos(line, 0, 0);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(C_RING), 0);
    lv_obj_set_style_line_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  }

  // Compass bezel: tick every 10 deg, cardinal letters.
  static const char *cardinals[] = {"N", "E", "S", "W", "N", nullptr};
  lv_obj_t *scale = lv_scale_create(screen);
  const int bezel = SCOPE_R * 2 + 18;
  lv_obj_set_size(scale, bezel, bezel);
  lv_obj_set_pos(scale, SCOPE_CX - bezel / 2, SCOPE_CY - bezel / 2);
  lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_range(scale, 0, 360);
  lv_scale_set_angle_range(scale, 360);
  lv_scale_set_rotation(scale, 270);
  lv_scale_set_total_tick_count(scale, 37);
  lv_scale_set_major_tick_every(scale, 9);
  lv_scale_set_label_show(scale, true);
  lv_scale_set_text_src(scale, cardinals);
  lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scale, 0, 0);
  lv_obj_set_style_arc_width(scale, 0, LV_PART_MAIN);
  lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
  lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
  lv_obj_set_style_line_color(scale, lv_color_hex(C_RING), LV_PART_ITEMS);
  lv_obj_set_style_length(scale, 9, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(scale, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_text_font(scale, &lv_font_weather_14, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(scale, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
  lv_obj_remove_flag(scale, LV_OBJ_FLAG_CLICKABLE);

  // Airport marker.
  lv_obj_t *center = makeBox(screen, 8, 8, C_SELECT, LV_OPA_COVER, 2);
  lv_obj_set_pos(center, SCOPE_CX - 4, SCOPE_CY - 4);
  lv_obj_set_style_transform_rotation(center, 450, 0);
  lv_obj_set_style_transform_pivot_x(center, 4, 0);
  lv_obj_set_style_transform_pivot_y(center, 4, 0);

  // Selected aircraft trail + highlight ring.
  trailLine = lv_line_create(screen);
  lv_obj_set_pos(trailLine, 0, 0);
  lv_obj_set_style_line_color(trailLine, lv_color_hex(C_SELECT), 0);
  lv_obj_set_style_line_width(trailLine, 2, 0);
  lv_obj_set_style_line_opa(trailLine, LV_OPA_70, 0);
  lv_obj_set_style_line_rounded(trailLine, true, 0);
  lv_obj_remove_flag(trailLine, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(trailLine, LV_OBJ_FLAG_HIDDEN);
  selRing = makeBox(screen, 38, 38, 0, LV_OPA_TRANSP, LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_color(selRing, lv_color_hex(C_SELECT), 0);
  lv_obj_set_style_border_width(selRing, 2, 0);
  lv_obj_add_flag(selRing, LV_OBJ_FLAG_HIDDEN);

  // Aircraft marker pool (one image each, recoloured + rotated by LVGL).
  for (uint8_t i = 0; i < FLIGHT_RADAR_MAX_AIRCRAFT; ++i) {
    lv_obj_t *img = lv_image_create(screen);
    lv_image_set_src(img, &radar_icon_jet);
    lv_obj_set_size(img, ICON_PX, ICON_PX);
    lv_image_set_pivot(img, ICON_PX / 2, ICON_PX / 2);
    lv_image_set_antialias(img, true);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(img, 8);
    lv_obj_add_event_cb(img, planeClickEvent, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    planeImg[i] = img;
  }
  for (uint8_t t = 0; t < TAG_COUNT; ++t) {
    lv_obj_t *tag = makeText(screen, "", &lv_font_weather_14, C_TEXT);
    lv_obj_set_style_bg_color(tag, lv_color_hex(0x031012), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_80, 0);
    lv_obj_set_style_border_width(tag, 1, 0);
    lv_obj_set_style_radius(tag, 4, 0);
    lv_obj_set_style_pad_hor(tag, 4, 0);
    lv_obj_set_style_pad_ver(tag, 1, 0);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    tagLabel[t] = tag;
  }

  // Corner pills (outside the circle).
  rangePill = makePill(screen, 252, 4, 64, rangeClickEvent);
  rangeText = makeText(rangePill, "10 km", &lv_font_weather_14, C_ACCENT);
  lv_obj_center(rangeText);
  statusPill = makePill(screen, 4, 290, 96, manualRefreshEvent);
  statusDot = makeBox(statusPill, 8, 8, 0x35E08F, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lv_obj_align(statusDot, LV_ALIGN_LEFT_MID, 9, 0);
  statusText = makeText(statusPill, "BUSCANDO", &lv_font_weather_14, C_TEXT);
  lv_obj_align(statusText, LV_ALIGN_LEFT_MID, 22, 0);
  mapPill = makePill(screen, 252, 290, 64, mapClickEvent);
  mapText = makeText(mapPill, "MAPA", &lv_font_weather_14, mapEnabled ? C_ACCENT : C_MUTED);
  lv_obj_center(mapText);
  attribution = makeText(screen, "Esri", &lv_font_weather_14, C_MUTED);
  lv_obj_set_pos(attribution, 214, 296);
  lv_obj_add_flag(attribution, LV_OBJ_FLAG_HIDDEN);
}

void buildPanel(lv_obj_t *screen) {
  clockLabel = makeText(screen, "--:--", &lv_font_montserrat_28, C_TEXT);
  lv_obj_set_pos(clockLabel, PANEL_X + 2, 2);
  lv_obj_t *refresh = makePill(screen, PANEL_X + PANEL_W - 36, 6, 36, manualRefreshEvent);
  lv_obj_t *refreshIcon = makeText(refresh, LV_SYMBOL_REFRESH, &lv_font_montserrat_14, C_ACCENT);
  lv_obj_center(refreshIcon);
  airportLabel = makeText(screen, "", &lv_font_weather_14, C_ACCENT);
  lv_obj_set_pos(airportLabel, PANEL_X + 2, 38);
  lv_obj_set_width(airportLabel, PANEL_W);
  lv_label_set_long_mode(airportLabel, LV_LABEL_LONG_DOT);
  countLabel = makeText(screen, "", &lv_font_weather_14, C_MUTED);
  lv_obj_set_pos(countLabel, PANEL_X + 2, 56);

  lv_obj_t *card = makeBox(screen, PANEL_W, 238, C_PANEL, LV_OPA_COVER, 14);
  lv_obj_set_pos(card, PANEL_X, 78);
  lv_obj_set_style_border_color(card, lv_color_hex(C_PANEL_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  const int inner = PANEL_W - 16;

  // ---- list of the nearest aircraft
  listView = makeBox(card, inner, 222, 0, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(listView, 8, 8);
  listTitle = makeText(listView, "MAIS PRÓXIMOS", &lv_font_weather_14, C_MUTED);
  lv_obj_set_pos(listTitle, 0, 0);
  listEmpty = makeText(listView, "", &lv_font_weather_14, C_MUTED);
  lv_obj_set_width(listEmpty, inner);
  lv_obj_set_pos(listEmpty, 0, 28);
  lv_label_set_long_mode(listEmpty, LV_LABEL_LONG_WRAP);
  for (uint8_t r = 0; r < LIST_ROWS; ++r) {
    lv_obj_t *row = makeBox(listView, inner, 38, 0x12302F, LV_OPA_TRANSP, 8);
    lv_obj_set_pos(row, 0, 22 + r * 40);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, rowClickEvent, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    listDot[r] = makeBox(row, 8, 8, C_ACCENT, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(listDot[r], 2, 6);
    listCall[r] = makeText(row, "", &lv_font_weather_14, C_TEXT);
    lv_obj_set_pos(listCall[r], 16, 1);
    listSub[r] = makeText(row, "", &lv_font_weather_14, C_MUTED);
    lv_obj_set_pos(listSub[r], 16, 19);
    lv_obj_set_width(listSub[r], inner - 18);
    lv_label_set_long_mode(listSub[r], LV_LABEL_LONG_DOT);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    listRow[r] = row;
  }

  // ---- selected aircraft
  detailView = makeBox(card, inner, 222, 0, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(detailView, 8, 8);
  lv_obj_add_flag(detailView, LV_OBJ_FLAG_HIDDEN);
  dTitle = makeText(detailView, "", &lv_font_weather_20, C_SELECT);
  lv_obj_set_pos(dTitle, 0, 0);
  lv_obj_t *close = makePill(detailView, inner - 28, 0, 28, closeDetailsEvent);
  lv_obj_t *closeIcon = makeText(close, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, C_TEXT);
  lv_obj_center(closeIcon);
  auto line = [&](int y, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = makeText(detailView, "", font, color);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_width(l, inner);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
  };
  dAirline = line(26, &lv_font_weather_14, C_MUTED);
  dRoute = line(44, &lv_font_weather_20, C_TEXT);
  dCities = line(68, &lv_font_weather_14, C_MUTED);
  dModel = line(86, &lv_font_weather_14, C_ACCENT);
  static const char *captions[6] = {"ALT", "VEL", "RUMO", "SUBIDA", "DIST", "SQUAWK"};
  for (int k = 0; k < 6; ++k) {
    const int x = (k % 2) * (inner / 2), y = 106 + (k / 2) * 32;
    lv_obj_t *cap = makeText(detailView, captions[k], &lv_font_weather_14, C_MUTED);
    lv_obj_set_style_text_opa(cap, LV_OPA_70, 0);
    lv_obj_set_pos(cap, x, y);
    dValue[k] = makeText(detailView, "--", &lv_font_weather_14, C_TEXT);
    lv_obj_set_pos(dValue[k], x, y + 15);
  }
  dFooter = line(204, &lv_font_weather_14, C_MUTED);
}
}  // namespace

// ================================================================ public API
void setFlightRadarRefreshRequester(void (*requester)()) { requestRefresh = requester; }
void setFlightRadarDetailsRequester(void (*requester)()) { requestDetails = requester; }
bool flightRadarIsActive() { return radarActive; }

void flightRadarOnEnter() {
  radarActive = true;
  renderedVersion = UINT32_MAX;
  renderedDetailsVersion = UINT32_MAX;
  renderedMapVersion = UINT32_MAX;
  renderedAgeSeconds = UINT32_MAX;
  if (sweepTimer && SWEEP_ENABLED) lv_timer_resume(sweepTimer);
}

void flightRadarOnExit() {
  radarActive = false;
  selectedHex[0] = 0;
  if (sweepTimer) lv_timer_pause(sweepTimer);
}

// Network task only.
static void maybeFetchMap(double lat, double lon, float radiusKm) {
  if (!mapEnabled || !radarActive || !screenJobStillCurrent()) return;
  if (mapAreaValid && mapFront >= 0 && mapLat == lat && mapLon == lon && mapRadius == radiusKm) return;
  if (mapRetryAfterMs && (int32_t)(millis() - mapRetryAfterMs) < 0) return;
  // Decode into the buffer the UI is NOT showing.
  const int8_t target = mapShownByUi == 0 ? 1 : 0;
  if (!mapBuffer[target]) {
    mapBuffer[target] = (uint16_t *)heap_caps_malloc(MAP_SIZE * MAP_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mapBuffer[target]) { mapRetryAfterMs = millis() + MAP_RETRY_MS; return; }
    mapDsc[target].data = (const uint8_t *)mapBuffer[target];
  }
  char error[48] = {};
  if (!fetchRadarMap(lat, lon, radiusKm, mapBuffer[target], MAP_SIZE, hexTo565(C_BG), error, sizeof error)) {
    mapRetryAfterMs = millis() + MAP_RETRY_MS;
    return;
  }
  mapRetryAfterMs = 0;
  mapLat = lat; mapLon = lon; mapRadius = radiusKm;
  xSemaphoreTake(mtx, portMAX_DELAY);
  mapFront = target;
  // Only valid if the user didn't switch airport/radius while downloading.
  mapAreaValid = snapshot->centerLat == lat && snapshot->centerLon == lon && snapshot->radiusKm == radiusKm;
  ++mapVersion;
  xSemaphoreGive(mtx);
}

bool updateFlightRadarData() {
  ensureState();
  if (!radarActive || !screenJobStillCurrent()) return false;
  uint8_t count = 0;
  char error[48] = {};
  double centerLat, centerLon;
  float radiusKm;
  xSemaphoreTake(mtx, portMAX_DELAY);
  centerLat = snapshot->centerLat;
  centerLon = snapshot->centerLon;
  radiusKm = snapshot->radiusKm;
  xSemaphoreGive(mtx);
  const bool ok = adsbLolProvider().fetch(centerLat, centerLon, radiusKm / 1.852f,
      freshAircraft, FLIGHT_RADAR_MAX_AIRCRAFT, count, error, sizeof error,
      screenJobStillCurrent);
  if (!screenJobStillCurrent()) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  // Settings may have changed while downloading: drop results for the old area.
  const bool sameArea = snapshot->centerLat == centerLat && snapshot->centerLon == centerLon &&
                        snapshot->radiusKm == radiusKm;
  if (ok && sameArea) {
    memcpy(snapshot->aircraft, freshAircraft, sizeof(Aircraft) * count);
    snapshot->count = count;
    snapshot->lastSuccessMs = millis();
    snapshot->status = count ? FlightRadarStatus::ONLINE : FlightRadarStatus::EMPTY;
    snprintf(snapshot->message, sizeof snapshot->message, "%s", count ? "Dados ADSB.lol" : "Nenhuma aeronave na área");
  } else if (!ok) {
    snapshot->status = snapshot->lastSuccessMs ? FlightRadarStatus::STALE : FlightRadarStatus::OFFLINE;
    snprintf(snapshot->message, sizeof snapshot->message, "%s", error[0] ? error : "Falha de consulta");
  }
  ++snapshot->version;
  xSemaphoreGive(mtx);
  // Basemap after the aircraft, so traffic shows up first.
  if (ok) maybeFetchMap(centerLat, centerLon, radiusKm);
  return ok;
}

bool updateFlightRadarDetails() {
  ensureState();
  if (!radarActive || !screenJobStillCurrent()) return false;
  AircraftDetails job;
  bool found = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint32_t newest = 0;
  for (uint8_t i = 0; i < DETAILS_CACHE; ++i)
    if (detailsCache[i].state == DetailsState::LOADING && detailsCache[i].requestedMs >= newest) {
      job = detailsCache[i];
      newest = job.requestedMs;
      found = true;
    }
  xSemaphoreGive(mtx);
  if (!found) return true;
  fetchAircraftDetails(job);
  xSemaphoreTake(mtx, portMAX_DELAY);
  AircraftDetails *slot = findDetails(job.icao24, job.callsign);
  if (slot) { job.requestedMs = slot->requestedMs; *slot = job; }
  ++detailsVersion;
  xSemaphoreGive(mtx);
  return job.state != DetailsState::FAILED;
}

bool setFlightRadarAirportCode(const char *input) {
  ensureState();
  struct Airport { const char *code; const char *name; double lat, lon; };
  static const Airport airports[] = {
    {"CGH","Congonhas",-23.626111,-46.656389},{"SBSP","Congonhas",-23.626111,-46.656389},
    {"GRU","Guarulhos",-23.435556,-46.473056},{"SBGR","Guarulhos",-23.435556,-46.473056},
    {"VCP","Viracopos",-23.007500,-47.134444},{"SBKP","Viracopos",-23.007500,-47.134444},
    {"SDU","Santos Dumont",-22.910500,-43.163100},{"SBRJ","Santos Dumont",-22.910500,-43.163100},
    {"GIG","Galeão",-22.809999,-43.250557},{"SBGL","Galeão",-22.809999,-43.250557},
    {"BSB","Brasília",-15.869167,-47.920834},{"SBBR","Brasília",-15.869167,-47.920834},
    {"CNF","Confins",-19.633750,-43.968856},{"SBCF","Confins",-19.633750,-43.968856},
    {"CWB","Curitiba",-25.528500,-49.175800},{"SBCT","Curitiba",-25.528500,-49.175800},
    {"POA","Porto Alegre",-29.994400,-51.171400},{"SBPA","Porto Alegre",-29.994400,-51.171400},
    {"FLN","Florianópolis",-27.670300,-48.552500},{"SBFL","Florianópolis",-27.670300,-48.552500},
    {"REC","Recife",-8.126400,-34.923600},{"SBRF","Recife",-8.126400,-34.923600},
    {"SSA","Salvador",-12.908600,-38.322500},{"SBSV","Salvador",-12.908600,-38.322500},
    {"FOR","Fortaleza",-3.776300,-38.532600},{"SBFZ","Fortaleza",-3.776300,-38.532600},
    {"BEL","Belém",-1.379300,-48.476300},{"SBBE","Belém",-1.379300,-48.476300},
    {"MAO","Manaus",-3.038600,-60.049700},{"SBEG","Manaus",-3.038600,-60.049700},
    {"NAT","Natal",-5.911400,-35.247700},{"SBSG","Natal",-5.911400,-35.247700},
    {"MCZ","Maceió",-9.510800,-35.791700},{"SBMO","Maceió",-9.510800,-35.791700},
    {"JPA","João Pessoa",-7.145800,-34.948600},{"SBJP","João Pessoa",-7.145800,-34.948600},
    {"IGU","Foz do Iguaçu",-25.596200,-54.487200},{"SBFI","Foz do Iguaçu",-25.596200,-54.487200},
    {"GYN","Goiânia",-16.632000,-49.220700},{"SBGO","Goiânia",-16.632000,-49.220700}
  };
  char code[5] = {};
  size_t n = 0;
  while (input && input[n] && n < sizeof(code) - 1) {
    char c = input[n];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    code[n] = c;
    ++n;
  }
  code[n] = '\0';
  const Airport *found = nullptr;
  for (const Airport &airport : airports) if (!strcmp(code, airport.code)) { found = &airport; break; }
  if (!found) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(snapshot->airportCode, sizeof snapshot->airportCode, "%s", code);
  snprintf(snapshot->airportName, sizeof snapshot->airportName, "%s", found->name);
  snapshot->centerLat = found->lat;
  snapshot->centerLon = found->lon;
  snapshot->count = 0;
  snapshot->lastSuccessMs = 0;
  snapshot->status = FlightRadarStatus::LOADING;
  snprintf(snapshot->message, sizeof snapshot->message, "Consultando %s", code);
  ++snapshot->version;
  mapRetryAfterMs = 0;
  mapAreaValid = false;   // hide the old basemap until the new one is decoded
  ++mapVersion;
  xSemaphoreGive(mtx);
  prefs.putString("radarAirport", code);
  if (radarActive && requestRefresh) requestRefresh();
  return true;
}

bool setFlightRadarRadiusKm(uint8_t radiusKm) {
  ensureState();
  if (radiusKm != 5 && radiusKm != 10 && radiusKm != 25 && radiusKm != 50) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  snapshot->radiusKm = radiusKm;
  snapshot->count = 0;
  snapshot->lastSuccessMs = 0;
  snapshot->status = FlightRadarStatus::LOADING;
  ++snapshot->version;
  mapRetryAfterMs = 0;
  mapAreaValid = false;   // hide the old basemap until the new one is decoded
  ++mapVersion;
  xSemaphoreGive(mtx);
  prefs.putUChar("radarRadiusKm", radiusKm);
  if (radarActive && requestRefresh) requestRefresh();
  return true;
}

void getFlightRadarSettings(char *code, size_t codeSize, char *name, size_t nameSize, uint8_t &radiusKm) {
  ensureState();
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (code) snprintf(code, codeSize, "%s", snapshot->airportCode);
  if (name) snprintf(name, nameSize, "%s", snapshot->airportName);
  radiusKm = (uint8_t)lroundf(snapshot->radiusKm);
  xSemaphoreGive(mtx);
}

void loadFlightRadarSettings() {
  ensureState();
  String saved = prefs.getString("radarAirport", "CGH");
  if (!setFlightRadarAirportCode(saved.c_str())) setFlightRadarAirportCode("CGH");
  uint8_t radius = prefs.getUChar("radarRadiusKm", 10);
  if (radius != 5 && radius != 10 && radius != 25 && radius != 50) radius = 10;
  snapshot->radiusKm = radius;
  mapEnabled = prefs.getBool("radarMap", true);
}

void buildFlightRadarScreen(lv_obj_t *screen) {
  ensureState();
  flightRadarView = screen;
  loadFlightRadarSettings();
  lv_obj_set_size(screen, SCR_W, SCR_H);
  lv_obj_set_pos(screen, 0, 0);
  lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_radius(screen, 0, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  buildScope(screen);
  buildPanel(screen);
  addBackButton(screen);   // last: always on top
  sweepTimer = lv_timer_create(sweepTimerCb, SWEEP_PERIOD_MS, nullptr);
  lv_timer_pause(sweepTimer);
  xSemaphoreTake(mtx, portMAX_DELAY);
  *uiCopy = *snapshot;
  xSemaphoreGive(mtx);
  renderedVersion = uiCopy->version;
  renderSnapshot(*uiCopy);
}

// UI thread, called every UI tick while the radar is the active screen.
void updateFlightRadarScreen() {
  if (!radarActive || !flightRadarView || !uiCopy) return;
  uint32_t version, dVersion;
  xSemaphoreTake(mtx, portMAX_DELAY);
  version = snapshot->version;
  dVersion = detailsVersion;
  const bool changed = version != renderedVersion;
  if (changed) *uiCopy = *snapshot;   // copy only when the network task published
  xSemaphoreGive(mtx);
  renderMap();
  if (changed) {
    if (uiCopy->lastSuccessMs && millis() - uiCopy->lastSuccessMs > FLIGHT_RADAR_UPDATE_INTERVAL_MS * 3 &&
        uiCopy->status == FlightRadarStatus::ONLINE) uiCopy->status = FlightRadarStatus::STALE;
    updateTrails(*uiCopy);
    renderSnapshot(*uiCopy);
    renderedVersion = version;
    // The selected aircraft left the area: back to the list.
    if (selectedHex[0] && findSelected(*uiCopy) < 0 && uiCopy->lastSuccessMs) { selectedHex[0] = 0; renderPanel(*uiCopy); }
  } else {
    placeAircraft(*uiCopy);   // dead-reckoning step
    renderAge(*uiCopy);
  }
  if (dVersion != renderedDetailsVersion) {
    renderedDetailsVersion = dVersion;
    if (selectedHex[0]) renderPanel(*uiCopy);
  }
}
