#pragma once
// Finance module: Yahoo Finance quotes for a configurable list of assets.
#include "core/app_core.h"
#include "modules/finance/finance_symbols.h"

// One asset on the Finance screen: Yahoo Finance symbol + optional caption.
// Examples: USDBRL=X (dólar), EURBRL=X, BTC-USD, AAPL, PETR4.SA, ^BVSP (Ibovespa).
static constexpr uint8_t FINANCE_MAX_ASSETS = 8;
static constexpr uint8_t FINANCE_HISTORY = 48;     // card sparkline: whole trading day downsampled
struct FinanceAsset {
  char symbol[16] = {};
  char label[32] = {};
};

// Quotes published by the network task for the Finance screen (and the web page).
// Always read/written under mtx.
struct FinanceData {
  FinanceAsset assets[FINANCE_MAX_ASSETS];
  uint8_t assetCount = 0;
  uint32_t configVersion = 0;          // bumped when the asset list changes
  char currency[FINANCE_MAX_ASSETS][4] = {};
  float price[FINANCE_MAX_ASSETS] = {};
  float previousClose[FINANCE_MAX_ASSETS] = {}, changePct[FINANCE_MAX_ASSETS] = {};
  bool haveChange[FINANCE_MAX_ASSETS] = {};
  float history[FINANCE_MAX_ASSETS][FINANCE_HISTORY] = {};
  uint8_t historyCount[FINANCE_MAX_ASSETS] = {};
  bool havePrice[FINANCE_MAX_ASSETS] = {};
  bool refreshing = false;
  uint8_t successCount = 0;
  bool apiKnown[FINANCE_MAX_ASSETS] = {};
  bool apiOk[FINANCE_MAX_ASSETS] = {};
  uint32_t lastSuccessMs = 0;
  uint32_t version = 0;                // bumped on every refresh
  uint32_t quoteEpoch[FINANCE_MAX_ASSETS] = {};
};
extern FinanceData fin;
extern SemaphoreHandle_t finMtx;             // guards fin, financeDetail(Want) (not the app-wide mtx)

// ---- asset list (finance_assets.cpp). UI thread / web handler.
void loadFinanceAssets();
bool setFinanceAssets(const FinanceAsset *assets, uint8_t count);

// ---- detail chart (long press on a card). UI writes financeDetailWant, the network
// task answers in financeDetail (both under mtx). Only fetched while Finance is active.
static constexpr uint8_t FIN_RANGES = 5;              // 1D 5D 1M 6M 1A
static constexpr int FIN_DETAIL_POINTS = 120;
enum FinMarketState : uint8_t { FIN_MKT_UNKNOWN = 0, FIN_MKT_OPEN, FIN_MKT_CLOSED, FIN_MKT_PRE, FIN_MKT_POST, FIN_MKT_24H };
struct FinanceDetailRequest {
  char symbol[16] = {};
  uint8_t range = 0;
};
struct FinanceDetail {
  char symbol[16] = {};
  uint8_t range = 0;
  bool ok = false;                 // false: request answered but no data (error / blocked)
  uint32_t version = 0;
  uint32_t fetchedMs = 0;
  float price = 0, open = NAN, dayHigh = NAN, dayLow = NAN, wk52High = NAN, wk52Low = NAN;
  float rangeRef = NAN;            // close before the range start (period change baseline)
  float periodHigh = 0, periodLow = 0;
  float series[FIN_DETAIL_POINTS] = {};
  uint8_t count = 0;
  uint32_t t0 = 0, t1 = 0;         // first / last candle (epoch)
  uint8_t market = FIN_MKT_UNKNOWN;
  char currency[4] = {};
};
extern FinanceDetail financeDetail;
extern FinanceDetailRequest financeDetailWant;
extern volatile int8_t financeSingleIndex;   // asset refreshed by the FIN_JOB_ONE job

// ---- network task (finance_fetch.cpp)
bool financeFetchAll();
bool financeFetchOne();                      // asset financeSingleIndex
bool financeFetchDetail();                   // answers financeDetailWant
void financeReleaseConnection();             // drop the keep-alive TLS session
