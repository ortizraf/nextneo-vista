#pragma once
// Finance module: Yahoo Finance quotes for a configurable list of assets.
#include "app_screens.h"
#include "modules/finance/finance_symbols.h"

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
extern volatile int8_t financeSingleIndex;   // asset refreshed by CMD_FINANCE_ONE

// ---- network task (finance_fetch.cpp)
bool financeFetchAll();
bool financeFetchOne();                      // asset financeSingleIndex
bool financeFetchDetail();                   // answers financeDetailWant
void financeReleaseConnection();             // drop the keep-alive TLS session
