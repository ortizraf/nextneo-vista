// Finance on the settings web page: the list of Yahoo Finance assets.
#include <Arduino.h>
#include <WebServer.h>
#include "app_screens.h"
#include "core/web_ui.h"
#include "modules/finance/finance.h"

static void financeCard(String &html) {
  xSemaphoreTake(mtx, portMAX_DELAY);
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
  String rows;
  for (uint8_t i = 0; i < assetCount; ++i) {
    String info;
    if (havePrice[i]) {
      char b[40];
      snprintf(b, sizeof b, "%s %.4g", currency[i], price[i]);
      info = b;
    } else if (apiKnown[i] && !apiOk[i]) info = "sem cotação (símbolo válido?)";
    else info = "aguardando consulta";
    rows += "<div class=\"asset\"><input name=\"sym\" value=\"" + webEscape(assets[i].symbol) +
            "\" placeholder=\"AAPL\" maxlength=\"15\" autocapitalize=\"characters\"><input name=\"lbl\" value=\"" +
            webEscape(assets[i].label) + "\" placeholder=\"Legenda (opcional)\" maxlength=\"31\">"
            "<button type=\"button\" class=\"del\" onclick=\"delRow(this)\" title=\"Remover\">&times;</button>"
            "<span class=\"info\">" + webEscape(info) + "</span></div>";
  }

  String c = R"HTML(<form class="card" method="post" action="/save-finance" accept-charset="UTF-8" onsubmit="return prep()"><h2>Finance</h2>
<div class="muted">Até 8 ativos. Use o símbolo do <b>Yahoo Finance</b>.</div>
<div id="assets">%ROWS%</div>
<div class="chips" id="chips"></div>
<button type="button" class="sec" onclick="addRow('','')">+ Adicionar ativo</button>
<button type="submit">Salvar Finance</button>
<small>Exemplos: moedas <code>USDBRL=X</code>, <code>GBPBRL=X</code>; cripto <code>BTC-USD</code>, <code>ETH-USD</code>; ações dos EUA <code>AAPL</code>, <code>MSFT</code>; B3 com <code>.SA</code>: <code>PETR4.SA</code>, <code>VALE3.SA</code>; índices <code>^BVSP</code> (Ibovespa), <code>^GSPC</code> (S&amp;P 500). Confira o símbolo em finance.yahoo.com.</small></form>
<script>
const MAX=%MAX%;
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
</script>)HTML";
  c.replace("%ROWS%", rows);
  c.replace("%MAX%", String(FINANCE_MAX_ASSETS));
  html += c;
}

static void financeRoutes(WebServer &server) {
  server.on("/save-finance", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    FinanceAsset input[FINANCE_MAX_ASSETS];
    uint8_t count = 0;
    String invalid;
    for (uint8_t i = 0; i < FINANCE_MAX_ASSETS; ++i) {
      String sym = server.arg(String("sym") + String(i));
      String lbl = server.arg(String("lbl") + String(i));
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
      if (activeScreenId() == SCREEN_FINANCE) postScreenJob(JOB_UPDATE);
      else notice += " Abra a tela Finance para consultar.";
    }
    if (!invalid.isEmpty()) notice += " Ignorados (caracteres inválidos): " + invalid + ".";
    webSendSettings(server, notice);
  });
}

const WebSection kFinanceWeb = {"finance", financeCard, financeRoutes};
