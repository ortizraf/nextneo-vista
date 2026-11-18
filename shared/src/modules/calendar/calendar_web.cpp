// Card "Calendário" da página de ajustes: escolhe o gato do fundo.
// Salvo em NVS ("calCat"); a tela lê calendarCat() e troca a arte ao perceber a versão nova.
#include "core/app_core.h"
#include "core/web_ui.h"
#include "modules/calendar/calendar_cats.h"

static int16_t cachedCat = -1;
static volatile uint32_t catVersion = 1;

uint8_t calendarCat() {
  if (cachedCat < 0) cachedCat = calendarCatFromArg(String(prefs.getUChar("calCat", 0)).c_str());
  return (uint8_t)cachedCat;
}
void setCalendarCat(uint8_t cat) {
  if (cat >= CAL_CAT_COUNT) cat = CAL_CAT_FRAJOLA;
  prefs.putUChar("calCat", cat);
  cachedCat = cat;
  catVersion++;
}
uint32_t calendarCatVersion() { return catVersion; }

static void calendarCard(String &html) {
  String opts;
  for (uint8_t c = 0; c < CAL_CAT_COUNT; ++c)
    opts += String("<option value=\"") + c + "\"" + (c == calendarCat() ? " selected" : "") + ">" + calendarCatName(c) + "</option>";
  html += String(R"HTML(<form class="card" method="post" action="/save-calendar"><h2>)HTML") + T("Calendário", "Calendar") +
          "</h2><div class=\"muted\">" + T("Gato do fundo do calendário.", "Cat on the calendar background.") +
          "</div><select name=\"cat\">" + opts + "</select><button type=\"submit\">" + T("Salvar", "Save") + "</button></form>\n";
}
static void calendarRoutes(WebServer &server) {
  server.on("/save-calendar", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    setCalendarCat(calendarCatFromArg(server.arg("cat").c_str()));
    webSendSettings(server, T("Gato do calendário salvo.", "Calendar cat saved."));
  });
}
const WebSection kCalendarWeb = {"calendar", calendarCard, calendarRoutes};
