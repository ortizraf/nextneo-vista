// TLE parsing + SGP4 for the Sky screen. Pure C++ (no Arduino): unit-tested on the computer.
#include "modules/sky/sky_orbit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sky {

static constexpr double RAD = M_PI / 180.0;

static constexpr double XKE = 0.0743669161331734, J2 = 0.001082616, J3OJ2 = -0.00000253881 / 0.001082616,
                        J4 = -0.00000165597, K_2PI = 2 * M_PI;

void sgp4Init(Sat &s) {
  const double x2o3 = 2.0 / 3.0, cosio = cos(s.inclo), cosio2 = cosio * cosio, eccsq = s.ecco * s.ecco;
  const double omeosq = 1 - eccsq, rteosq = sqrt(omeosq);
  const double ak = pow(XKE / s.no, x2o3), d1 = 0.75 * J2 * (3 * cosio2 - 1) / (rteosq * omeosq);
  double del = d1 / (ak * ak);
  const double adel = ak * (1 - del * del - del * (1.0 / 3.0 + 134 * del * del / 81));
  del = d1 / (adel * adel);
  s.no = s.no / (1 + del);
  const double ao = pow(XKE / s.no, x2o3), sinio = sin(s.inclo), po = ao * omeosq, con42 = 1 - 5 * cosio2;
  s.con41 = -con42 - cosio2 - cosio2;
  const double posq = po * po, rp = ao * (1 - s.ecco);
  s.isimp = rp < (220 / RE_KM + 1);
  double sfour = 78 / RE_KM + 1, qzms24 = pow((120 - 78) / RE_KM, 4);
  const double perige = (rp - 1) * RE_KM;
  if (perige < 156) {
    sfour = perige < 98 ? 20 : perige - 78;
    qzms24 = pow((120 - sfour) / RE_KM, 4); sfour = sfour / RE_KM + 1;
  }
  const double pinvsq = 1 / posq, tsi = 1 / (ao - sfour);
  s.eta = ao * s.ecco * tsi;
  const double etasq = s.eta * s.eta, eeta = s.ecco * s.eta, psisq = fabs(1 - etasq), coef = qzms24 * pow(tsi, 4),
               coef1 = coef / pow(psisq, 3.5);
  const double cc2 = coef1 * s.no * (ao * (1 + 1.5 * etasq + eeta * (4 + etasq)) +
                                     0.375 * J2 * tsi / psisq * s.con41 * (8 + 3 * etasq * (8 + etasq)));
  s.cc1 = s.bstar * cc2;
  const double cc3 = s.ecco > 1e-4 ? -2 * coef * tsi * J3OJ2 * s.no * sinio / s.ecco : 0;
  s.x1mth2 = 1 - cosio2;
  s.cc4 = 2 * s.no * coef1 * ao * omeosq *
          (s.eta * (2 + 0.5 * etasq) + s.ecco * (0.5 + 2 * etasq) -
           J2 * tsi / (ao * psisq) * (-3 * s.con41 * (1 - 2 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                                      0.75 * s.x1mth2 * (2 * etasq - eeta * (1 + etasq)) * cos(2 * s.argpo)));
  s.cc5 = 2 * coef1 * ao * omeosq * (1 + 2.75 * (etasq + eeta) + eeta * etasq);
  const double cosio4 = cosio2 * cosio2, temp1 = 1.5 * J2 * pinvsq * s.no, temp2 = 0.5 * temp1 * J2 * pinvsq,
               temp3 = -0.46875 * J4 * pinvsq * pinvsq * s.no;
  s.mdot = s.no + 0.5 * temp1 * rteosq * s.con41 + 0.0625 * temp2 * rteosq * (13 - 78 * cosio2 + 137 * cosio4);
  s.argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7 - 114 * cosio2 + 395 * cosio4) + temp3 * (3 - 36 * cosio2 + 49 * cosio4);
  const double xhdot1 = -temp1 * cosio;
  s.nodedot = xhdot1 + (0.5 * temp2 * (4 - 19 * cosio2) + 2 * temp3 * (3 - 7 * cosio2)) * cosio;
  s.omgcof = s.bstar * cc3 * cos(s.argpo);
  s.xmcof = s.ecco > 1e-4 ? -x2o3 * coef * s.bstar / eeta : 0;
  s.nodecf = 3.5 * omeosq * xhdot1 * s.cc1;
  s.t2cof = 1.5 * s.cc1;
  s.xlcof = -0.25 * J3OJ2 * sinio * (3 + 5 * cosio) / (fabs(cosio + 1) > 1.5e-12 ? 1 + cosio : 1.5e-12);
  s.aycof = -0.5 * J3OJ2 * sinio;
  s.delmo = pow(1 + s.eta * cos(s.mo), 3);
  s.sinmao = sin(s.mo);
  s.x7thm1 = 7 * cosio2 - 1;
  s.d2 = s.d3 = s.d4 = s.t3cof = s.t4cof = s.t5cof = 0;
  if (!s.isimp) {
    const double cc1sq = s.cc1 * s.cc1;
    s.d2 = 4 * ao * tsi * cc1sq;
    const double temp = s.d2 * tsi * s.cc1 / 3;
    s.d3 = (17 * ao + sfour) * temp;
    s.d4 = 0.5 * temp * ao * tsi * (221 * ao + 31 * sfour) * s.cc1;
    s.t3cof = s.d2 + 2 * cc1sq;
    s.t4cof = 0.25 * (3 * s.d3 + s.cc1 * (12 * s.d2 + 10 * cc1sq));
    s.t5cof = 0.2 * (3 * s.d4 + 12 * s.cc1 * s.d3 + 6 * s.d2 * s.d2 + 15 * cc1sq * (2 * s.d2 + cc1sq));
  }
}

