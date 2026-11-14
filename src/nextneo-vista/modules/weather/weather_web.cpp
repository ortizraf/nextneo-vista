// Weather on the settings web page: city / coordinates and the 24 h clock.
#include <Arduino.h>
#include <WebServer.h>
#include "app_screens.h"
#include "core/web_ui.h"
#include "modules/weather/weather.h"

static void weatherCard(String &html) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const String city = wx.cityName;
  xSemaphoreGive(mtx);
  String c = R"HTML(<form class="card" method="post" action="/save-city" accept-charset="UTF-8"><h2>Clima e relógio</h2>
<label class="label" for="city">Cidade</label><input id="city" name="city" value="%CITY%" required><div class="muted" style="margin-top:6px;font-size:13px">Ex.: <code>São Paulo, BR</code> ou <code>Nome @ lat,lon</code> (Google Maps &rarr; botão direito no local)</div>
<label class="label row"><input type="checkbox" name="h24" value="1" %H24%> Relógio 24 horas</label>
<button type="submit">Salvar clima</button></form>

)HTML";
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
        webSendSettings(server, "Coordenadas inválidas. Use: Nome @ lat,lon (ex.: Minha casa @ -xx.xxx,-xx.xxx)");
        return;
      }
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;   // o job abaixo só reaplica (sem rede) e atualiza o clima
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
      webSendSettings(server, "Local salvo.");
      return;
    }
    if (!city.isEmpty()) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;
      sh.status = "Buscando cidade...";
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
    }
    webSendSettings(server, city.isEmpty() ? "Formato de hora salvo." : "Ajustes salvos; atualizando a previsão.");
  });
}

const WebSection kWeatherWeb = {"weather", weatherCard, weatherRoutes};
