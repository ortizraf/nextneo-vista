#include "core/i18n.h"
#include "web_auth.h"
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include "app_log.h"

extern Preferences prefs;

static constexpr uint8_t SESSIONS = 4;
static constexpr uint8_t MAX_FAILS = 5;
static constexpr uint32_t LOCK_MS = 30000UL;

struct Session { char token[33]; uint32_t lastMs; };
static Session sessions[SESSIONS];
static char gUser[33] = "admin";
static char gPassHash[65] = "";
static uint8_t fails = 0;
static uint32_t lockedUntil = 0;

static void hashPass(const char *pass, char out[65]) {
  String salted = String("nextneo-vista:") + pass;
  unsigned char d[32];
  mbedtls_sha256((const unsigned char *)salted.c_str(), salted.length(), d, 0);
  for (int i = 0; i < 32; ++i) sprintf(out + i * 2, "%02x", d[i]);
  out[64] = 0;
}

// constant-time compare (no early exit on the first different char)
static bool sameStr(const char *a, const char *b) {
  const size_t la = strlen(a), lb = strlen(b);
  uint8_t diff = la != lb;
  for (size_t i = 0; i < la && i < lb; ++i) diff |= a[i] ^ b[i];
  return !diff;
}

static void clearSessions() { for (Session &s : sessions) s.token[0] = 0; }

static Session *findSession(WebServer &server) {
  const String cookie = server.header("Cookie");
  const int at = cookie.indexOf("nnv=");
  if (at < 0) return nullptr;
  const String tok = cookie.substring(at + 4, at + 4 + 32);
  if (tok.length() != 32) return nullptr;
  for (Session &s : sessions) {
    if (!s.token[0]) continue;
    if (millis() - s.lastMs > WEB_SESSION_MS) { s.token[0] = 0; continue; }   // expired
    if (sameStr(s.token, tok.c_str())) return &s;
  }
  return nullptr;
}

bool webAuthCheck(WebServer &server) {
  Session *s = findSession(server);
  if (!s) return false;
  s->lastMs = millis();                       // sliding: 10 min after the last request
  return true;
}

void webAuthDeny(WebServer &server) {
  if (server.method() == HTTP_GET) {
    String next = server.uri();
    if (server.args()) {
      next += '?';
      for (int i = 0; i < server.args(); ++i) { if (i) next += '&'; next += server.argName(i) + "=" + server.arg(i); }
    }
    server.sendHeader("Location", "/login?next=" + next);
    server.send(302, "text/plain", "");
  } else {
    server.send(401, "text/plain; charset=utf-8", T("Sessão expirada: entre novamente", "Session expired: log in again"));
  }
}