// Position (km, TEME frame) t minutes after the TLE epoch. false if decayed / invalid.
bool sgp4(const Sat &s, double t, double r[3]) {
  const double xmdf = s.mo + s.mdot * t, argpdf = s.argpo + s.argpdot * t, nodedf = s.nodeo + s.nodedot * t, t2 = t * t;
  double argpm = argpdf, mm = xmdf, nodem = nodedf + s.nodecf * t2;
  double tempa = 1 - s.cc1 * t, tempe = s.bstar * s.cc4 * t, templ = s.t2cof * t2;
  if (!s.isimp) {
    const double delomg = s.omgcof * t, dm = 1 + s.eta * cos(xmdf);
    const double delm = s.xmcof * (dm * dm * dm - s.delmo), temp = delomg + delm, t3 = t2 * t, t4 = t3 * t;
    mm = xmdf + temp; argpm = argpdf - temp;
    tempa -= s.d2 * t2 + s.d3 * t3 + s.d4 * t4;
    tempe += s.bstar * s.cc5 * (sin(mm) - s.sinmao);
    templ += s.t3cof * t3 + t4 * (s.t4cof + t * s.t5cof);
  }
  const double am = pow(XKE / s.no, 2.0 / 3.0) * tempa * tempa;
  double em = s.ecco - tempe;
  if (em >= 1 || em < -0.001 || am < 0.95) return false;
  if (em < 1e-6) em = 1e-6;
  mm += s.no * templ;
  double xlm = fmod(mm + argpm + nodem, K_2PI);
  nodem = fmod(nodem, K_2PI); argpm = fmod(argpm, K_2PI);
  mm = fmod(xlm - argpm - nodem, K_2PI);
  const double sinip = sin(s.inclo), cosip = cos(s.inclo);
  const double axnl = em * cos(argpm), temp0 = 1 / (am * (1 - em * em)), aynl = em * sin(argpm) + temp0 * s.aycof;
  const double xl = mm + argpm + nodem + temp0 * s.xlcof * axnl;
  const double u = fmod(xl - nodem, K_2PI);
  double eo1 = u, tem5 = 9999, sineo1 = 0, coseo1 = 1;
  for (int k = 0; fabs(tem5) >= 1e-12 && k < 10; ++k) {
    sineo1 = sin(eo1); coseo1 = cos(eo1);
    tem5 = 1 - coseo1 * axnl - sineo1 * aynl;
    tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
    if (fabs(tem5) >= 0.95) tem5 = tem5 > 0 ? 0.95 : -0.95;
    eo1 += tem5;
  }
  const double ecose = axnl * coseo1 + aynl * sineo1, esine = axnl * sineo1 - aynl * coseo1;
  const double el2 = axnl * axnl + aynl * aynl, pl = am * (1 - el2);
  if (pl < 0) return false;
  const double rl = am * (1 - ecose), betal = sqrt(1 - el2), temp = esine / (1 + betal);
  const double sinu = am / rl * (sineo1 - aynl - axnl * temp), cosu = am / rl * (coseo1 - axnl + aynl * temp);
  double su = atan2(sinu, cosu);
  const double sin2u = (cosu + cosu) * sinu, cos2u = 1 - 2 * sinu * sinu, tp = 1 / pl, temp1 = 0.5 * J2 * tp, temp2 = temp1 * tp;
  const double mrt = rl * (1 - 1.5 * temp2 * betal * s.con41) + 0.5 * temp1 * s.x1mth2 * cos2u;
  if (mrt < 1) return false;
  su -= 0.25 * temp2 * s.x7thm1 * sin2u;
  const double xnode = nodem + 1.5 * temp2 * cosip * sin2u, xinc = s.inclo + 1.5 * temp2 * cosip * sinip * cos2u;
  const double sinsu = sin(su), cossu = cos(su), snod = sin(xnode), cnod = cos(xnode), sini = sin(xinc), cosi = cos(xinc);
  const double xmx = -snod * cosi, xmy = cnod * cosi;
  r[0] = mrt * (xmx * sinsu + cnod * cossu) * RE_KM;
  r[1] = mrt * (xmy * sinsu + snod * cossu) * RE_KM;
  r[2] = mrt * (sini * sinsu) * RE_KM;
  return true;
}

