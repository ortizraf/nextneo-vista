// Weather on the settings web page: city / coordinates and the 24 h clock.
#include <Arduino.h>
#include <WebServer.h>
#include "core/app_core.h"
#include "core/web_ui.h"
#include "modules/weather/weather.h"

static void weatherCard(String &html) {
  xSemaphoreTake(wxMtx, portMAX_DELAY);
  const String city = wx.cityName;
  xSemaphoreGive(wxMtx);
  String c = T(R"HTML(<form class="card" method="post" action="/save-city" accept-charset="UTF-8"><h2>Clima e relógio</h2>
<label class="label" for="city">Cidade</label><input id="city" name="city" value="%CITY%" required><div class="muted" style="margin-top:6px;font-size:13px">Ex.: <code>São Paulo, BR</code> ou <code>Nome @ lat,lon</code> (Google Maps &rarr; botão direito no local)</div>
<label class="label row"><input type="checkbox" name="h24" value="1" %H24%> Relógio 24 horas</label>
<button type="submit">Salvar clima</button></form>

)HTML",
    R"HTML(<form class="card" method="post" action="/save-city" accept-charset="UTF-8"><h2>Weather and clock</h2>
<label class="label" for="city">City</label><input id="city" name="city" value="%CITY%" required><div class="muted" style="margin-top:6px;font-size:13px">E.g. <code>London, GB</code> or <code>Name @ lat,lon</code> (Google Maps &rarr; right-click the place)</div>
<label class="label row"><input type="checkbox" name="h24" value="1" %H24%> 24-hour clock</label>
<button type="submit">Save weather</button></form>

)HTML");
  c.replace("%CITY%", webEscape(prefs.isKey("cityRaw") ? prefs.getString("cityRaw", city) : city));
  c.replace("%H24%", cfg.h24 ? "checked" : "");
  html += c;
}

static void weatherRoutes(WebServer &server) {
  server.on("/save-city", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    String city = server.arg("city");
    city.trim();
    cfg.h24 = server.hasArg("h24");
    prefs.putBool("h24", cfg.h24);
    if (city.indexOf('@') >= 0) {
      if (!applyManualLocation(city)) {
        webSendSettings(server, T("Coordenadas inválidas. Use: Nome @ lat,lon (ex.: Minha casa @ -xx.xxx,-xx.xxx)", "Invalid coordinates. Use: Name @ lat,lon (e.g. My home @ -xx.xxx,-xx.xxx)"));
        return;
      }
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;   // o job abaixo só reaplica (sem rede) e atualiza o clima
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
      webSendSettings(server, T("Local salvo.", "Location saved."));
      return;
    }
    if (!city.isEmpty()) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;
      sh.status = T("Buscando cidade...", "Searching city...");
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
    }
    webSendSettings(server, city.isEmpty() ? T("Formato de hora salvo.", "Time format saved.") : T("Ajustes salvos; atualizando a previsão.", "Settings saved; updating the forecast."));
  });
}

const WebSection kWeatherWeb = {"weather", weatherCard, weatherRoutes};
