#pragma once
// Gato do fundo do Calendário (escolhido na página web, card "Calendário").
// Lógica pura: testada no computador (test_native_holidays).
#include <stdint.h>

enum CalendarCat : uint8_t { CAL_CAT_FRAJOLA = 0, CAL_CAT_TRICOLOR, CAL_CAT_LARANJA, CAL_CAT_CINZA,
                             CAL_CAT_RANDOM,          // opção: um gato diferente a cada abertura
                             CAL_CAT_COUNT };
static constexpr uint8_t CAL_CAT_ART_COUNT = CAL_CAT_RANDOM;   // gatos com arte (0..3)

const char *calendarCatName(uint8_t cat);        // "Frajola", "Tricolor", "Laranja", "Cinza rajado", "Aleatório"
// Gato a mostrar ao abrir a tela: a escolha, ou (aleatório) um sorteado de rnd que
// não repete o anterior. rnd = qualquer número aleatório (esp_random()).
uint8_t calendarCatResolve(uint8_t setting, uint8_t previous, uint32_t rnd);
uint8_t calendarCatFromArg(const char *arg);     // texto do formulário -> gato (inválido: frajola)

// Board side (calendar_web.cpp): valor salvo em NVS ("calCat").
uint8_t calendarCat();
void setCalendarCat(uint8_t cat);
uint32_t calendarCatVersion();                   // muda quando o gato é trocado
