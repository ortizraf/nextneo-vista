// Native suite (computer, no board): Yahoo Finance symbol helpers.
#include <unity.h>
#include "modules/finance/finance_symbols.h"

void setUp() {}
void tearDown() {}

static void test_normalize_uppercases_and_trims() {
  char out[16];
  TEST_ASSERT_TRUE(normalizeFinanceSymbol(" petr4.sa ", out, sizeof out));
  TEST_ASSERT_EQUAL_STRING("PETR4.SA", out);
  TEST_ASSERT_TRUE(normalizeFinanceSymbol("usdbrl=x", out, sizeof out));
  TEST_ASSERT_EQUAL_STRING("USDBRL=X", out);
  TEST_ASSERT_TRUE(normalizeFinanceSymbol("^bvsp", out, sizeof out));
  TEST_ASSERT_EQUAL_STRING("^BVSP", out);
}

static void test_normalize_rejects_bad_input() {
  char out[16];
  TEST_ASSERT_FALSE(normalizeFinanceSymbol("", out, sizeof out));
  TEST_ASSERT_FALSE(normalizeFinanceSymbol("   ", out, sizeof out));
  TEST_ASSERT_FALSE(normalizeFinanceSymbol("AAPL!", out, sizeof out));      // invalid char
  TEST_ASSERT_FALSE(normalizeFinanceSymbol("ABCDEFGHIJKLMNOPQ", out, 8));   // too long
  TEST_ASSERT_FALSE(normalizeFinanceSymbol(nullptr, out, sizeof out));
}

static void test_display_name() {
  char out[16];
  financeDisplayName("USDBRL=X", out, sizeof out); TEST_ASSERT_EQUAL_STRING("USD/BRL", out);
  financeDisplayName("BRL=X", out, sizeof out);    TEST_ASSERT_EQUAL_STRING("USD/BRL", out);
  financeDisplayName("BTC-USD", out, sizeof out);  TEST_ASSERT_EQUAL_STRING("BTC/USD", out);
  financeDisplayName("PETR4.SA", out, sizeof out); TEST_ASSERT_EQUAL_STRING("PETR4", out);
  financeDisplayName("^BVSP", out, sizeof out);    TEST_ASSERT_EQUAL_STRING("BVSP", out);
  financeDisplayName("AAPL", out, sizeof out);     TEST_ASSERT_EQUAL_STRING("AAPL", out);
}

static void test_currency_symbol() {
  TEST_ASSERT_EQUAL_STRING("R$", currencySymbol("BRL"));
  TEST_ASSERT_EQUAL_STRING("$", currencySymbol("USD"));
  TEST_ASSERT_EQUAL_STRING("\xE2\x82\xAC", currencySymbol("EUR"));
  TEST_ASSERT_EQUAL_STRING("CHF", currencySymbol("CHF"));   // unknown: code itself
  TEST_ASSERT_EQUAL_STRING("", currencySymbol(""));
  TEST_ASSERT_EQUAL_STRING("", currencySymbol(nullptr));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_normalize_uppercases_and_trims);
  RUN_TEST(test_normalize_rejects_bad_input);
  RUN_TEST(test_display_name);
  RUN_TEST(test_currency_symbol);
  return UNITY_END();
}
