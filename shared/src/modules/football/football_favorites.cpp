// Times favoritos do Futebol ("FLA,PAL", abreviações da ESPN), salvos em prefs "futFav".
// Editados na página web; a tela de Futebol de cada placa consulta footballIsFavorite()
// e redesenha quando footballFavoritesVersion() muda.
#include <Arduino.h>
#include <string.h>
#include "core/app_core.h"
#include "modules/football/football.h"

static char favorites[64] = "";
static bool loaded = false;
static uint32_t version = 1;

static void load() {
  if (loaded) return;
  loaded = true;
  if (prefs.isKey("futFav")) strlcpy(favorites, prefs.getString("futFav", "").c_str(), sizeof favorites);
}

bool footballIsFavorite(const char *abbr) {
  load();
  if (!abbr || !abbr[0] || !favorites[0]) return false;
  char key[8]; snprintf(key, sizeof key, "%s", abbr);
  const char *p = favorites;
  const size_t k = strlen(key);
  while ((p = strstr(p, key))) {
    if ((p == favorites || p[-1] == ',') && (p[k] == 0 || p[k] == ',')) return true;
    p += k;
  }
  return false;
}

void footballSetFavorites(const char *favList) {          // web page (loop thread)
  load();
  String s = favList ? favList : ""; s.toUpperCase(); s.replace(" ", "");
  strlcpy(favorites, s.c_str(), sizeof favorites);
  prefs.putString("futFav", favorites);
  ++version;
}

String footballFavorites() { load(); return String(favorites); }
uint32_t footballFavoritesVersion() { return version; }
