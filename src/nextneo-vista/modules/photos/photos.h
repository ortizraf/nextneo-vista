#pragma once
// Photos module: slideshow of photos uploaded on the web page (/photos), on LittleFS.
#include <stddef.h>
#include <stdint.h>

static constexpr int PHOTO_SLOTS = 8;
static constexpr uint32_t PHOTO_SLIDE_MS = 30000UL;   // one photo every 30 s
bool photosFsReady();                                 // LittleFS mounted (also used by F1 / Futebol)
bool photoExists(int slot);
void photoPath(int slot, char *out, size_t n);
void photosChanged();                                // after an upload/delete on the web page
