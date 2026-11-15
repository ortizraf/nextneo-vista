#pragma once
// Satellite orbits for the Sky screen: TLE parsing + SGP4 (near-Earth, WGS-72).
// Pure C++ (no Arduino): unit-tested on the computer (test/test_native_sky).
#include <stdint.h>

namespace sky {

// ---------------------------------------------------------------- satellites (CelesTrak TLE + SGP4)
// Downloaded only while this screen is open, at most every 6 h (CelesTrak asks for no more than
// that): ISS + Tiangong (group "stations"), the ~150 brightest ("visual") and the Starlinks
// launched in the last 30 days ("trains"). Positions are computed here with SGP4 (near-Earth
// model, WGS-72), so the dots move in real time without more network.
enum { SAT_ISS, SAT_CSS, SAT_VISUAL, SAT_STARLINK };
struct Sat {
  char name[22]; uint8_t kind; bool up;                   // up: near/above the horizon (checked in turns)
  double epochJd, no, ecco, inclo, nodeo, argpo, mo, bstar;
  // SGP4 init constants
  bool isimp; double aycof, con41, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao, t2cof, t3cof,
      t4cof, t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf;
};
constexpr int MAX_SATS = 700;
constexpr double RE_KM = 6378.135;                    // Earth radius used by SGP4 (WGS-72)

void sgp4Init(Sat &s);                                      // after the TLE fields are set
bool sgp4(const Sat &s, double minutesSinceEpoch, double r[3]);   // TEME position (km)
// Appends the satellites of one CelesTrak TLE text to out[n..cap). mode: 0 = stations
// (ISS + Tiangong only), 1 = visual (without those two), 2 = STARLINK only. Returns new n.
int parseTle(const char *txt, Sat *out, int n, int cap, uint8_t mode);
// Same for CelesTrak FORMAT=csv (OMM mean elements). Needed for catalog numbers >= 100000,
// which TLE cannot hold (new launches). Columns are found by name in the header.
int parseOmmCsv(const char *txt, Sat *out, int n, int cap, uint8_t mode);

}  // namespace sky
