// Internationalization tables. Pure C++ (no Arduino): unit-tested on the computer.
#include "core/i18n.h"

uint8_t gLang = LANG_PT;

static const char *pick(const char *const pt[], const char *const en[], int i, int n) {
  if (i < 0 || i >= n) return "";
  return gLang == LANG_EN ? en[i] : pt[i];
}

const char *langName(uint8_t lang) { return lang == LANG_EN ? "English" : "Portugu\xC3\xAAs"; }
const char *langCode(uint8_t lang) { return lang == LANG_EN ? "en" : "pt"; }

static const char *const kWdShortPt[7] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "S\xC3\xA1" "b"};
static const char *const kWdShortEn[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const kWdLongPt[7] = {"Domingo", "Segunda", "Ter\xC3\xA7" "a", "Quarta", "Quinta", "Sexta", "S\xC3\xA1" "bado"};
static const char *const kWdLongEn[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const kWdUpperPt[7] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
static const char *const kWdUpperEn[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const kMonShortPt[12] = {"Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez"};
static const char *const kMonShortEn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const kMonLongPt[12] = {"janeiro", "fevereiro", "mar\xC3\xA7o", "abril", "maio", "junho", "julho",
                                           "agosto", "setembro", "outubro", "novembro", "dezembro"};
static const char *const kMonLongEn[12] = {"January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December"};

const char *weekdayShort(int w) { return pick(kWdShortPt, kWdShortEn, w, 7); }
const char *weekdayLong(int w) { return pick(kWdLongPt, kWdLongEn, w, 7); }
const char *weekdayUpper(int w) { return pick(kWdUpperPt, kWdUpperEn, w, 7); }
const char *monthShort(int m) { return pick(kMonShortPt, kMonShortEn, m, 12); }
const char *monthLong(int m) { return pick(kMonLongPt, kMonLongEn, m, 12); }
