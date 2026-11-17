// Armazenamento das fotos (LittleFS): slots /photos/<n>.jpg, enviados pela página web.
// Comum às placas; a tela de fotos de cada placa só lê daqui e observa photosVersion().
#include <Arduino.h>
#include <LittleFS.h>
#include "modules/photos/photos.h"

static bool fsReady() {
  static int state = -1;
  if (state < 0) {
    state = LittleFS.begin(true) ? 1 : 0;
    if (state == 1 && !LittleFS.exists("/photos")) LittleFS.mkdir("/photos");   // once, not on every check
  }
  return state == 1;
}

// Which slots hold a photo. Each LittleFS lookup costs ~10 ms and the screen checks all
// slots on entry (was ~200 ms): cached, refreshed after a web upload / delete.
static int8_t slotHas[PHOTO_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static uint32_t version = 1;

void photoPath(int slot, char *out, size_t n) { snprintf(out, n, "/photos/%d.jpg", slot); }

bool photoExists(int slot) {
  if (slot < 0 || slot >= PHOTO_SLOTS || !fsReady()) return false;
  if (slotHas[slot] < 0) { char p[24]; photoPath(slot, p, sizeof p); slotHas[slot] = LittleFS.exists(p) ? 1 : 0; }
  return slotHas[slot] == 1;
}

void photosChanged() {                    // web upload/delete: look again
  for (int8_t &s : slotHas) s = -1;
  ++version;
}

uint32_t photosVersion() { return version; }
bool photosFsReady() { return fsReady(); }
