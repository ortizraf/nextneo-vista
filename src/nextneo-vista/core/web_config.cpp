// Settings web page (http://nextneovista.local) and its server. The page is the header
// card + one card per WebSection (each module adds its own: core/web_ui.h) + the display
// card; the log page and the login stay here. Runs on the Arduino loop thread.
#include "core/build_info.h"
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <esp_netif.h>
#include "app_screens.h"
#include "core/web_config.h"
#include "core/web_ui.h"
#include "core/display_touch.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

static WebServer configServer(80);
static bool configServerStarted = false;
static bool mdnsStarted = false;

String webEscape(const String &value) {
  String out;
  out.reserve(value.length() + 12);
  for (size_t i = 0; i < value.length(); i++) {
    switch (value[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += value[i]; break;
    }
  }
  return out;
}

// ---------------------------------------------------------------- display card (core)
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
static void displayRoutes(WebServer &) {
  configServer.on("/save-display", HTTP_POST, []() { WEB_REQUIRE_LOGIN(configServer)
    const int mhz = configServer.arg("qspi").toInt();
    if (mhz == 40 || mhz == 60 || mhz == 80) prefs.putUInt("qspiHz", (uint32_t)mhz * 1000000UL);
    configServer.send(200, "text/html; charset=utf-8",
      String("<meta http-equiv=refresh content='8;url=/'><body style='font-family:sans-serif;background:#0c1728;color:#fff;padding:30px'>") +
      T("Salvo. Reiniciando a placa com QSPI ", "Saved. Restarting the board with QSPI ") + String(mhz) + " MHz...</body>");
    delay(400);
    ESP.restart();
  });
}
static const WebSection kDisplayWeb = {"display", displayCard, displayRoutes};

// ---------------------------------------------------------------- language card (core)
static void languageCard(String &html) {
  String opts;
  for (uint8_t l = 0; l < LANG_COUNT; ++l)
    opts += String("<option value=\"") + l + "\"" + (l == gLang ? " selected" : "") + ">" + langName(l) + "</option>";
  html += String(R"HTML(<form class="card" method="post" action="/save-language"><h2>Idioma / Language</h2>
<div class="muted">)HTML") + T("Idioma das telas e desta página. A placa reinicia ao salvar.",
                               "Language of the screens and of this page. The board restarts when saved.") +
          "</div><select name=\"lang\">" + opts + "</select><button type=\"submit\">" + T("Salvar e reiniciar", "Save and restart") +
          "</button></form>\n";
}
static void languageRoutes(WebServer &) {
  configServer.on("/save-language", HTTP_POST, []() { WEB_REQUIRE_LOGIN(configServer)
    const int lang = configServer.arg("lang").toInt();
    if (lang >= 0 && lang < LANG_COUNT) prefs.putUChar("lang", (uint8_t)lang);
    configServer.send(200, "text/html; charset=utf-8",
      String("<meta http-equiv=refresh content='8;url=/'><body style='font-family:sans-serif;background:#0c1728;color:#fff;padding:30px'>") +
      (lang == LANG_EN ? "Saved. Restarting the board..." : "Salvo. Reiniciando a placa...") + "</body>");
    delay(400);
    ESP.restart();
  });
}
static const WebSection kLanguageWeb = {"language", languageCard, languageRoutes};

// ---------------------------------------------------------------- sections, in page order
static const WebSection *const kSections[] = {
  &kDisplayWeb, &kLanguageWeb, &kWeatherWeb, &kRadarWeb, &kFinanceWeb, &kPhotosWeb, &kServersWeb, &kFootballWeb,
};

