#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "modules/f1/f1_schedule.h"

time_t f1ResultDayEnd(time_t raceStartUtc, long utcOffsetS) {
  const long long local = (long long)raceStartUtc + F1_RACE_DURATION_S + utcOffsetS;   // finish, local seconds
  long long dayStart = local - (local % 86400);
  if (local % 86400 < 0) dayStart -= 86400;                // (pre-1970 safety)
  return (time_t)(dayStart + 86400 - utcOffsetS);
}

F1RacePhase f1RacePhase(time_t raceStartUtc, time_t now, long utcOffsetS) {
  if (!raceStartUtc || now < raceStartUtc + F1_RACE_DURATION_S) return F1_UPCOMING;
  return now < f1ResultDayEnd(raceStartUtc, utcOffsetS) ? F1_RESULT_DAY : F1_DONE;
}

const char *f1ResultGap(const char *time, const char *status, bool en, char *out, int outLen) {
  if (!out || outLen <= 0) return out;
  out[0] = 0;
  if (time && *time) { snprintf(out, outLen, "%s", time); return out; }
  const char *st = status ? status : "";
  if (st[0] == '+') {                                          // "+1 Lap", "+2 Laps"
    const int n = atoi(st + 1);
    if (en) snprintf(out, outLen, "+%d lap%s", n, n == 1 ? "" : "s");
    else snprintf(out, outLen, "+%d volta%s", n, n == 1 ? "" : "s");
  } else if (!strcmp(st, "Lapped")) snprintf(out, outLen, en ? "+1 lap" : "+1 volta");
  else if (!strcmp(st, "Finished") || !st[0]) out[0] = 0;
  else if (!strcmp(st, "Did not start")) snprintf(out, outLen, en ? "DNS" : "n\xC3\xA3o largou");
  else if (!strcmp(st, "Disqualified")) snprintf(out, outLen, en ? "DSQ" : "desclassif.");
  else snprintf(out, outLen, en ? "DNF" : "abandonou");
  return out;
}
