// Text helpers for the UI fonts. Pure C++ (no Arduino/LVGL): unit-tested on the computer.
#include "core/text_utils.h"
#include <stdint.h>
#include <string.h>

// Our fonts (lv_font_weather_14/20) have ASCII, Latin-1, Latin Extended-A, "•" and "€";
// LVGL's symbols (U+F000..F8FF) come from the Montserrat fallback. Anything else (an em
// dash, curly quotes from an API, ...) would show as an empty box, so it is replaced.
static bool glyphOk(uint32_t c) {
  return c < 0x80 || (c >= 0xA0 && c <= 0x17F) || c == 0x2022 || c == 0x20AC || (c >= 0xF000 && c <= 0xF8FF);
}
const char *uiSafeText(const char *in, char *out, size_t n) {
  const uint8_t *p = (const uint8_t *)in;
  bool clean = true;
  for (const uint8_t *q = p; *q; ++q) if (*q >= 0xE0) { clean = false; break; }   // 3/4-byte chars only
  if (clean || n < 4) return in;
  size_t o = 0;
  while (*p && o + 4 < n) {
    uint32_t c = *p; int len = 1;
    if (c >= 0xF0 && p[1] && p[2] && p[3]) { c = ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); len = 4; }
    else if (c >= 0xE0 && p[1] && p[2]) { c = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); len = 3; }
    else if (c >= 0xC0 && p[1]) len = 2;
    if (len < 3 || glyphOk(c)) { memcpy(out + o, p, len); o += len; }
    else if (c >= 0x2010 && c <= 0x2015) out[o++] = '-';
    else if (c >= 0x2018 && c <= 0x201B) out[o++] = '\'';
    else if (c >= 0x201C && c <= 0x201F) out[o++] = '"';
    else if (c == 0x2026) { memcpy(out + o, "...", 3); o += 3; }
    else if (c == 0x2039) out[o++] = '<';
    else if (c == 0x203A) out[o++] = '>';
    else out[o++] = '?';
    p += len;
  }
  out[o] = 0;
  return out;
}