// ---------------------------------------------------------------- settings page
static String settingsPage(const String &notice) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  const String status = sh.status;
  xSemaphoreGive(mtx);
  String page = T(R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Ajustes</title>
<style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(145deg,#091629,#1b3b62);color:#edf4ff;font:16px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:18px}
main{max-width:560px;margin:0 auto}.card{padding:22px;margin-bottom:16px;border:1px solid #ffffff20;border-radius:20px;background:#ffffff0d;box-shadow:0 20px 60px #0005}
h1{font-size:24px;margin:0 0 4px}h2{font-size:18px;margin:0 0 4px}.muted{color:#b4c7df}.status{margin:14px 0 0;padding:14px;border-radius:14px;background:#ffffff10;color:#d9e8fa;line-height:1.7}
.notice{background:#18a4ef33;border:1px solid #18a4ef88}.label{display:block;margin:16px 0 8px;color:#d3e1f1}
input,select{width:100%;padding:12px;border:1px solid #ffffff30;border-radius:12px;background:#0c1728;color:#fff;font-size:16px}
button{margin-top:14px;width:100%;padding:13px;border:0;border-radius:12px;background:#18a4ef;color:#071525;font-size:16px;font-weight:700;cursor:pointer}
button.sec{background:#ffffff1a;color:#edf4ff}small{display:block;margin-top:12px;line-height:1.5;color:#adbed2}.row{display:flex;gap:12px;align-items:center}.row input{width:auto}
.asset{display:grid;grid-template-columns:1fr 1.4fr 44px;gap:8px;margin-top:10px}.asset .info{grid-column:1/4;font-size:13px;color:#9fb6d0;margin:-2px 0 4px 4px}
.del{margin:0;padding:0;background:#ff5a5a33;color:#ffb3b3;font-size:22px}.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{width:auto;margin:0;padding:6px 10px;border-radius:999px;background:#ffffff14;color:#cfe2f7;font-size:13px;font-weight:600}code{color:#9fe3ff}.btnlink{display:block;margin-top:14px;padding:12px;border-radius:12px;background:#ffffff1a;color:#edf4ff;text-align:center;text-decoration:none;font-weight:700}</style>
<main>
<div class="card"><h1>NextNeo Vista</h1><div class="muted">Ajustes do aparelho</div>
<div class="status %NOTICECLASS%">%NOTICE%Wi-Fi: %STATUS%<br>Endereço: http://%IP%<br>Versão: <b>%VERSION%</b> · branch %BRANCH%<br>Gravado (build): <span class="bt" data-ts="%EPOCH%">%BUILDUTC%</span></div>
<a class="btnlink" href="/log">Ver log da aplicação</a><a class="btnlink" href="/logout">Sair</a></div>

)HTML",
    R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Settings</title>
<style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(145deg,#091629,#1b3b62);color:#edf4ff;font:16px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:18px}
main{max-width:560px;margin:0 auto}.card{padding:22px;margin-bottom:16px;border:1px solid #ffffff20;border-radius:20px;background:#ffffff0d;box-shadow:0 20px 60px #0005}
h1{font-size:24px;margin:0 0 4px}h2{font-size:18px;margin:0 0 4px}.muted{color:#b4c7df}.status{margin:14px 0 0;padding:14px;border-radius:14px;background:#ffffff10;color:#d9e8fa;line-height:1.7}
.notice{background:#18a4ef33;border:1px solid #18a4ef88}.label{display:block;margin:16px 0 8px;color:#d3e1f1}
input,select{width:100%;padding:12px;border:1px solid #ffffff30;border-radius:12px;background:#0c1728;color:#fff;font-size:16px}
button{margin-top:14px;width:100%;padding:13px;border:0;border-radius:12px;background:#18a4ef;color:#071525;font-size:16px;font-weight:700;cursor:pointer}
button.sec{background:#ffffff1a;color:#edf4ff}small{display:block;margin-top:12px;line-height:1.5;color:#adbed2}.row{display:flex;gap:12px;align-items:center}.row input{width:auto}
.asset{display:grid;grid-template-columns:1fr 1.4fr 44px;gap:8px;margin-top:10px}.asset .info{grid-column:1/4;font-size:13px;color:#9fb6d0;margin:-2px 0 4px 4px}
.del{margin:0;padding:0;background:#ff5a5a33;color:#ffb3b3;font-size:22px}.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{width:auto;margin:0;padding:6px 10px;border-radius:999px;background:#ffffff14;color:#cfe2f7;font-size:13px;font-weight:600}code{color:#9fe3ff}.btnlink{display:block;margin-top:14px;padding:12px;border-radius:12px;background:#ffffff1a;color:#edf4ff;text-align:center;text-decoration:none;font-weight:700}</style>
<main>
<div class="card"><h1>NextNeo Vista</h1><div class="muted">Device settings</div>
<div class="status %NOTICECLASS%">%NOTICE%Wi-Fi: %STATUS%<br>Address: http://%IP%<br>Version: <b>%VERSION%</b> · branch %BRANCH%<br>Flashed (build): <span class="bt" data-ts="%EPOCH%">%BUILDUTC%</span></div>
<a class="btnlink" href="/log">View the app log</a><a class="btnlink" href="/logout">Log out</a></div>

)HTML");
  page.replace("%NOTICECLASS%", notice.isEmpty() ? "" : "notice");
  page.replace("%NOTICE%", notice.isEmpty() ? String() : webEscape(notice) + "<br>");
  page.replace("%STATUS%", webEscape(status));
  page.replace("%IP%", WiFi.localIP().toString());
  page.replace("%VERSION%", webEscape(kBuildVersion));
  page.replace("%BRANCH%", webEscape(kBuildBranch));
  page.replace("%EPOCH%", String(kBuildEpoch));
  {
    char utc[32];
    const time_t t = (time_t)kBuildEpoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(utc, sizeof utc, "%d/%m/%Y %H:%M UTC", &tmv);
    page.replace("%BUILDUTC%", utc);
  }
  for (const WebSection *s : kSections) s->card(page);
  // Horário do build no fuso do navegador (o texto em UTC fica se o JS não rodar)
  page += "<script>document.querySelectorAll('.bt').forEach(function(e){e.textContent="
          "new Date(e.dataset.ts*1000).toLocaleString()})</script></main></html>";
  return page;
}

void webSendSettings(WebServer &server, const String &notice) {
  server.send(200, "text/html; charset=utf-8", settingsPage(notice));
}

// ---------------------------------------------------------------- log page
// Last 30 minutes of log, newest first. Errors in red, warnings in yellow.
// Streamed in chunks so no big String is built in RAM.
static void sendLogPage() {
  static LogLine *snapshot = nullptr;
  if (!snapshot) snapshot = (LogLine *)psramCalloc(APP_LOG_CAPACITY, sizeof(LogLine));
  const String filter = configServer.arg("f");          // "", "warn" or "err"
  const uint32_t window = 30UL * 60UL * 1000UL;
  const uint32_t nowMs = millis();
  const uint32_t since = nowMs > window ? nowMs - window : 0;
  const size_t n = snapshot ? appLogSnapshot(snapshot, APP_LOG_CAPACITY, since) : 0;
  size_t errors = 0, warnings = 0;
  for (size_t i = 0; i < n; ++i) {
    if (snapshot[i].level == LogLevel::ERROR) ++errors;
    else if (snapshot[i].level == LogLevel::WARN) ++warnings;
  }
  const time_t nowEpoch = time(nullptr);
  const bool haveClock = nowEpoch > 1700000000;

  configServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  configServer.send(200, "text/html; charset=utf-8", "");
  String chunk = T(R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Log</title>
<style>*{box-sizing:border-box}body{margin:0;background:#0b1424;color:#d9e4f2;font:14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px}
main{max-width:900px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.muted{color:#8fa6c2}.bar{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}
.bar a,.bar label{padding:8px 12px;border-radius:10px;background:#ffffff14;color:#d9e4f2;text-decoration:none;font-weight:600}.bar a.on{background:#18a4ef;color:#071525}
table{width:100%;border-collapse:collapse;font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace}td{padding:5px 8px;border-bottom:1px solid #ffffff10;vertical-align:top}
td.t{color:#7f93ad;white-space:nowrap}td.m{word-break:break-word}tr.e td.m{color:#ff6b6b}tr.w td.m{color:#ffd477}
.b{display:inline-block;min-width:44px;text-align:center;border-radius:6px;padding:1px 6px;font-size:11px;font-weight:700}.b.e{background:#ff6b6b33;color:#ff8f8f}.b.w{background:#ffd47733;color:#ffd477}.b.i{background:#ffffff14;color:#9fb6d0}.b.v{background:#ffffff0a;color:#6f839c}tr.v td.m{color:#8fa0b6}
.empty{padding:24px;text-align:center;color:#8fa6c2}</style>
<main><h1>Log da aplicação</h1><div class="muted">Últimos 30 minutos · mais recentes primeiro · %COUNT% linhas (%ERR% erros, %WARN% avisos)</div>
<div class="bar"><a href="/">&larr; Ajustes</a><a href="/log" class="%ALL%">Tudo</a><a href="/log?f=warn" class="%FW%">Avisos e erros</a><a href="/log?f=err" class="%FE%">Só erros</a>
<a href="/log-verbose?on=%VNEXT%" class="%VON%">Verbose: %VTXT%</a>
<label><input type="checkbox" id="auto" onchange="tick()"> Atualizar a cada 5 s</label></div><table>)HTML",
    R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Log</title>
<style>*{box-sizing:border-box}body{margin:0;background:#0b1424;color:#d9e4f2;font:14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px}
main{max-width:900px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.muted{color:#8fa6c2}.bar{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}
.bar a,.bar label{padding:8px 12px;border-radius:10px;background:#ffffff14;color:#d9e4f2;text-decoration:none;font-weight:600}.bar a.on{background:#18a4ef;color:#071525}
table{width:100%;border-collapse:collapse;font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace}td{padding:5px 8px;border-bottom:1px solid #ffffff10;vertical-align:top}
td.t{color:#7f93ad;white-space:nowrap}td.m{word-break:break-word}tr.e td.m{color:#ff6b6b}tr.w td.m{color:#ffd477}
.b{display:inline-block;min-width:44px;text-align:center;border-radius:6px;padding:1px 6px;font-size:11px;font-weight:700}.b.e{background:#ff6b6b33;color:#ff8f8f}.b.w{background:#ffd47733;color:#ffd477}.b.i{background:#ffffff14;color:#9fb6d0}.b.v{background:#ffffff0a;color:#6f839c}tr.v td.m{color:#8fa0b6}
.empty{padding:24px;text-align:center;color:#8fa6c2}</style>
<main><h1>App log</h1><div class="muted">Last 30 minutes · newest first · %COUNT% lines (%ERR% errors, %WARN% warnings)</div>
<div class="bar"><a href="/">&larr; Settings</a><a href="/log" class="%ALL%">All</a><a href="/log?f=warn" class="%FW%">Warnings and errors</a><a href="/log?f=err" class="%FE%">Errors only</a>
<a href="/log-verbose?on=%VNEXT%" class="%VON%">Verbose: %VTXT%</a>
<label><input type="checkbox" id="auto" onchange="tick()"> Refresh every 5 s</label></div><table>)HTML");
  chunk.replace("%COUNT%", String((unsigned)n));
  chunk.replace("%ERR%", String((unsigned)errors));
  chunk.replace("%WARN%", String((unsigned)warnings));
  chunk.replace("%ALL%", filter.isEmpty() ? "on" : "");
  chunk.replace("%FW%", filter == "warn" ? "on" : "");
  chunk.replace("%FE%", filter == "err" ? "on" : "");
  chunk.replace("%VNEXT%", gLogVerbose ? "0" : "1");
  chunk.replace("%VON%", gLogVerbose ? "on" : "");
  chunk.replace("%VTXT%", gLogVerbose ? T("ligado", "on") : T("desligado", "off"));
  configServer.sendContent(chunk);

  size_t shown = 0;
  chunk = "";
  for (size_t k = n; k-- > 0;) {
    const LogLine &l = snapshot[k];
    if (filter == "err" && l.level != LogLevel::ERROR) continue;
    if (filter == "warn" && (l.level == LogLevel::INFO || l.level == LogLevel::VERBOSE)) continue;
    char when[16];
    const uint32_t agoS = (nowMs - l.ms) / 1000;
    if (haveClock) {
      const time_t at = nowEpoch - agoS;
      struct tm tmv;
      localtime_r(&at, &tmv);
      strftime(when, sizeof when, "%H:%M:%S", &tmv);
    } else {
      snprintf(when, sizeof when, "-%lus", (unsigned long)agoS);
    }
    const char *cls = l.level == LogLevel::ERROR ? "e" : l.level == LogLevel::WARN ? "w" : l.level == LogLevel::VERBOSE ? "v" : "i";
    const char *tag = l.level == LogLevel::ERROR ? T("ERRO", "ERROR") : l.level == LogLevel::WARN ? T("AVISO", "WARN") : l.level == LogLevel::VERBOSE ? "verb" : "info";
    chunk += "<tr class=\"";
    chunk += cls;
    chunk += "\"><td class=\"t\">";
    chunk += when;
    chunk += "</td><td><span class=\"b ";
    chunk += cls;
    chunk += "\">";
    chunk += tag;
    chunk += "</span></td><td class=\"m\">";
    chunk += webEscape(l.text);
    chunk += "</td></tr>";
    ++shown;
    if (chunk.length() > 3000) { configServer.sendContent(chunk); chunk = ""; }
  }
  if (!shown) chunk += String("<tr><td class=\"empty\" colspan=\"3\">") + T("Nada registrado neste período.", "Nothing logged in this period.") + "</td></tr>";
  chunk += R"HTML(</table></main><script>
const a=document.getElementById('auto');a.checked=location.hash=='#auto';let t;
function tick(){clearTimeout(t);if(a.checked){history.replaceState(null,'','#auto');t=setTimeout(()=>location.reload(),5000)}else history.replaceState(null,'',location.pathname+location.search)}
tick();</script></html>)HTML";
  configServer.sendContent(chunk);
  configServer.sendContent("");   // end of chunked response
}

static void startConfigServer() {
  webAuthBegin(configServer);
  configServer.on("/", HTTP_GET, []() { WEB_REQUIRE_LOGIN(configServer) webSendSettings(configServer, ""); });
  configServer.on("/log", HTTP_GET, []() { WEB_REQUIRE_LOGIN(configServer) sendLogPage(); });
  configServer.on("/log-verbose", HTTP_GET, []() { WEB_REQUIRE_LOGIN(configServer)         // ?on=1 / ?on=0, saved
    gLogVerbose = configServer.arg("on") == "1";
    prefs.putBool("logVerbose", gLogVerbose);
    Log.printf("Log verbose %s\n", gLogVerbose ? "ativado" : "desativado");
    configServer.sendHeader("Location", "/log");
    configServer.send(302, "text/plain", "");
  });
  for (const WebSection *s : kSections) s->routes(configServer);
  configServer.onNotFound([]() { configServer.send(404, "text/plain; charset=utf-8", T("Não encontrado", "Not found")); });
  configServer.begin();
  configServerStarted = true;
  // mDNS was started in setup() (before Wi-Fi). Here the IP exists: make sure the Wi-Fi
  // interface answers the hostname (A record) and announce it right away.
  if (mdnsStarted) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) mdns_netif_action(sta, (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
    Log.printf("mDNS: http://%s.local -> %s\n", DEVICE_HOSTNAME, WiFi.localIP().toString().c_str());
  }
}

// http://nextneovista.local (mDNS / Bonjour). Started BEFORE connecting: the mDNS
// responder enables the Wi-Fi interface on the "got IP" event. Started after it (as
// before), the service was announced but the hostname itself was never answered:
// "dns-sd -B" listed the board, while "ping nextneovista.local" could not resolve it.
void webConfigBeginMdns() {
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.setInstanceName("NextNeo Vista");     // friendly name in Bonjour/network browsers
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  } else Log.println("mDNS: falha ao iniciar");
}

void webConfigLoop() {
  if (WiFi.status() == WL_CONNECTED && !configServerStarted) startConfigServer();
  if (configServerStarted) configServer.handleClient();
}
