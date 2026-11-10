#include "app_log.h"
#include "radar_map_provider.h"
#include "aircraft_data_provider.h"
#include "https_fetch.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <math.h>
#include <new>
#include <stdio.h>

namespace {
PsramBuffer jpegBody(512 * 1024);

struct DecodeTarget {
  uint16_t *out;
  int size;
  int radius2;          // squared scope radius in px
  uint16_t bg;
  uint16_t lut[64];     // luminance (6 bit) -> tinted RGB565
};

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Street map is light: invert luminance so land becomes dark teal, roads
// slightly brighter and text labels bright cyan (the "night radar" look).
void buildTintLut(uint16_t *lut) {
  const float dark[3] = {6, 22, 26}, light[3] = {150, 225, 214};
  for (int i = 0; i < 64; ++i) {
    float v = 1.0f - i / 63.0f;          // inverted luminance
    v = v < 0.06f ? 0 : (v - 0.06f) / 0.94f;
    v = powf(v, 0.85f) * 0.95f;
    lut[i] = rgb565((uint8_t)(dark[0] + (light[0] - dark[0]) * v),
                    (uint8_t)(dark[1] + (light[1] - dark[1]) * v),
                    (uint8_t)(dark[2] + (light[2] - dark[2]) * v));
  }
}

int drawBlock(JPEGDRAW *draw) {
  DecodeTarget *t = (DecodeTarget *)draw->pUser;
  const int c = t->size / 2;
  for (int row = 0; row < draw->iHeight; ++row) {
    const int y = draw->y + row;
    if (y < 0 || y >= t->size) continue;
    const uint16_t *src = draw->pPixels + row * draw->iWidth;
    uint16_t *dst = t->out + y * t->size;
    const int dy = y - c;
    for (int col = 0; col < draw->iWidth; ++col) {
      const int x = draw->x + col;
      if (x < 0 || x >= t->size) continue;
      const int dx = x - c;
      if (dx * dx + dy * dy > t->radius2) { dst[x] = t->bg; continue; }
      const uint16_t p = src[col];
      // 6-bit luminance from RGB565
      const uint32_t r = (p >> 11) << 1, g = (p >> 5) & 0x3F, b = (p & 0x1F) << 1;
      dst[x] = t->lut[(r * 77 + g * 150 + b * 29) >> 8];
    }
  }
  return 1;
}
}  // namespace

bool fetchRadarMap(double lat, double lon, float radiusKm, uint16_t *out, int size,
                   uint16_t bgColor565, char *error, size_t errorCapacity) {
  if (WiFi.status() != WL_CONNECTED) { snprintf(error, errorCapacity, "Sem Wi-Fi"); return false; }
  // Square bbox in Web Mercator (EPSG:3857). Mercator stretches by 1/cos(lat),
  // so the ground radius is scaled to keep the same px/km as the radar.
  constexpr double R = 6378137.0, rad = 0.017453292519943295;
  const double x = R * lon * rad;
  const double y = R * log(tan(M_PI / 4.0 + lat * rad / 2.0));
  const double half = radiusKm * 1000.0 / cos(lat * rad);
  char url[320];
  snprintf(url, sizeof url,
           "https://server.arcgisonline.com/arcgis/rest/services/World_Street_Map/MapServer/export"
           "?bbox=%.1f,%.1f,%.1f,%.1f&bboxSR=3857&imageSR=3857&size=%d,%d&format=jpg&f=image",
           x - half, y - half, x + half, y + half, size, size);
  adsbLolReleaseConnection();  // one TLS context at a time
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12);
  const uint32_t started = millis();
  const int status = httpsFetch(client, url, jpegBody, 15000, false);
  client.stop();
  if (status != HTTP_CODE_OK) {
    snprintf(error, errorCapacity, status < 0 ? "Mapa: erro %d" : "Mapa: HTTP %d", status);
    return false;
  }
  // JPEGDEC keeps ~18 KB of state: put it in PSRAM, never on the task stack.
  void *mem = psramMalloc(sizeof(JPEGDEC));
  DecodeTarget *target = (DecodeTarget *)psramMalloc(sizeof(DecodeTarget));
  if (!mem || !target) {
    if (mem) heap_caps_free(mem);
    if (target) heap_caps_free(target);
    snprintf(error, errorCapacity, "Mapa: sem memória");
    return false;
  }
  JPEGDEC *jpeg = new (mem) JPEGDEC();
  target->out = out;
  target->size = size;
  target->radius2 = (size / 2) * (size / 2);
  target->bg = bgColor565;
  buildTintLut(target->lut);
  for (int i = 0; i < size * size; ++i) out[i] = bgColor565;
  bool ok = false;
  if (jpeg->openRAM((uint8_t *)jpegBody.data(), (int)jpegBody.length(), drawBlock)) {
    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    jpeg->setUserPointer(target);
    ok = jpeg->decode(0, 0, 0) == 1;
    if (!ok) snprintf(error, errorCapacity, "Mapa: JPEG erro %d", jpeg->getLastError());
    jpeg->close();
  } else {
    snprintf(error, errorCapacity, "Mapa: JPEG inválido");
  }
  jpeg->~JPEGDEC();
  heap_caps_free(mem);
  heap_caps_free(target);
  Log.printf("Mapa: %u bytes em %lums (%s)\n", (unsigned)jpegBody.length(),
                (unsigned long)(millis() - started), ok ? "ok" : error);
  return ok;
}
