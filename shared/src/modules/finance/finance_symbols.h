#pragma once
// Yahoo Finance symbol helpers. Pure C++ (no Arduino): unit-tested on the computer
// (test/test_native_logic).
#include <stddef.h>

bool normalizeFinanceSymbol(const char *in, char *out, size_t outSize);   // " petr4.sa " -> "PETR4.SA"
void financeDisplayName(const char *symbol, char *out, size_t outSize);   // "USDBRL=X" -> "USD/BRL"
const char *currencySymbol(const char *currency);                         // "BRL" -> "R$"
