#pragma once
// Pure C++ (no Arduino/LVGL): unit-tested on the computer (test/test_native_logic).
#include <stddef.h>

// Our fonts lack some glyphs (em dash, curly quotes, ...): returns `in` when it is
// already safe, otherwise a copy in `out` with replacements ("-", "'", "...", "?").
const char *uiSafeText(const char *in, char *out, size_t n);
