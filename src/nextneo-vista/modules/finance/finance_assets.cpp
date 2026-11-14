// Finance asset list: which Yahoo Finance symbols the Finance screen shows.
// Saved in Preferences ("finAssets") as lines "SYMBOL|Legenda".
// Edited from the web page (http://IP-DO-APARELHO).
#include "app_screens.h"
#include "modules/finance/finance.h"
#include <string.h>

namespace {
const FinanceAsset kDefaults[] = {
  {"USDBRL=X", "Dólar para real"},
  {"EURBRL=X", "Euro para real"},
  {"BTC-USD", "Bitcoin em dólar"},
};

uint8_t parseAssets(const String &text, FinanceAsset *out) {
  uint8_t count = 0;
  int start = 0;
  while (start < (int)text.length() && count < FINANCE_MAX_ASSETS) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    start = end + 1;
    int bar = line.indexOf('|');
    String symbol = bar >= 0 ? line.substring(0, bar) : line;
    String label = bar >= 0 ? line.substring(bar + 1) : String();
    symbol.trim();
    label.trim();
    FinanceAsset asset;
    if (!normalizeFinanceSymbol(symbol.c_str(), asset.symbol, sizeof asset.symbol)) continue;
    snprintf(asset.label, sizeof asset.label, "%s", label.c_str());
    out[count++] = asset;
  }
  return count;
}

String serialize(const FinanceAsset *assets, uint8_t count) {
  String text;
  for (uint8_t i = 0; i < count; ++i) {
    text += assets[i].symbol;
    text += '|';
    text += assets[i].label;
    text += '\n';
  }
  return text;
}

// Publishes a new list: quotes of symbols that stay in the list are kept,
// new ones start empty.
void apply(const FinanceAsset *assets, uint8_t count) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  static Shared old;  // static: keeps the ~3 KB copy off the loop task stack
  old = sh;
  for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) {
    int from = -1;
    if (i < count)
      for (uint8_t j = 0; j < old.financeAssetCount; ++j)
        if (!strcmp(old.financeAssets[j].symbol, assets[i].symbol)) { from = j; break; }
    sh.financeAssets[i] = i < count ? assets[i] : FinanceAsset();
    if (from >= 0) {
      memcpy(sh.financeCurrency[i], old.financeCurrency[from], sizeof sh.financeCurrency[i]);
      sh.finance[i] = old.finance[from];
      sh.previousFinance[i] = old.previousFinance[from];
      sh.financeChange[i] = old.financeChange[from];
      sh.haveChange[i] = old.haveChange[from];
      memcpy(sh.history[i], old.history[from], sizeof sh.history[i]);
      sh.historyCount[i] = old.historyCount[from];
      sh.haveFinance[i] = old.haveFinance[from];
      sh.financeApiKnown[i] = old.financeApiKnown[from];
      sh.financeApiOk[i] = old.financeApiOk[from];
      sh.financeQuoteEpoch[i] = old.financeQuoteEpoch[from];
    } else {
      sh.financeCurrency[i][0] = 0;
      sh.finance[i] = sh.previousFinance[i] = sh.financeChange[i] = 0;
      sh.haveChange[i] = sh.haveFinance[i] = sh.financeApiKnown[i] = sh.financeApiOk[i] = false;
      sh.historyCount[i] = 0;
      sh.financeQuoteEpoch[i] = 0;
    }
  }
  sh.financeAssetCount = count;
  ++sh.financeConfigVersion;
  ++sh.financeVersion;
  xSemaphoreGive(mtx);
}
}  // namespace

void loadFinanceAssets() {
  FinanceAsset assets[FINANCE_MAX_ASSETS];
  uint8_t count = parseAssets(prefs.getString("finAssets", ""), assets);
  if (!count) {
    count = sizeof kDefaults / sizeof *kDefaults;
    for (uint8_t i = 0; i < count; ++i) assets[i] = kDefaults[i];
  }
  apply(assets, count);
}

bool setFinanceAssets(const FinanceAsset *input, uint8_t count) {
  if (count > FINANCE_MAX_ASSETS) count = FINANCE_MAX_ASSETS;
  FinanceAsset assets[FINANCE_MAX_ASSETS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < count; ++i) {
    FinanceAsset a;
    if (!normalizeFinanceSymbol(input[i].symbol, a.symbol, sizeof a.symbol)) continue;
    bool duplicate = false;
    for (uint8_t j = 0; j < n; ++j) if (!strcmp(assets[j].symbol, a.symbol)) duplicate = true;
    if (duplicate) continue;
    snprintf(a.label, sizeof a.label, "%s", input[i].label);
    for (char *p = a.label; *p; ++p) if (*p == '|' || *p == '\n' || *p == '\r') *p = ' ';
    assets[n++] = a;
  }
  if (!n) return false;
  prefs.putString("finAssets", serialize(assets, n));
  apply(assets, n);
  return true;
}
