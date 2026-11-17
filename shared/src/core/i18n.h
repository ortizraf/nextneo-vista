#pragma once
// Internationalization: Portuguese (default) and English.
// Pure C++ (no Arduino): unit-tested on the computer (test/test_native_i18n).
//
//   setLabelIfChanged(lbl, T("Chuva", "Rain"));
//
// The language is loaded once at boot (prefs "lang") before the screens are built; changing
// it saves the choice and restarts the board, so every fixed text is created again in the
// new language. Logs stay in Portuguese.
#include <stdint.h>

enum Lang : uint8_t { LANG_PT = 0, LANG_EN = 1, LANG_COUNT };

extern uint8_t gLang;                         // current language (LANG_PT by default)

// Text in the current language. Both arguments are string literals (or live as long).
inline const char *T(const char *pt, const char *en) { return gLang == LANG_EN ? en : pt; }

const char *langName(uint8_t lang);           // "Português" / "English" (in that language)
const char *langCode(uint8_t lang);           // "pt" / "en"

// Calendar names in the current language (index 0 = Sunday / January).
const char *weekdayShort(int wday);           // "Dom" / "Sun"
const char *weekdayLong(int wday);            // "Domingo" / "Sunday"
const char *weekdayUpper(int wday);           // "DOM" / "SUN" (menu calendar icon)
const char *monthShort(int mon);              // "Jan" / "Jan"
const char *monthLong(int mon);               // "janeiro" / "January"