static String htmlEsc(const String &v) {
  String o; o.reserve(v.length() + 8);
  for (char c : v) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static const char LOGIN_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;color:#edf4ff;
background:radial-gradient(120% 90% at 50% 0%,#0b2a6b 0%,#050e24 60%,#03091a 100%)}
.box{width:min(360px,calc(100% - 32px));padding:28px 24px;border-radius:20px;background:#0a1430cc;
border:1px solid #1e4fd855;box-shadow:0 20px 60px #0008}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:6px}
.mark{width:42px;height:42px;border-radius:12px;background:linear-gradient(160deg,#38d6f5,#1456e0 70%);
display:flex;align-items:center;justify-content:center;font-weight:800;color:#03122e;font-size:18px}
h1{margin:0;font-size:22px;letter-spacing:.3px}.sub{color:#8fa8cc;font-size:14px;margin:0 0 22px}
label{display:block;font-size:13px;color:#9fb6d0;margin:14px 0 6px}
input{width:100%;padding:12px 14px;border:1px solid #ffffff26;border-radius:12px;background:#07122a;color:#fff;font-size:16px}
input:focus{outline:none;border-color:#38d6f5}
button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:12px;background:#1e90ff;color:#fff;
font-size:16px;font-weight:700;cursor:pointer}button:active{background:#1767c8}
.err{margin-top:14px;padding:10px 12px;border-radius:10px;background:#ff5a5a22;color:#ffb3b3;font-size:14px}
.foot{margin-top:18px;color:#5f7699;font-size:12px;text-align:center}</style></head><body>
<form class="box" method="post" action="/login">
<div class="logo"><div class="mark">NV</div><h1>NextNeo Vista</h1></div>
<p class="sub">Entre para acessar os ajustes do aparelho</p>
<label for="u">Usuário</label><input id="u" name="u" autocomplete="username" autocapitalize="none" required autofocus>
<label for="p">Senha</label><input id="p" name="p" type="password" autocomplete="current-password" required>
<input type="hidden" name="next" value="%NEXT%">%ERR%
<button type="submit">Entrar</button>
<div class="foot">A sessão expira após 10 min sem uso · usuário e senha são alterados no aparelho, em Ajustes</div>
</form></body></html>)HTML";

static const char LOGIN_PAGE_EN[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;color:#edf4ff;
background:radial-gradient(120% 90% at 50% 0%,#0b2a6b 0%,#050e24 60%,#03091a 100%)}
.box{width:min(360px,calc(100% - 32px));padding:28px 24px;border-radius:20px;background:#0a1430cc;
border:1px solid #1e4fd855;box-shadow:0 20px 60px #0008}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:6px}
.mark{width:42px;height:42px;border-radius:12px;background:linear-gradient(160deg,#38d6f5,#1456e0 70%);
display:flex;align-items:center;justify-content:center;font-weight:800;color:#03122e;font-size:18px}
h1{margin:0;font-size:22px;letter-spacing:.3px}.sub{color:#8fa8cc;font-size:14px;margin:0 0 22px}
label{display:block;font-size:13px;color:#9fb6d0;margin:14px 0 6px}
input{width:100%;padding:12px 14px;border:1px solid #ffffff26;border-radius:12px;background:#07122a;color:#fff;font-size:16px}
input:focus{outline:none;border-color:#38d6f5}
button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:12px;background:#1e90ff;color:#fff;
font-size:16px;font-weight:700;cursor:pointer}button:active{background:#1767c8}
.err{margin-top:14px;padding:10px 12px;border-radius:10px;background:#ff5a5a22;color:#ffb3b3;font-size:14px}
.foot{margin-top:18px;color:#5f7699;font-size:12px;text-align:center}</style></head><body>
<form class="box" method="post" action="/login">
<div class="logo"><div class="mark">NV</div><h1>NextNeo Vista</h1></div>
<p class="sub">Entre para acessar os ajustes do aparelho</p>
<label for="u">User</label><input id="u" name="u" autocomplete="username" autocapitalize="none" required autofocus>
<label for="p">Password</label><input id="p" name="p" type="password" autocomplete="current-password" required>
<input type="hidden" name="next" value="%NEXT%">%ERR%
<button type="submit">Log in</button>
<div class="foot">The session expires after 10 min without use · user and password are changed on the device, in Settings</div>
</form></body></html>)HTML";

static void sendLogin(WebServer &server, const String &next, const char *err) {
  String page = gLang == LANG_EN ? FPSTR(LOGIN_PAGE_EN) : FPSTR(LOGIN_PAGE);
  page.replace("%NEXT%", htmlEsc(next));
  page.replace("%ERR%", err && *err ? String("<div class=\"err\">") + err + "</div>" : String());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", page);
}

static String safeNext(const String &n) {               // only local paths: no open redirect
  return n.startsWith("/") && !n.startsWith("//") && !n.startsWith("/login") ? n : String("/");
}

static bool loaded = false;
static void loadCredentials() {                          // also used by Settings, which is built before the web server
  if (loaded) return;
  loaded = true;
  // isKey first: getString on a missing key logs an [E] line (seen as ERRO in /log)
  String u = prefs.isKey("webUser") ? prefs.getString("webUser", "admin") : String("admin");
  if (!u.length()) u = "admin";
  snprintf(gUser, sizeof gUser, "%s", u.c_str());
  String h = prefs.isKey("webPassH") ? prefs.getString("webPassH", "") : String();
  if (h.length() == 64) snprintf(gPassHash, sizeof gPassHash, "%s", h.c_str());
  else hashPass("admin", gPassHash);
}

void webAuthBegin(WebServer &server) {
  loadCredentials();
  clearSessions();

  static const char *headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/login", HTTP_GET, [&server]() {
    if (webAuthCheck(server)) { server.sendHeader("Location", safeNext(server.arg("next"))); server.send(302, "text/plain", ""); return; }
    sendLogin(server, safeNext(server.arg("next")), nullptr);
  });
  server.on("/login", HTTP_POST, [&server]() {
    const String next = safeNext(server.arg("next"));
    if ((int32_t)(millis() - lockedUntil) < 0) { sendLogin(server, next, T("Muitas tentativas. Aguarde 30 segundos.", "Too many attempts. Wait 30 seconds.")); return; }
    char h[65]; hashPass(server.arg("p").c_str(), h);
    const bool ok = sameStr(server.arg("u").c_str(), gUser) & sameStr(h, gPassHash);
    if (!ok) {
      if (++fails >= MAX_FAILS) { fails = 0; lockedUntil = millis() + LOCK_MS; }
      Log.println("Web: login recusado");
      sendLogin(server, next, T("Usuário ou senha incorretos.", "Wrong user or password."));
      return;
    }
    fails = 0;
    Session *slot = &sessions[0];                        // free slot, else the oldest one
    for (Session &s : sessions) {
      if (!s.token[0] || millis() - s.lastMs > WEB_SESSION_MS) { slot = &s; break; }
      if (s.lastMs < slot->lastMs) slot = &s;
    }
    for (int i = 0; i < 16; ++i) sprintf(slot->token + i * 2, "%02x", (unsigned)(esp_random() & 0xFF));
    slot->token[32] = 0;
    slot->lastMs = millis();
    Log.println("Web: login ok");
    server.sendHeader("Set-Cookie", String("nnv=") + slot->token + "; Path=/; HttpOnly; SameSite=Strict");
    server.sendHeader("Location", next);
    server.send(302, "text/plain", "");
  });
  server.on("/logout", HTTP_GET, [&server]() {
    if (Session *s = findSession(server)) s->token[0] = 0;
    server.sendHeader("Set-Cookie", "nnv=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
  });
}

bool webAuthSet(const char *user, const char *pass) {
  loadCredentials();
  if (!user || !*user || strlen(user) > 32 || !pass || strlen(pass) < 4 || strlen(pass) > 64) return false;
  snprintf(gUser, sizeof gUser, "%s", user);
  hashPass(pass, gPassHash);
  prefs.putString("webUser", gUser);
  prefs.putString("webPassH", gPassHash);
  clearSessions();                                       // everyone logs in again with the new credentials
  Log.println("Web: usuário/senha da página alterados no aparelho");
  return true;
}

String webAuthUser() { loadCredentials(); return String(gUser); }
