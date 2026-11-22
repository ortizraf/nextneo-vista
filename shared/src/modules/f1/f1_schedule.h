#pragma once
// Regra de exibição da corrida (lógica pura, testada no computador: test_native_f1).
// Depois da bandeirada (largada + 2 h) a tela mostra o RESULTADO até 23h59 do dia local
// em que a corrida terminou; à meia-noite passa para a próxima corrida.
#include <time.h>

static constexpr long F1_RACE_DURATION_S = 7200;          // largada -> bandeirada (estimado)

enum F1RacePhase { F1_UPCOMING, F1_RESULT_DAY, F1_DONE };

// Fim (exclusivo, epoch UTC) do dia local em que a corrida termina.
time_t f1ResultDayEnd(time_t raceStartUtc, long utcOffsetS);
// UPCOMING até a bandeirada, RESULT_DAY até 23h59:59 local, depois DONE.
F1RacePhase f1RacePhase(time_t raceStartUtc, time_t now, long utcOffsetS);

// Coluna "tempo / diferença" do resultado: tempo do vencedor ou "+0.639"; sem tempo,
// pelo status da API: "+1 Lap" -> "+1 volta", "Lapped" -> "+1 volta", "Retired"/outros ->
// "abandonou", "Did not start" -> "não largou", "Disqualified" -> "desclassif.".
// en = textos em inglês. Retorna out.
const char *f1ResultGap(const char *time, const char *status, bool en, char *out, int outLen);
