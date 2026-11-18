#pragma once
// Feriados nacionais do Brasil (BrasilAPI), por ano, com cache em NVS.
// A API só é chamada com a tela Calendário ativa (job dessa tela; ao sair, cancela).
// UI thread: holidaysWant() / holidayIs() / holidaysVersion() / holidaysNeedFetch().
// Network task: holidaysFetch().
#include <stdint.h>

void holidaysWant(int year);              // ano exibido: carrega o cache (NVS) e marca se precisa buscar
bool holidaysNeedFetch();                 // Calendário ativo + Wi-Fi + ano pendente
bool holidayIs(int year, int month, int day);   // month 1-12
uint32_t holidaysVersion();               // muda quando chegam dados novos
bool holidaysFetch();                     // NetJob da tela Calendário