// ---- TLE download + parsing (network task)
static double tleField(const char *l, int a, int b) { char t[24]; int n = b - a + 1; memcpy(t, l + a, n); t[n] = 0; return atof(t); }
static double tleExp(const char *l, int a) {             // " 12345-3" -> 0.12345e-3
  char m[8]; memcpy(m, l + a + 1, 5); m[5] = 0;
  const char ex[3] = {l[a + 6], l[a + 7], 0};
  const double v = atof(m) * 1e-5 * pow(10.0, atoi(ex));
  return l[a] == '-' ? -v : v;
}
static double jdOfYearDay(int yy, double day) {
  const int y = yy < 57 ? 2000 + yy : 1900 + yy;
  const long a = (14 - 1) / 12, Y = y + 4800 - a, m = 1 + 12 * a - 3;           // Jan 1 -> JDN
  const long jdn = 1 + (153 * m + 2) / 5 + 365 * Y + Y / 4 - Y / 100 + Y / 400 - 32045;
  return jdn - 0.5 + (day - 1);
}
int parseTle(const char *txt, Sat *out, int n, int cap, uint8_t mode) {
  // mode: 0 = stations (ISS + Tiangong only), 1 = visual, 2 = last-30-days (Starlink only)
  const char *p = txt;
  char l0[80], l1[80], l2[80];
  auto getLine = [&](char *dst) -> bool {
    if (!*p) return false;
    int k = 0;
    while (*p && *p != '\n') { if (*p != '\r' && k < 79) dst[k++] = *p; ++p; }
    if (*p) ++p;
    dst[k] = 0; return true;
  };
  while (n < cap && getLine(l0)) {
    if (l0[0] == '1' && l0[1] == ' ') continue;
    if (!getLine(l1) || !getLine(l2)) break;
    if (l1[0] != '1' || l2[0] != '2' || strlen(l1) < 69 || strlen(l2) < 63) continue;
    const int cat = (int)tleField(l1, 2, 6);
    const double revDay = tleField(l2, 52, 62);
    if (revDay < 6.4) continue;                           // deep space (GEO, Molniya...): skip
    uint8_t kind = SAT_VISUAL;
    if (mode == 0) { if (cat == 25544) kind = SAT_ISS; else if (cat == 48274) kind = SAT_CSS; else continue; }
    if (mode == 2) { if (strncmp(l0, "STARLINK", 8)) continue; kind = SAT_STARLINK; }
    Sat &s = out[n];
    memset(&s, 0, sizeof s);
    int e = strlen(l0); while (e > 0 && l0[e - 1] == ' ') --e; l0[e] = 0;
    snprintf(s.name, sizeof s.name, "%s", kind == SAT_ISS ? "ISS" : kind == SAT_CSS ? "Tiangong" : l0);
    if (mode == 1 && (cat == 25544 || cat == 48274)) continue;   // already from "stations"
    s.kind = kind;
    s.epochJd = jdOfYearDay((int)tleField(l1, 18, 19), tleField(l1, 20, 31));
    s.bstar = tleExp(l1, 53);
    s.inclo = tleField(l2, 8, 15) * RAD; s.nodeo = tleField(l2, 17, 24) * RAD;
    char ec[12] = "0."; memcpy(ec + 2, l2 + 26, 7); ec[9] = 0; s.ecco = atof(ec);
    s.argpo = tleField(l2, 34, 41) * RAD; s.mo = tleField(l2, 43, 50) * RAD;
    s.no = revDay * K_2PI / 1440.0;
    s.up = true;
    sgp4Init(s);
    ++n;
  }
  return n;
}



