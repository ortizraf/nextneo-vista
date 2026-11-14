// Photos on the settings web page: link card + the /photos editor (crops in the
// browser, uploads 480x320 JPEGs to LittleFS).
#include <Arduino.h>
#include <WebServer.h>
#include "app_screens.h"
#include "core/web_ui.h"
#include <LittleFS.h>
#include "modules/photos/photos.h"
#include "services/app_log.h"

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

static void photosCard(String &html) {
  html += R"HTML(<div class="card"><h2>Fotos</h2><div class="muted">Até 8 fotos na tela Fotos · cada uma passa 30 s · recorte feito aqui no navegador</div>
<a class="btnlink" href="/photos">Gerenciar fotos</a></div>
)HTML";
}

static void photosRoutes(WebServer &server) {
  // ---- the page crops/resizes in the browser and sends 480x320 JPEGs
  server.on("/photos", HTTP_GET, [&server]() { WEB_REQUIRE_LOGIN(server) server.send_P(200, "text/html; charset=utf-8", PHOTOS_PAGE); });
  server.on("/photo", HTTP_GET, [&server]() { WEB_REQUIRE_LOGIN(server)
    const int slot = server.arg("slot").toInt();
    char p[24]; photoPath(slot, p, sizeof p);
    if (!photoExists(slot)) { server.send(404, "text/plain", ""); return; }
    File f = LittleFS.open(p, "r");
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "image/jpeg");
    f.close();
  });
  server.on("/photo-delete", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    const int slot = server.arg("slot").toInt();
    char p[24]; photoPath(slot, p, sizeof p);
    if (photoExists(slot)) LittleFS.remove(p);
    photosChanged();
    Log.printf("Fotos: foto %d apagada\n", slot + 1);
    server.send(200, "text/plain", "ok");
  });
  static File photoUp; static bool photoUpOk = false; static size_t photoUpBytes = 0;
  server.on("/photo-upload", HTTP_POST, [&server]() { WEB_REQUIRE_LOGIN(server)
    const int slot = server.arg("slot").toInt();
    char p[24], tmp[28]; photoPath(slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!photoUpOk) { LittleFS.remove(tmp); server.send(500, "text/plain", "sem espaço ou falha ao gravar"); return; }
    LittleFS.remove(p); LittleFS.rename(tmp, p);
    photosChanged();
    Log.printf("Fotos: foto %d recebida (%u KB, livre %u KB)\n", slot + 1, (unsigned)(photoUpBytes / 1024),
               (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
    server.send(200, "text/plain", "ok");
  }, [&server]() {
    if (!webAuthCheck(server)) { photoUpOk = false; return; }   // no session: ignore the data
    HTTPUpload &up = server.upload();
    const int slot = server.arg("slot").toInt();
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
}

const WebSection kPhotosWeb = {"photos", photosCard, photosRoutes};
