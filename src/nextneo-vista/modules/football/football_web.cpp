// Futebol on the settings web page: favourite teams.
#include <Arduino.h>
#include <WebServer.h>
#include "app_screens.h"
#include "core/web_ui.h"
#include "modules/football/football.h"

static void footballCard(String &html) {
  String c = R"HTML(<form class="card" method="post" action="/save-football" accept-charset="UTF-8"><h2>Futebol</h2>
<div class="muted">Times favoritos ficam em destaque e sempre no topo da aba Ao vivo. Use a sigla da ESPN, separadas por vírgula (ex.: <code>FLA</code>, <code>PAL,SAN</code>, <code>RMA</code>).</div>
<label class="label" for="fav">Times favoritos</label><input id="fav" name="fav" value="%FUTFAV%" maxlength="60" placeholder="FLA">
<button type="submit">Salvar</button></form>

)HTML";
  c.replace("%FUTFAV%", webEscape(footballFavorites()));
  html += c;
}

static void footballRoutes(WebServer &server) {
  server.on("/save-football", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    footballSetFavorites(server.arg("fav").c_str());
    webSendSettings(server, String("Futebol: favoritos ") + (footballFavorites().length() ? footballFavorites() : String("(nenhum)")) + ".");
  });
}

const WebSection kFootballWeb = {"football", footballCard, footballRoutes};
