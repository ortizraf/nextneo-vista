// Web configuration page (http://nextneovista.local): settings, Finance assets,
// servers, photos upload and the log page. Runs on the Arduino loop thread.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <esp_netif.h>
#include "app_screens.h"
#include "core/web_config.h"
#include "core/display_touch.h"
#include "modules/weather/weather.h"
#include "modules/finance/finance.h"
#include "modules/servers/servers.h"
#include "modules/photos/photos.h"
#include "modules/football/football.h"
#include "modules/flight_radar/flight_radar.h"
#include "services/web_auth.h"
#include "services/psram_alloc.h"
#include "services/app_log.h"

static WebServer configServer(80);
static bool configServerStarted = false;
static bool mdnsStarted = false;

static const char PHOTOS_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Fotos</title>
<style>*{box-sizing:border-box}body{margin:0;background:#0b1424;color:#d9e4f2;font:15px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px}
main{max-width:560px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.muted{color:#8fa6c2;font-size:13px}a{color:#7cc4ff}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin:14px 0}.slot{position:relative;aspect-ratio:3/2;border-radius:10px;background:#ffffff10;overflow:hidden;border:2px solid transparent;cursor:pointer;display:flex;align-items:center;justify-content:center;color:#8fa6c2;font-size:22px}
.slot img{width:100%;height:100%;object-fit:cover}.slot.sel{border-color:#18a4ef}.slot .n{position:absolute;left:5px;top:3px;font-size:11px;color:#fff;text-shadow:0 1px 2px #000}
.ed{background:#ffffff0c;border-radius:14px;padding:12px;display:none}.ed.on{display:block}canvas{width:100%;border-radius:8px;touch-action:none;background:#000;display:block}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px}button,label.btn{border:0;border-radius:10px;padding:10px 14px;font-weight:700;background:#ffffff18;color:#d9e4f2;font-size:14px;cursor:pointer}
button.p{background:#18a4ef;color:#071525}button.d{background:#ff6b6b33;color:#ff9a9a}input[type=range]{flex:1}.seg button.on{background:#18a4ef;color:#071525}#msg{margin-top:10px;min-height:20px}</style>
<main><h1>Fotos</h1><div class="muted"><a href="/">&larr; Ajustes</a> · até 8 fotos · cada uma passa 30 s · arraste para o lado na placa para trocar</div>
<div class="grid" id="grid"></div>
<div class="ed" id="ed"><canvas id="cv" width="480" height="320"></canvas>
<div class="muted" style="margin-top:6px">Arraste para escolher a parte da foto · use o zoom para fechar</div>
<div class="row">Zoom <input type="range" id="zoom" min="1" max="4" step="0.01" value="1"></div>
<div class="row"><span class="seg"><button id="mFill" class="on">Preencher</button> <button id="mFit">Inteira</button></span><button id="rot">Girar 90°</button></div>
<div class="row"><label class="btn"><input type="file" id="file" accept="image/*" hidden>Escolher foto</label><button class="p" id="send">Enviar para a placa</button><button class="d" id="del">Apagar</button></div>
<div id="msg" class="muted"></div></div></main>
<script>
const N=8,W=480,H=320;let sel=-1,src=null,zoom=1,ox=0,oy=0,mode='fill';const cv=document.getElementById('cv'),cx=cv.getContext('2d'),$=id=>document.getElementById(id);
function grid(){const g=$('grid');g.innerHTML='';for(let i=0;i<N;i++){const d=document.createElement('div');d.className='slot'+(i==sel?' sel':'');d.innerHTML='<span class="n">'+(i+1)+'</span>+';
 const im=new Image();im.onload=()=>{d.innerHTML='<span class="n">'+(i+1)+'</span>';d.prepend(im)};im.src='/photo?slot='+i+'&t='+Date.now();d.onclick=()=>pick(i);g.appendChild(d)}}
function pick(i){sel=i;src=null;$('ed').classList.add('on');$('msg').textContent='Foto '+(i+1)+': escolha uma imagem';cx.fillStyle='#000';cx.fillRect(0,0,W,H);
 document.querySelectorAll('.slot').forEach((d,k)=>d.classList.toggle('sel',k==i));const im=new Image();im.onload=()=>{cx.drawImage(im,0,0,W,H)};im.src='/photo?slot='+i+'&t='+Date.now();$('ed').scrollIntoView({behavior:'smooth'})}
function base(){const w=src.width,h=src.height;return mode=='fill'?Math.max(W/w,H/h):Math.min(W/w,H/h)}
function clamp(){if(mode!='fill')return;const s=base()*zoom,mx=Math.max(0,(src.width*s-W)/2),my=Math.max(0,(src.height*s-H)/2);ox=Math.max(-mx,Math.min(mx,ox));oy=Math.max(-my,Math.min(my,oy))}
function draw(){if(!src)return;clamp();cx.fillStyle='#000';cx.fillRect(0,0,W,H);
 if(mode=='fit'){const b=Math.max(W/src.width,H/src.height);cx.save();cx.filter='blur(18px) brightness(0.55)';cx.drawImage(src,(W-src.width*b)/2,(H-src.height*b)/2,src.width*b,src.height*b);cx.restore()}
 const s=base()*zoom,w=src.width*s,h=src.height*s;cx.drawImage(src,(W-w)/2+ox,(H-h)/2+oy,w,h)}
$('file').onchange=e=>{const f=e.target.files[0];if(!f)return;const im=new Image();im.onload=()=>{const c=document.createElement('canvas');c.width=im.naturalWidth;c.height=im.naturalHeight;c.getContext('2d').drawImage(im,0,0);src=c;zoom=1;ox=oy=0;$('zoom').value=1;draw();$('msg').textContent='Ajuste e toque em Enviar'};im.onerror=()=>{$('msg').textContent='Formato não suportado por este navegador (tente JPG/PNG)'};im.src=URL.createObjectURL(f);e.target.value=''};
$('zoom').oninput=e=>{zoom=+e.target.value;draw()};
$('mFill').onclick=()=>{mode='fill';$('mFill').classList.add('on');$('mFit').classList.remove('on');ox=oy=0;draw()};
$('mFit').onclick=()=>{mode='fit';$('mFit').classList.add('on');$('mFill').classList.remove('on');ox=oy=0;draw()};
$('rot').onclick=()=>{if(!src)return;const c=document.createElement('canvas');c.width=src.height;c.height=src.width;const g=c.getContext('2d');g.translate(c.width,0);g.rotate(Math.PI/2);g.drawImage(src,0,0);src=c;ox=oy=0;draw()};
let drag=null;cv.onpointerdown=e=>{if(!src)return;cv.setPointerCapture(e.pointerId);drag={x:e.clientX,y:e.clientY,ox,oy}};
cv.onpointermove=e=>{if(!drag)return;const k=W/cv.clientWidth;ox=drag.ox+(e.clientX-drag.x)*k;oy=drag.oy+(e.clientY-drag.y)*k;draw()};cv.onpointerup=cv.onpointercancel=()=>drag=null;
$('send').onclick=()=>{if(!src||sel<0){$('msg').textContent='Escolha uma foto primeiro';return}$('msg').textContent='Enviando...';
 cv.toBlob(b=>{const fd=new FormData();fd.append('f',b,'p.jpg');fetch('/photo-upload?slot='+sel,{method:'POST',body:fd}).then(r=>r.text().then(t=>{$('msg').textContent=r.ok?'Enviada ('+Math.round(b.size/1024)+' KB)':'Erro: '+t;if(r.ok){src=null;grid()}})).catch(()=>$('msg').textContent='Falha no envio')},'image/jpeg',0.9)};
$('del').onclick=()=>{if(sel<0)return;fetch('/photo-delete?slot='+sel,{method:'POST'}).then(()=>{$('msg').textContent='Apagada';src=null;cx.fillStyle='#000';cx.fillRect(0,0,W,H);grid()})};
grid();</script></html>)HTML";

static String htmlEscape(const String &value) {
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

static String configPage(const String &notice = "") {
  xSemaphoreTake(mtx, portMAX_DELAY);
  String city = wx.cityName;
  String status = sh.status;
  const uint8_t assetCount = fin.assetCount;
  static FinanceAsset assets[FINANCE_MAX_ASSETS];
  static float price[FINANCE_MAX_ASSETS];
  static bool havePrice[FINANCE_MAX_ASSETS], apiKnown[FINANCE_MAX_ASSETS], apiOk[FINANCE_MAX_ASSETS];
  static char currency[FINANCE_MAX_ASSETS][4];
  memcpy(assets, fin.assets, sizeof assets);
  memcpy(price, fin.price, sizeof price);
  memcpy(havePrice, fin.havePrice, sizeof havePrice);
  memcpy(apiKnown, fin.apiKnown, sizeof apiKnown);
  memcpy(apiOk, fin.apiOk, sizeof apiOk);
  memcpy(currency, fin.currency, sizeof currency);
  xSemaphoreGive(mtx);
  char airportCode[8], airportName[32];
  uint8_t radius = 10;
  getFlightRadarSettings(airportCode, sizeof airportCode, airportName, sizeof airportName, radius);

  String rows;
  for (uint8_t i = 0; i < assetCount; ++i) {
    String info;
    if (havePrice[i]) {
      char b[40];
      snprintf(b, sizeof b, "%s %.4g", currency[i], price[i]);
      info = b;
    } else if (apiKnown[i] && !apiOk[i]) info = "sem cotação (símbolo válido?)";
    else info = "aguardando consulta";
    rows += "<div class=\"asset\"><input name=\"sym\" value=\"" + htmlEscape(assets[i].symbol) +
            "\" placeholder=\"AAPL\" maxlength=\"15\" autocapitalize=\"characters\"><input name=\"lbl\" value=\"" +
            htmlEscape(assets[i].label) + "\" placeholder=\"Legenda (opcional)\" maxlength=\"31\">"
            "<button type=\"button\" class=\"del\" onclick=\"delRow(this)\" title=\"Remover\">&times;</button>"
            "<span class=\"info\">" + htmlEscape(info) + "</span></div>";
  }

  String page = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Ajustes</title>
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
<div class="status %NOTICECLASS%">%NOTICE%Wi-Fi: %STATUS%<br>Endereço: http://%IP%</div>
<a class="btnlink" href="/log">Ver log da aplicação</a><a class="btnlink" href="/logout">Sair</a></div>

<form class="card" method="post" action="/save-football" accept-charset="UTF-8"><h2>Futebol</h2>
<div class="muted">Times favoritos ficam em destaque e sempre no topo da aba Ao vivo. Use a sigla da ESPN, separadas por vírgula (ex.: <code>FLA</code>, <code>PAL,SAN</code>, <code>RMA</code>).</div>
<label class="label" for="fav">Times favoritos</label><input id="fav" name="fav" value="%FUTFAV%" maxlength="60" placeholder="FLA">
<button type="submit">Salvar</button></form>

<form class="card" method="post" action="/save-display"><h2>Tela (desempenho)</h2>
<div class="muted">Velocidade do barramento QSPI da tela. Valores maiores deixam as animações mais fluidas; se aparecerem listras ou chiado, volte para 40 MHz. A placa reinicia ao salvar.</div>
<select name="qspi"><option value="40" %Q40%>40 MHz (padrão, seguro)</option><option value="60" %Q60%>60 MHz</option><option value="80" %Q80%>80 MHz (mais rápido)</option></select>
<button type="submit">Salvar e reiniciar</button></form>

<form class="card" method="post" action="/save-city" accept-charset="UTF-8"><h2>Clima e relógio</h2>
<label class="label" for="city">Cidade</label><input id="city" name="city" value="%CITY%" required><div class="muted" style="margin-top:6px;font-size:13px">Ex.: <code>São Paulo, BR</code> ou <code>Nome @ lat,lon</code> (Google Maps &rarr; botão direito no local)</div>
<label class="label row"><input type="checkbox" name="h24" value="1" %H24%> Relógio 24 horas</label>
<button type="submit">Salvar clima</button></form>

<form class="card" method="post" action="/save-radar" accept-charset="UTF-8"><h2>Radar aéreo</h2>
<div class="muted">Atual: %AIRPORT% (%AIRPORTNAME%), raio %RADIUS% km</div>
<label class="label" for="airport">Aeroporto (IATA ou ICAO)</label>
<input id="airport" name="airport" value="%AIRPORT%" maxlength="4" list="airports" autocapitalize="characters" required>
<datalist id="airports"><option value="CGH">Congonhas</option><option value="GRU">Guarulhos</option><option value="VCP">Viracopos</option><option value="SDU">Santos Dumont</option><option value="GIG">Galeão</option><option value="BSB">Brasília</option><option value="CNF">Confins</option><option value="CWB">Curitiba</option><option value="POA">Porto Alegre</option><option value="FLN">Florianópolis</option><option value="REC">Recife</option><option value="SSA">Salvador</option><option value="FOR">Fortaleza</option><option value="BEL">Belém</option><option value="MAO">Manaus</option><option value="NAT">Natal</option><option value="MCZ">Maceió</option><option value="JPA">João Pessoa</option><option value="IGU">Foz do Iguaçu</option><option value="GYN">Goiânia</option></datalist>
<label class="label" for="radius">Raio</label>
<select id="radius" name="radius"><option value="5" %R5%>5 km</option><option value="10" %R10%>10 km</option><option value="25" %R25%>25 km</option><option value="50" %R50%>50 km</option></select>
<button type="submit">Salvar radar</button></form>

<form class="card" method="post" action="/save-finance" accept-charset="UTF-8" onsubmit="return prep()"><h2>Finance</h2>
<div class="muted">Até 8 ativos. Use o símbolo do <b>Yahoo Finance</b>.</div>
<div id="assets">%ROWS%</div>
<div class="chips" id="chips"></div>
<button type="button" class="sec" onclick="addRow('','')">+ Adicionar ativo</button>
<button type="submit">Salvar Finance</button>
<small>Exemplos: moedas <code>USDBRL=X</code>, <code>GBPBRL=X</code>; cripto <code>BTC-USD</code>, <code>ETH-USD</code>; ações dos EUA <code>AAPL</code>, <code>MSFT</code>; B3 com <code>.SA</code>: <code>PETR4.SA</code>, <code>VALE3.SA</code>; índices <code>^BVSP</code> (Ibovespa), <code>^GSPC</code> (S&amp;P 500). Confira o símbolo em finance.yahoo.com.</small></form>
<div class="card"><h2>Fotos</h2><div class="muted">Até 8 fotos na tela Fotos · cada uma passa 30 s · recorte feito aqui no navegador</div>
<a class="btnlink" href="/photos">Gerenciar fotos</a></div>
<form class="card" method="post" action="/save-servers" accept-charset="UTF-8"><h2>Servidores (Beszel)</h2>
<div class="muted">Use um usuário <b>somente leitura</b> do Beszel, com acesso aos dois sistemas.</div>
<label class="label" for="bzuser">E-mail do usuário</label><input id="bzuser" name="bzuser" type="email" value="%BZUSER%" autocomplete="username">
<label class="label" for="bzpass">Senha</label><input id="bzpass" name="bzpass" type="password" placeholder="%BZPASSPH%" autocomplete="current-password">
%BZROWS%
<button type="submit">Salvar servidores</button>
<small>Cole o link da página do sistema no Beszel (ex.: <code>http://nextneo.local:8090/system/abc123</code>); a URL do hub e o ID são extraídos automaticamente.</small></form>
</main>
<script>
const MAX=8;
function rows(){return document.querySelectorAll('#assets .asset')}
function addRow(s,l){if(rows().length>=MAX){alert('Máximo de '+MAX+' ativos');return}
const d=document.createElement('div');d.className='asset';
d.innerHTML='<input name="sym" placeholder="AAPL" maxlength="15" autocapitalize="characters"><input name="lbl" placeholder="Legenda (opcional)" maxlength="31"><button type="button" class="del" onclick="delRow(this)" title="Remover">&times;</button>';
d.children[0].value=s;d.children[1].value=l;document.getElementById('assets').appendChild(d);if(!s)d.children[0].focus()}
function delRow(b){b.parentNode.remove()}
function prep(){let n=0;rows().forEach(r=>{const s=r.children[0],l=r.children[1];s.value=s.value.trim().toUpperCase();
if(!s.value){r.remove();return}s.name='sym'+n;l.name='lbl'+n;n++});if(!n){alert('Adicione pelo menos um ativo');return false}return true}
[['AAPL','Apple'],['PETR4.SA','Petrobras'],['^BVSP','Ibovespa'],['ETH-USD','Ethereum'],['GBPBRL=X','Libra para real']].forEach(([s,l])=>{
const c=document.createElement('button');c.type='button';c.className='chip';c.textContent='+ '+s;c.onclick=()=>addRow(s,l);document.getElementById('chips').appendChild(c)});
</script></html>)HTML";
  page.replace("%NOTICECLASS%", notice.isEmpty() ? "" : "notice");
  page.replace("%NOTICE%", notice.isEmpty() ? String() : htmlEscape(notice) + "<br>");
  page.replace("%STATUS%", htmlEscape(status));
  page.replace("%IP%", WiFi.localIP().toString());
  page.replace("%CITY%", htmlEscape(prefs.isKey("cityRaw") ? prefs.getString("cityRaw", city) : city));
  page.replace("%H24%", cfg.h24 ? "checked" : "");
  page.replace("%AIRPORTNAME%", htmlEscape(airportName));
  page.replace("%AIRPORT%", htmlEscape(airportCode));
  page.replace("%RADIUS%", String(radius));
  page.replace("%R5%", radius == 5 ? "selected" : "");
  page.replace("%R10%", radius == 10 ? "selected" : "");
  page.replace("%R25%", radius == 25 ? "selected" : "");
  page.replace("%R50%", radius == 50 ? "selected" : "");
  page.replace("%ROWS%", rows);
  page.replace("%Q40%", displayQspiHz() == 40000000 ? "selected" : "");
  page.replace("%FUTFAV%", htmlEscape(footballFavorites()));
  page.replace("%Q60%", displayQspiHz() == 60000000 ? "selected" : "");
  page.replace("%Q80%", displayQspiHz() == 80000000 ? "selected" : "");
  String bzRows;
  for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
    char lbl[64], url[64], id[64];
    serversGetConfig(i, lbl, url, id, sizeof lbl);
    const String n = String(i);
    bzRows += "<label class=\"label\">Servidor " + String(i + 1) + "</label><div class=\"asset\" style=\"grid-template-columns:1fr 1.6fr\">"
              "<input name=\"bzlbl" + n + "\" value=\"" + htmlEscape(lbl) + "\" placeholder=\"Apelido\" maxlength=\"23\">"
              "<input name=\"bzlink" + n + "\" value=\"" + htmlEscape(String(url) + "/system/" + id) + "\" placeholder=\"http://host:8090/system/ID\">"
              "<span class=\"info\" style=\"grid-column:1/3\">" + htmlEscape(serversStatusText(i)) + "</span></div>";
  }
  page.replace("%BZROWS%", bzRows);
  page.replace("%BZUSER%", htmlEscape(serversUser()));
  page.replace("%BZPASSPH%", serversHasPassword() ? "•••••• (salva; deixe vazio para manter)" : "senha do usuário");
  return page;
}

static void sendConfigPage(const String &notice) {
  configServer.send(200, "text/html; charset=utf-8", configPage(notice));
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
  String chunk = R"HTML(<!doctype html><html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>NextNeo Vista | Log</title>
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
<label><input type="checkbox" id="auto" onchange="tick()"> Atualizar a cada 5 s</label></div><table>)HTML";
  chunk.replace("%COUNT%", String((unsigned)n));
  chunk.replace("%ERR%", String((unsigned)errors));
  chunk.replace("%WARN%", String((unsigned)warnings));
  chunk.replace("%ALL%", filter.isEmpty() ? "on" : "");
  chunk.replace("%FW%", filter == "warn" ? "on" : "");
  chunk.replace("%FE%", filter == "err" ? "on" : "");
  chunk.replace("%VNEXT%", gLogVerbose ? "0" : "1");
  chunk.replace("%VON%", gLogVerbose ? "on" : "");
  chunk.replace("%VTXT%", gLogVerbose ? "ligado" : "desligado");
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
    const char *tag = l.level == LogLevel::ERROR ? "ERRO" : l.level == LogLevel::WARN ? "AVISO" : l.level == LogLevel::VERBOSE ? "verb" : "info";
    chunk += "<tr class=\"";
    chunk += cls;
    chunk += "\"><td class=\"t\">";
    chunk += when;
    chunk += "</td><td><span class=\"b ";
    chunk += cls;
    chunk += "\">";
    chunk += tag;
    chunk += "</span></td><td class=\"m\">";
    chunk += htmlEscape(l.text);
    chunk += "</td></tr>";
    ++shown;
    if (chunk.length() > 3000) { configServer.sendContent(chunk); chunk = ""; }
  }
  if (!shown) chunk += "<tr><td class=\"empty\" colspan=\"3\">Nada registrado neste período.</td></tr>";
  chunk += R"HTML(</table></main><script>
const a=document.getElementById('auto');a.checked=location.hash=='#auto';let t;
function tick(){clearTimeout(t);if(a.checked){history.replaceState(null,'','#auto');t=setTimeout(()=>location.reload(),5000)}else history.replaceState(null,'',location.pathname+location.search)}
tick();</script></html>)HTML";
  configServer.sendContent(chunk);
  configServer.sendContent("");   // end of chunked response
}

// Every page/action needs a login session (services/web_auth); /login and /logout are open.
#define WEB_AUTH if (!webAuthCheck(configServer)) { webAuthDeny(configServer); return; }
static void startConfigServer() {
  webAuthBegin(configServer);
  configServer.on("/", HTTP_GET, []() { WEB_AUTH sendConfigPage(""); });
  configServer.on("/save-city", HTTP_POST, []() { WEB_AUTH
    String city = configServer.arg("city");
    city.trim();
    cfg.h24 = configServer.hasArg("h24");
    prefs.putBool("h24", cfg.h24);
    if (city.indexOf('@') >= 0) {
      if (!applyManualLocation(city)) {
        sendConfigPage("Coordenadas inválidas. Use: Nome @ lat,lon (ex.: Minha casa @ -xx.xxx,-xx.xxx)");
        return;
      }
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;   // o job abaixo só reaplica (sem rede) e atualiza o clima
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
      sendConfigPage("Local salvo.");
      return;
    }
    if (!city.isEmpty()) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      sh.pendingCity = city;
      sh.status = "Buscando cidade...";
      xSemaphoreGive(mtx);
      post(CMD_GEOCODE);
    }
    sendConfigPage(city.isEmpty() ? "Formato de hora salvo." : "Ajustes salvos; atualizando a previsão.");
  });
  configServer.on("/save-radar", HTTP_POST, []() { WEB_AUTH
    String airport = configServer.arg("airport");
    airport.trim();
    const int radius = configServer.arg("radius").toInt();
    char currentCode[8];
    uint8_t currentRadius = 0;
    getFlightRadarSettings(currentCode, sizeof currentCode, nullptr, 0, currentRadius);
    // Only apply what changed (each setter clears the radar and re-queries).
    const bool airportOk = airport.equalsIgnoreCase(currentCode) || setFlightRadarAirportCode(airport.c_str());
    const bool radiusOk = radius == currentRadius || setFlightRadarRadiusKm((uint8_t)radius);
    String notice;
    if (!airportOk) notice = String("Aeroporto \"") + airport + "\" não reconhecido. Use um da lista (ex.: CGH, GRU, SBGR).";
    else if (!radiusOk) notice = "Raio inválido.";
    else notice = String("Radar salvo: ") + airport + ", " + String(radius) + " km.";
    sendConfigPage(notice);
  });
  configServer.on("/save-finance", HTTP_POST, []() { WEB_AUTH
    FinanceAsset input[FINANCE_MAX_ASSETS];
    uint8_t count = 0;
    String invalid;
    for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) {
      String sym = configServer.arg(String("sym") + String(i));
      String lbl = configServer.arg(String("lbl") + String(i));
      sym.trim();
      lbl.trim();
      if (sym.isEmpty()) continue;
      FinanceAsset a;
      if (!normalizeFinanceSymbol(sym.c_str(), a.symbol, sizeof a.symbol)) {
        invalid += String(invalid.isEmpty() ? "" : ", ") + sym;
        continue;
      }
      snprintf(a.label, sizeof a.label, "%s", lbl.c_str());
      input[count++] = a;
    }
    String notice;
    if (!count) notice = "Nenhum símbolo válido; lista mantida.";
    else if (!setFinanceAssets(input, count)) notice = "Não foi possível salvar a lista.";
    else {
      notice = String("Finance salvo com ") + String(count) + (count == 1 ? " ativo." : " ativos.");
      if (activeScreenId() == SCREEN_FINANCE) postScreenJob(CMD_FINANCE);
      else notice += " Abra a tela Finance para consultar.";
    }
    if (!invalid.isEmpty()) notice += " Ignorados (caracteres inválidos): " + invalid + ".";
    sendConfigPage(notice);
  });
  configServer.on("/save-servers", HTTP_POST, []() { WEB_AUTH
    String user = configServer.arg("bzuser"); user.trim();
    String pass = configServer.arg("bzpass");
    serversSetCredentials(user.c_str(), pass.c_str());
    String notice = "Servidores salvos.";
    for (uint8_t i = 0; i < SERVERS_MAX; ++i) {
      String lbl = configServer.arg("bzlbl" + String(i)); lbl.trim();
      char url[64] = "", id[24] = "";
      if (!serversParseLink(configServer.arg("bzlink" + String(i)).c_str(), url, sizeof url, id, sizeof id)) continue;
      if (!id[0]) notice += " Servidor " + String(i + 1) + ": link sem /system/ID (ID mantido).";
      serversSetServer(i, lbl.c_str(), url, id);
    }
    if (activeScreenId() == SCREEN_SERVERS) postScreenJob(CMD_SERVERS);
    else notice += " Abra a tela Servers para consultar.";
    sendConfigPage(notice);
  });
  configServer.on("/save-football", HTTP_POST, []() { WEB_AUTH
    footballSetFavorites(configServer.arg("fav").c_str());
    sendConfigPage(String("Futebol: favoritos ") + (footballFavorites().length() ? footballFavorites() : String("(nenhum)")) + ".");
  });
  configServer.on("/save-display", HTTP_POST, []() { WEB_AUTH
    const int mhz = configServer.arg("qspi").toInt();
    if (mhz == 40 || mhz == 60 || mhz == 80) prefs.putUInt("qspiHz", (uint32_t)mhz * 1000000UL);
    configServer.send(200, "text/html; charset=utf-8",
      "<meta http-equiv=refresh content='8;url=/'><body style='font-family:sans-serif;background:#0c1728;color:#fff;padding:30px'>"
      "Salvo. Reiniciando a placa com QSPI " + String(mhz) + " MHz...</body>");
    delay(400);
    ESP.restart();
  });
  configServer.on("/log", HTTP_GET, []() { WEB_AUTH sendLogPage(); });
  // ---- Photos: the page crops/resizes in the browser and sends 480x320 JPEGs
  configServer.on("/photos", HTTP_GET, []() { WEB_AUTH configServer.send_P(200, "text/html; charset=utf-8", PHOTOS_PAGE); });
  configServer.on("/photo", HTTP_GET, []() { WEB_AUTH
    const int slot = configServer.arg("slot").toInt();
    char p[24]; photoPath(slot, p, sizeof p);
    if (!photoExists(slot)) { configServer.send(404, "text/plain", ""); return; }
    File f = LittleFS.open(p, "r");
    configServer.sendHeader("Cache-Control", "no-store");
    configServer.streamFile(f, "image/jpeg");
    f.close();
  });
  configServer.on("/photo-delete", HTTP_POST, []() { WEB_AUTH
    const int slot = configServer.arg("slot").toInt();
    char p[24]; photoPath(slot, p, sizeof p);
    if (photoExists(slot)) LittleFS.remove(p);
    photosChanged();
    Log.printf("Fotos: foto %d apagada\n", slot + 1);
    configServer.send(200, "text/plain", "ok");
  });
  static File photoUp; static bool photoUpOk = false; static size_t photoUpBytes = 0;
  configServer.on("/photo-upload", HTTP_POST, []() { WEB_AUTH
    const int slot = configServer.arg("slot").toInt();
    char p[24], tmp[28]; photoPath(slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!photoUpOk) { LittleFS.remove(tmp); configServer.send(500, "text/plain", "sem espaço ou falha ao gravar"); return; }
    LittleFS.remove(p); LittleFS.rename(tmp, p);
    photosChanged();
    Log.printf("Fotos: foto %d recebida (%u KB, livre %u KB)\n", slot + 1, (unsigned)(photoUpBytes / 1024),
               (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
    configServer.send(200, "text/plain", "ok");
  }, []() {
    if (!webAuthCheck(configServer)) { photoUpOk = false; return; }   // no session: ignore the data
    HTTPUpload &up = configServer.upload();
    const int slot = configServer.arg("slot").toInt();
    char p[24], tmp[28]; photoPath(slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (up.status == UPLOAD_FILE_START) {
      photoUpBytes = 0;
      photoUpOk = slot >= 0 && slot < PHOTO_SLOTS && photosFsReady();
      if (photoUpOk) { photoUp = LittleFS.open(tmp, "w"); photoUpOk = (bool)photoUp; }
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (photoUpOk) {
        photoUpOk = photoUp.write(up.buf, up.currentSize) == up.currentSize && photoUpBytes + up.currentSize < 400 * 1024;
        photoUpBytes += up.currentSize;
      }
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
      if (photoUp) photoUp.close();
      if (up.status == UPLOAD_FILE_ABORTED) photoUpOk = false;
    }
  });
  configServer.on("/log-verbose", HTTP_GET, []() { WEB_AUTH         // ?on=1 / ?on=0, saved
    gLogVerbose = configServer.arg("on") == "1";
    prefs.putBool("logVerbose", gLogVerbose);
    Log.printf("Log verbose %s\n", gLogVerbose ? "ativado" : "desativado");
    configServer.sendHeader("Location", "/log");
    configServer.send(302, "text/plain", "");
  });
  configServer.onNotFound([]() { configServer.send(404, "text/plain; charset=utf-8", "Nao encontrado"); });
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