// ---------------------------------------------------------------- OMM CSV (CelesTrak FORMAT=csv)
// TLE cannot hold catalog numbers >= 100000, so CelesTrak returns no TLE for newly
// cataloged objects (e.g. "last-30-days" -> 404 "No GP data found"). CSV carries the same
// mean elements (OMM) for every object:
// OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,
// ARG_OF_PERICENTER,MEAN_ANOMALY,...,NORAD_CAT_ID,...,BSTAR,... (columns found by name).
static double jdOfIso(const char *s) {                      // "2026-11-14T01:47:00.575808" -> JD
  int y, mo, d, h = 0, mi = 0; double sec = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
  const long a = (14 - mo) / 12, Y = y + 4800 - a, m = mo + 12 * a - 3;
  const long jdn = d + (153 * m + 2) / 5 + 365 * Y + Y / 4 - Y / 100 + Y / 400 - 32045;
  return jdn - 0.5 + (h + mi / 60.0 + sec / 3600.0) / 24.0;
}

int parseOmmCsv(const char *txt, Sat *out, int n, int cap, uint8_t mode) {
  enum { C_NAME, C_EPOCH, C_MM, C_ECC, C_INC, C_RAAN, C_ARGP, C_MA, C_CAT, C_BSTAR, C_COUNT };
  static const char *const kCols[C_COUNT] = {"OBJECT_NAME", "EPOCH", "MEAN_MOTION", "ECCENTRICITY", "INCLINATION",
                                             "RA_OF_ASC_NODE", "ARG_OF_PERICENTER", "MEAN_ANOMALY", "NORAD_CAT_ID", "BSTAR"};
  int col[C_COUNT];
  for (int &c : col) c = -1;
  const char *p = txt;
  char line[320];
  auto getLine = [&]() -> bool {
    if (!*p) return false;
    int k = 0;
    while (*p && *p != '\n') { if (*p != '\r' && k < (int)sizeof line - 1) line[k++] = *p; ++p; }
    if (*p) ++p;
    line[k] = 0; return true;
  };
  auto split = [](char *l, char **f, int max) {             // in place, no quoted commas in these fields
    int nf = 0; f[nf++] = l;
    for (char *c = l; *c && nf < max; ++c) if (*c == ',') { *c = 0; f[nf++] = c + 1; }
    return nf;
  };
  char *f[24];
  if (!getLine()) return n;
  const int nh = split(line, f, 24);
  for (int i = 0; i < nh; ++i)
    for (int c = 0; c < C_COUNT; ++c) if (!strcmp(f[i], kCols[c])) col[c] = i;
  for (int c = 0; c < C_COUNT; ++c) if (col[c] < 0) return n;   // not an OMM CSV (error page...)
  while (n < cap && getLine()) {
    const int nf = split(line, f, 24);
    bool ok = true;
    for (int c = 0; c < C_COUNT; ++c) if (col[c] >= nf) ok = false;
    if (!ok) continue;
    const long cat = atol(f[col[C_CAT]]);
    const double revDay = atof(f[col[C_MM]]);
    if (revDay < 6.4) continue;                             // deep space (GEO, Molniya...): skip
    const char *name = f[col[C_NAME]];
    uint8_t kind = SAT_VISUAL;
    if (mode == 0) { if (cat == 25544) kind = SAT_ISS; else if (cat == 48274) kind = SAT_CSS; else continue; }
    if (mode == 1 && (cat == 25544 || cat == 48274)) continue;   // already from "stations"
    if (mode == 2) { if (strncmp(name, "STARLINK", 8)) continue; kind = SAT_STARLINK; }
    Sat &s = out[n];
    memset(&s, 0, sizeof s);
    snprintf(s.name, sizeof s.name, "%s", kind == SAT_ISS ? "ISS" : kind == SAT_CSS ? "Tiangong" : name);
    s.kind = kind;
    s.epochJd = jdOfIso(f[col[C_EPOCH]]);
    if (s.epochJd <= 0) continue;
    s.bstar = atof(f[col[C_BSTAR]]);
    s.inclo = atof(f[col[C_INC]]) * RAD; s.nodeo = atof(f[col[C_RAAN]]) * RAD;
    s.ecco = atof(f[col[C_ECC]]);
    s.argpo = atof(f[col[C_ARGP]]) * RAD; s.mo = atof(f[col[C_MA]]) * RAD;
    s.no = revDay * K_2PI / 1440.0;
    s.up = true;
    sgp4Init(s);
    ++n;
  }
  return n;
}

}  // namespace sky
