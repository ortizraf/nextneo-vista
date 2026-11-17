// Cartão da página web específico desta placa (ESP32-S3 3,5"): velocidade do barramento
// QSPI da tela. A página (shared/src/core/web_config.cpp) mostra kBoardWeb logo após o
// cartão de idioma; cada placa define o seu.
#include <Arduino.h>
#include <WebServer.h>
#include "core/app_core.h"
#include "core/web_ui.h"
#include "core/display_touch.h"

static void displayCard(String &html) {
  String c = T(R"HTML(<form class="card" method="post" action="/save-display"><h2>Tela (desempenho)</h2>
<div class="muted">Velocidade do barramento QSPI da tela. Valores maiores deixam as animações mais fluidas; se aparecerem listras ou chiado, volte para 40 MHz. A placa reinicia ao salvar.</div>
<select name="qspi"><option value="40" %Q40%>40 MHz (padrão, seguro)</option><option value="60" %Q60%>60 MHz</option><option value="80" %Q80%>80 MHz (mais rápido)</option></select>
<button type="submit">Salvar e reiniciar</button></form>

)HTML",
    R"HTML(<form class="card" method="post" action="/save-display"><h2>Display (performance)</h2>
<div class="muted">Speed of the display's QSPI bus. Higher values make animations smoother; if stripes or noise appear, go back to 40 MHz. The board restarts when saved.</div>
<select name="qspi"><option value="40" %Q40%>40 MHz (default, safe)</option><option value="60" %Q60%>60 MHz</option><option value="80" %Q80%>80 MHz (faster)</option></select>
<button type="submit">Save and restart</button></form>

)HTML");
  c.replace("%Q40%", displayQspiHz() == 40000000 ? "selected" : "");
  c.replace("%Q60%", displayQspiHz() == 60000000 ? "selected" : "");
  c.replace("%Q80%", displayQspiHz() == 80000000 ? "selected" : "");
  html += c;
}
static void displayRoutes(WebServer &configServer) {
  configServer.on("/save-display", HTTP_POST, [&configServer]() { WEB_REQUIRE_LOGIN(configServer)
    const int mhz = configServer.arg("qspi").toInt();
    if (mhz == 40 || mhz == 60 || mhz == 80) prefs.putUInt("qspiHz", (uint32_t)mhz * 1000000UL);
    configServer.send(200, "text/html; charset=utf-8",
      String("<meta http-equiv=refresh content='8;url=/'><body style='font-family:sans-serif;background:#0c1728;color:#fff;padding:30px'>") +
      T("Salvo. Reiniciando a placa com QSPI ", "Saved. Restarting the board with QSPI ") + String(mhz) + " MHz...</body>");
    delay(400);
    ESP.restart();
  });
}
const WebSection kBoardWeb = {"display", displayCard, displayRoutes};

