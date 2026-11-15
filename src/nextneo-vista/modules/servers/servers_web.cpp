// Servers on the settings web page: Beszel login and the two system links.
#include <Arduino.h>
#include <WebServer.h>
#include "app_screens.h"
#include "core/web_ui.h"
#include "modules/servers/servers.h"

static void serversCard(String &html) {
  String bzRows;
  for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
    char lbl[64], url[64], id[64];
    serversGetConfig(i, lbl, url, id, sizeof lbl);
    const String n = String(i);
    bzRows += String("<label class=\"label\">") + T("Servidor ", "Server ") + String(i + 1) +
              "</label><div class=\"asset\" style=\"grid-template-columns:1fr 1.6fr\">"
              "<input name=\"bzlbl" + n + "\" value=\"" + webEscape(lbl) + "\" placeholder=\"" + T("Apelido", "Nickname") + "\" maxlength=\"23\">"
              "<input name=\"bzlink" + n + "\" value=\"" + webEscape(String(url) + "/system/" + id) + "\" placeholder=\"http://host:8090/system/ID\">"
              "<span class=\"info\" style=\"grid-column:1/3\">" + webEscape(serversStatusText(i)) + "</span></div>";
  }
  String c = T(R"HTML(<form class="card" method="post" action="/save-servers" accept-charset="UTF-8"><h2>Servidores (Beszel)</h2>
<div class="muted">Use um usuário <b>somente leitura</b> do Beszel, com acesso aos dois sistemas.</div>
<label class="label" for="bzuser">E-mail do usuário</label><input id="bzuser" name="bzuser" type="email" value="%BZUSER%" autocomplete="username">
<label class="label" for="bzpass">Senha</label><input id="bzpass" name="bzpass" type="password" placeholder="%BZPASSPH%" autocomplete="current-password">
%BZROWS%
<button type="submit">Salvar servidores</button>
<small>Cole o link da página do sistema no Beszel (ex.: <code>http://nextneo.local:8090/system/abc123</code>); a URL do hub e o ID são extraídos automaticamente.</small></form>
)HTML",
    R"HTML(<form class="card" method="post" action="/save-servers" accept-charset="UTF-8"><h2>Servers (Beszel)</h2>
<div class="muted">Use a <b>read-only</b> Beszel user with access to both systems.</div>
<label class="label" for="bzuser">User e-mail</label><input id="bzuser" name="bzuser" type="email" value="%BZUSER%" autocomplete="username">
<label class="label" for="bzpass">Password</label><input id="bzpass" name="bzpass" type="password" placeholder="%BZPASSPH%" autocomplete="current-password">
%BZROWS%
<button type="submit">Save servers</button>
<small>Paste the link of the system page in Beszel (e.g. <code>http://nextneo.local:8090/system/abc123</code>); the hub URL and the ID are extracted automatically.</small></form>
)HTML");
  c.replace("%BZROWS%", bzRows);
  c.replace("%BZUSER%", webEscape(serversUser()));
  c.replace("%BZPASSPH%", serversHasPassword() ? T("•••••• (salva; deixe vazio para manter)", "•••••• (saved; leave empty to keep)") : T("senha do usuário", "user password"));
  html += c;
}

static void serversRoutes(WebServer &server) {
  server.on("/save-servers", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    String user = server.arg("bzuser"); user.trim();
    String pass = server.arg("bzpass");
    serversSetCredentials(user.c_str(), pass.c_str());
    String notice = T("Servidores salvos.", "Servers saved.");
    for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
      String lbl = server.arg("bzlbl" + String(i)); lbl.trim();
      char url[64] = "", id[24] = "";
      if (!serversParseLink(server.arg("bzlink" + String(i)).c_str(), url, sizeof url, id, sizeof id)) continue;
      if (!id[0]) notice += String(T(" Servidor ", " Server ")) + String(i + 1) + T(": link sem /system/ID (ID mantido).", ": link without /system/ID (ID kept).");
      serversSetServer(i, lbl.c_str(), url, id);
    }
    if (activeScreenId() == SCREEN_SERVERS) postScreenJob(JOB_UPDATE);
    else notice += T(" Abra a tela Servers para consultar.", " Open the Servers screen to check.");
    webSendSettings(server, notice);
  });
}

const WebSection kServersWeb = {"servers", serversCard, serversRoutes};
