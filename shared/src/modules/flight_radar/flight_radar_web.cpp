// Flight Radar on the settings web page: airport and radius.
#include <Arduino.h>
#include <WebServer.h>
#include "core/app_core.h"
#include "core/web_ui.h"
#include "modules/flight_radar/flight_radar.h"

static void radarCard(String &html) {
  char airportCode[8], airportName[32];
  uint8_t radius = 10;
  getFlightRadarSettings(airportCode, sizeof airportCode, airportName, sizeof airportName, radius);
  String c = T(R"HTML(<form class="card" method="post" action="/save-radar" accept-charset="UTF-8"><h2>Radar aéreo</h2>
<div class="muted">Atual: %AIRPORT% (%AIRPORTNAME%), raio %RADIUS% km</div>
<label class="label" for="airport">Aeroporto (IATA ou ICAO)</label>
<input id="airport" name="airport" value="%AIRPORT%" maxlength="4" list="airports" autocapitalize="characters" required>
<datalist id="airports"><option value="CGH">Congonhas</option><option value="GRU">Guarulhos</option><option value="VCP">Viracopos</option><option value="SDU">Santos Dumont</option><option value="GIG">Galeão</option><option value="BSB">Brasília</option><option value="CNF">Confins</option><option value="CWB">Curitiba</option><option value="POA">Porto Alegre</option><option value="FLN">Florianópolis</option><option value="REC">Recife</option><option value="SSA">Salvador</option><option value="FOR">Fortaleza</option><option value="BEL">Belém</option><option value="MAO">Manaus</option><option value="NAT">Natal</option><option value="MCZ">Maceió</option><option value="JPA">João Pessoa</option><option value="IGU">Foz do Iguaçu</option><option value="GYN">Goiânia</option></datalist>
<label class="label" for="radius">Raio</label>
<select id="radius" name="radius"><option value="5" %R5%>5 km</option><option value="10" %R10%>10 km</option><option value="25" %R25%>25 km</option><option value="50" %R50%>50 km</option></select>
<button type="submit">Salvar radar</button></form>

)HTML",
    R"HTML(<form class="card" method="post" action="/save-radar" accept-charset="UTF-8"><h2>Flight radar</h2>
<div class="muted">Current: %AIRPORT% (%AIRPORTNAME%), %RADIUS% km radius</div>
<label class="label" for="airport">Airport (IATA or ICAO)</label>
<input id="airport" name="airport" value="%AIRPORT%" maxlength="4" list="airports" autocapitalize="characters" required>
<datalist id="airports"><option value="CGH">Congonhas</option><option value="GRU">Guarulhos</option><option value="VCP">Viracopos</option><option value="SDU">Santos Dumont</option><option value="GIG">Galeão</option><option value="BSB">Brasília</option><option value="CNF">Confins</option><option value="CWB">Curitiba</option><option value="POA">Porto Alegre</option><option value="FLN">Florianópolis</option><option value="REC">Recife</option><option value="SSA">Salvador</option><option value="FOR">Fortaleza</option><option value="BEL">Belém</option><option value="MAO">Manaus</option><option value="NAT">Natal</option><option value="MCZ">Maceió</option><option value="JPA">João Pessoa</option><option value="IGU">Foz do Iguaçu</option><option value="GYN">Goiânia</option></datalist>
<label class="label" for="radius">Radius</label>
<select id="radius" name="radius"><option value="5" %R5%>5 km</option><option value="10" %R10%>10 km</option><option value="25" %R25%>25 km</option><option value="50" %R50%>50 km</option></select>
<button type="submit">Save radar</button></form>

)HTML");
  c.replace("%AIRPORTNAME%", webEscape(airportName));
  c.replace("%AIRPORT%", webEscape(airportCode));
  c.replace("%RADIUS%", String(radius));
  c.replace("%R5%", radius == 5 ? "selected" : "");
  c.replace("%R10%", radius == 10 ? "selected" : "");
  c.replace("%R25%", radius == 25 ? "selected" : "");
  c.replace("%R50%", radius == 50 ? "selected" : "");
  html += c;
}

static void radarRoutes(WebServer &server) {
  server.on("/save-radar", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    String airport = server.arg("airport");
    airport.trim();
    const int radius = server.arg("radius").toInt();
    char currentCode[8];
    uint8_t currentRadius = 0;
    getFlightRadarSettings(currentCode, sizeof currentCode, nullptr, 0, currentRadius);
    // Only apply what changed (each setter clears the radar and re-queries).
    const bool airportOk = airport.equalsIgnoreCase(currentCode) || setFlightRadarAirportCode(airport.c_str());
    const bool radiusOk = radius == currentRadius || setFlightRadarRadiusKm((uint8_t)radius);
    String notice;
    if (!airportOk) notice = String(T("Aeroporto \"", "Airport \"")) + airport +
                             T("\" não reconhecido. Use um da lista (ex.: CGH, GRU, SBGR).", "\" not recognized. Use one from the list (e.g. CGH, GRU, SBGR).");
    else if (!radiusOk) notice = T("Raio inválido.", "Invalid radius.");
    else notice = String(T("Radar salvo: ", "Radar saved: ")) + airport + ", " + String(radius) + " km.";
    webSendSettings(server, notice);
  });
}

const WebSection kRadarWeb = {"radar", radarCard, radarRoutes};
