// Yahoo Finance symbol helpers. Pure C++ (no Arduino): unit-tested on the computer.
#include "modules/finance/finance_symbols.h"
#include <stdio.h>
#include <string.h>

// "aapl" -> "AAPL", " petr4.sa " -> "PETR4.SA". Allowed: A-Z 0-9 . - = ^
bool normalizeFinanceSymbol(const char *in, char *out, size_t outSize) {
  if (!in || !out || outSize < 2) return false;
  size_t n = 0;
  for (const char *p = in; *p; ++p) {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '\r') continue;
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '=' || c == '^';
    if (!ok || n >= outSize - 1) return false;
    out[n++] = c;
  }
  out[n] = 0;
  return n > 0;
}

// Short title for a card: "USDBRL=X" -> "USD/BRL", "BTC-USD" -> "BTC/USD",
// "PETR4.SA" -> "PETR4", "^BVSP" -> "BVSP", "AAPL" -> "AAPL".
void financeDisplayName(const char *symbol, char *out, size_t outSize) {
  const size_t len = strlen(symbol);
  if (len == 8 && !strcmp(symbol + 6, "=X")) { snprintf(out, outSize, "%.3s/%.3s", symbol, symbol + 3); return; }
  if (len == 5 && !strcmp(symbol + 3, "=X")) { snprintf(out, outSize, "USD/%.3s", symbol); return; }
  const char *s = symbol[0] == '^' ? symbol + 1 : symbol;
  snprintf(out, outSize, "%s", s);
  char *dot = strrchr(out, '.');
  if (dot && dot != out) *dot = 0;
  for (char *p = out; *p; ++p) if (*p == '-') *p = '/';
}

const char *currencySymbol(const char *currency) {
  if (!currency || !*currency) return "";
  if (!strcmp(currency, "USD")) return "$";
  if (!strcmp(currency, "BRL")) return "R$";
  if (!strcmp(currency, "EUR")) return "€";
  if (!strcmp(currency, "GBP") || !strcmp(currency, "GBp")) return "£";
  if (!strcmp(currency, "JPY")) return "¥";
  return currency;
}

