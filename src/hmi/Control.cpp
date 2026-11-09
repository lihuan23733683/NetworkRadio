// ============================================================================
//  Control.cpp -- 局域网网页控制台实现
//  播放控制 + 音量 + 换台 + 电台管理（添加/编辑/删除/批量导入/导出，存文件）
//  列表支持搜索框 + 分页（每页 20 条），全量列表仅在打开/变更时请求一次
// ============================================================================
#include "Control.h"

#include <WebServer.h>
#include <WiFi.h>

#include "../core/AppConfig.h"
#include "../core/EventLog.h"
#include "../audio/Player.h"
#include "../audio/Stations.h"

// 主程序（NetworkRadio.ino）里的全局符号
extern void    playStation(uint8_t idx);
extern uint8_t g_station;
extern bool    portalActive();
extern Player  player;

static WebServer ctrlServer(CTRL_PORT);

static String jsonEsc(const char* s) {
    String r;
    for (; *s; ++s) {
        char c = *s;
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else r += c;
    }
    return r;
}

static const uint8_t FAVICON_PNG[] PROGMEM = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,64,0,0,0,64,8,6,0,0,0,170,105,113,222,0,0,1,231,73,68,65,84,120,156,237,155,77,142,195,32,12,133,205,19,231,104,119,51,199,107,143,50,115,188,206,142,94,164,163,46,34,101,80,18,176,129,96,195,124,139,74,32,32,126,230,199,16,26,71,12,46,183,199,139,12,240,252,254,116,185,101,221,40,162,165,206,192,200,226,115,52,56,73,165,145,70,3,102,17,191,167,13,169,2,163,17,107,244,100,144,240,245,241,39,125,189,255,136,219,130,181,222,143,197,239,229,29,177,214,138,56,67,51,71,66,165,78,0,77,14,104,114,48,194,240,151,114,185,61,94,126,20,225,210,72,0,154,88,252,27,111,73,120,137,80,51,35,32,156,40,94,213,8,56,91,184,26,7,132,131,121,222,90,124,119,7,244,234,245,238,14,8,10,132,119,91,4,131,34,241,167,142,0,109,194,23,156,100,43,220,98,91,90,11,174,67,49,146,120,137,125,104,217,120,47,56,118,162,69,163,26,200,181,23,53,27,211,70,142,221,158,140,17,47,114,165,157,227,107,25,179,54,132,155,207,125,214,86,190,212,17,16,213,218,48,104,73,115,243,37,207,146,150,25,98,10,132,138,119,2,42,223,7,196,164,166,205,222,20,107,238,128,16,25,179,164,185,249,189,241,37,149,247,68,112,243,123,226,173,68,129,86,64,123,20,72,205,241,82,167,122,50,70,237,227,51,200,0,57,61,123,250,70,40,156,28,5,222,229,247,194,96,201,122,226,114,94,136,104,89,176,90,76,25,95,171,241,255,40,112,111,119,22,104,137,39,67,108,57,173,116,122,130,140,144,58,14,75,129,133,40,144,18,89,226,4,167,61,10,112,196,109,217,153,170,15,154,28,208,228,128,148,147,59,253,154,110,133,175,157,99,118,74,92,201,189,35,114,141,208,234,132,210,75,87,199,189,28,181,112,46,224,116,22,90,54,222,3,174,125,238,253,99,229,223,162,45,190,32,1,77,14,104,114,192,253,206,110,20,22,205,136,51,102,96,173,21,52,57,88,39,102,24,5,177,70,164,10,140,196,150,54,119,84,97,148,253,193,81,167,66,90,209,10,41,13,142,211,152,149,17,193,233,184,95,195,75,40,70,191,53,81,94,0,0,0,0,73,69,78,68,174,66,96,130};

static const char PAGE[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>📻 网络收音机</title>
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAB50lEQVR4nO2bTY7DIAyFzRPnaHczx2uPMnO8zo5epKMuImVQErCBYMN8i0ogIH7mxxAaRwwut8eLDPD8/nS5Zd0ooqXOwMjiczQ4SaWRRgNmEb+nDakCoxFr9GSQ8PXxJ329/4jbgrXej8Xv5R2x1oo4QzNHQqVOAE0OaHIwwvCXcrk9Xn4U4dJIAJpY/BtvSXiJUDMjIJwoXtUIOFu4GgeEg3neWnx3B/Tq9e4OCAqEd1sEgyLxp44AbcIXnGQr3GJbWguuQzGSeIl9aNl4Lzh2okWjGsi1FzUb00aO3Z6MES9ypZ3jaxmzNoSbz33WVr7UERDV2jBoSXPzJc+SlhliCoSKdwIq3wfEpKbN3hRr7oAQGbOkufm98SWV90Rw83virUSBVkB7FEjN8VKnejJG7eMzyAA5PXv6RiicHAXe5ffCYMl64nJeiGhZsFpMGV+r8f8ocG93FmiJJ0NsOa10eoKMkDoOS4GFKJASWeIEpz0KcMRt2ZmqD5oc0OSAlJM7/Zpuha+dY3ZKXMm9I3KN0OqE0ktXx70ctXAu4HQWWjbeA6597v1j5d+iLb4gAU0OaHLA/c5uFBbNiDNmYK0VNDlYJ2YYBbFGpAqMxJY2d1RhlP3BUadCWtEKKQ2O05iVEcHpuF/DSyhGvzVRXgAAAABJRU5ErkJggg==">
<style>
body{margin:0;padding:18px 14px 40px;background:#101418;color:#e8eaed;
     font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:430px;margin:0 auto}
h1{font-size:19px;font-weight:600;margin:4px 0 2px}
.sub{font-size:12px;color:#9aa0a6;margin-bottom:16px}
.card{background:#1b2129;border-radius:14px;padding:16px 14px;margin-bottom:14px}
#now{font-size:17px;font-weight:600}
#st{font-size:12.5px;color:#8ab4f8;margin-top:4px}
.row{display:flex;gap:10px;margin-top:12px}
button{flex:1;height:52px;border:0;border-radius:11px;font-size:15px;font-weight:600;
       background:#2b323c;color:#e8eaed;cursor:pointer}
button:active{background:#3a4450}
button.pri{background:#1a73e8;color:#fff}
button.pri:active{background:#3b82f0}
button.sm{height:44px;font-size:16px;flex:0 0 64px}
#volVal{font-size:22px;font-weight:600;text-align:center;width:52px;line-height:44px}
input[type=range]{width:100%;height:40px;margin:6px 0 0;accent-color:#1a73e8}
.station{display:block;width:100%;text-align:left;height:46px;
         font-weight:400;font-size:14.5px;padding:0 14px;box-sizing:border-box}
.station.cur{background:#1a73e8;color:#fff}
.strow{display:flex;gap:6px;margin-top:8px}
.strow .station{flex:1}
.sbtn{flex:0 0 46px;height:46px;font-weight:400;font-size:15px;padding:0}
.sbtn.del{color:#f28b82}
.mut{background:none;border:0;color:#9aa0a6;font-size:13px;height:auto;flex:0 0 auto;
     padding:0 6px;text-decoration:underline}
input[type=text],textarea{width:100%;box-sizing:border-box;margin-top:8px;
     border:1px solid #3a4450;border-radius:10px;background:#12161b;color:#e8eaed;
     padding:10px 12px;font-size:14px}
input[type=text]{height:44px;padding:0 12px}
input:focus,textarea:focus{outline:none;border-color:#1a73e8}
textarea{min-height:110px;resize:vertical;font-family:inherit;line-height:1.5}
#fmsg,#imsg{font-size:12.5px;margin-top:6px;min-height:16px}
.ferr,.ierr{color:#f28b82}.fok,.iok{color:#81c995}
#st .ehint{color:#f28b82;font-size:12.5px;margin-top:6px;line-height:1.5}
.hint{font-size:12px;color:#9aa0a6;margin-top:8px;line-height:1.6}
#srch{margin-bottom:2px}
.lhead{display:flex;justify-content:space-between;align-items:baseline;font-weight:600;font-size:14px}
.lcnt{font-size:11.5px;color:#9aa0a6;font-weight:400}
.pager{display:flex;gap:8px;margin-top:10px;align-items:center;justify-content:center}
.pager button{flex:0 0 auto;height:36px;padding:0 16px;font-size:13px}
.pinfo{font-size:12.5px;color:#9aa0a6;min-width:70px;text-align:center}
details{margin-top:10px}
summary{cursor:pointer;font-size:13.5px;font-weight:600;color:#8ab4f8}
a.exp{display:inline-block;font-size:12.5px;color:#8ab4f8;text-decoration:underline;margin-top:8px}
</style></head><body><div class="wrap">
<h1><img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAB50lEQVR4nO2bTY7DIAyFzRPnaHczx2uPMnO8zo5epKMuImVQErCBYMN8i0ogIH7mxxAaRwwut8eLDPD8/nS5Zd0ooqXOwMjiczQ4SaWRRgNmEb+nDakCoxFr9GSQ8PXxJ329/4jbgrXej8Xv5R2x1oo4QzNHQqVOAE0OaHIwwvCXcrk9Xn4U4dJIAJpY/BtvSXiJUDMjIJwoXtUIOFu4GgeEg3neWnx3B/Tq9e4OCAqEd1sEgyLxp44AbcIXnGQr3GJbWguuQzGSeIl9aNl4Lzh2okWjGsi1FzUb00aO3Z6MES9ypZ3jaxmzNoSbz33WVr7UERDV2jBoSXPzJc+SlhliCoSKdwIq3wfEpKbN3hRr7oAQGbOkufm98SWV90Rw83virUSBVkB7FEjN8VKnejJG7eMzyAA5PXv6RiicHAXe5ffCYMl64nJeiGhZsFpMGV+r8f8ocG93FmiJJ0NsOa10eoKMkDoOS4GFKJASWeIEpz0KcMRt2ZmqD5oc0OSAlJM7/Zpuha+dY3ZKXMm9I3KN0OqE0ktXx70ctXAu4HQWWjbeA6597v1j5d+iLb4gAU0OaHLA/c5uFBbNiDNmYK0VNDlYJ2YYBbFGpAqMxJY2d1RhlP3BUadCWtEKKQ2O05iVEcHpuF/DSyhGvzVRXgAAAABJRU5ErkJggg==" style="width:22px;height:22px;vertical-align:-4px" alt=""> 网络收音机</h1>
<div class="sub" id="ip"></div>

<div class="card">
  <div id="now">--</div>
  <div id="st"></div>
  <div class="row">
    <button onclick="api('/prev')">&#9664; 上一台</button>
    <button class="pri" id="pp" onclick="api('/pause')">&#9199;</button>
    <button onclick="api('/next')">下一台 &#9654;</button>
  </div>
</div>

<div class="card">
  <div style="display:flex;align-items:center;gap:8px">
    <button class="sm" onclick="volStep(-1)">&#8722;</button>
    <div id="volVal">--</div>
    <button class="sm" onclick="volStep(1)">+</button>
    <button class="mut" id="mu" onclick="api('/mute')">静音</button>
  </div>
  <input type="range" id="vol" min="0" max="__MAXVOL__" value="12"
         oninput="volDrag=1" onchange="api('/vol?v='+this.value,1).then(()=>volDrag=0)">
</div>

<div class="card">
  <div class="lhead"><span>&#128251; 电台列表</span><span class="lcnt" id="lcnt"></span></div>
  <input type="text" id="srch" placeholder="&#128269; 搜索名称或地址（本地过滤，不联网）">
  <div id="list"></div>
  <div class="pager" id="pager" style="display:none">
    <button onclick="pg(-1)">&#8249; 上一页</button>
    <span class="pinfo" id="pinfo"></span>
    <button onclick="pg(1)">下一页 &#8250;</button>
  </div>
  <a class="exp" href="/sta/export">&#11014; 导出备份（M3U 文件）</a>
</div>

<div class="card">
  <div style="font-size:14px;font-weight:600" id="ftitle">&#10133; 添加自定义电台</div>
  <input type="text" id="fname" placeholder="电台名称，如：上海经典947" maxlength="15">
  <input type="text" id="furl" placeholder="流媒体地址（http:// 或 https:// 开头）">
  <div id="fmsg"></div>
  <div class="row">
    <button class="pri" id="fbtn" onclick="submitSta()">添加电台</button>
    <button id="fcancel" style="display:none" onclick="resetForm()">取消编辑</button>
  </div>
  <div class="hint">仅支持 HTTP / HTTPS 协议的直播流（mp3、aac、m3u8、pls 等），rtmp、rtsp、udp 等协议会被拒绝并提示不支持。自定义电台保存在设备里，断电不丢；内置电台不可编辑或删除。</div>

  <details>
    <summary>&#128230; 批量导入（每行一条：名称,URL 或 M3U 格式）</summary>
    <textarea id="itext" placeholder="两种格式可混用，每行一条：
上海经典947,http://ngcdn007.cnr.cn/live/zgzs/index.m3u8
#EXTM3U
#EXTINF:-1,音乐台
http://example.com/music.mp3
http://只有地址也行.com/stream.mp3"></textarea>
    <div id="imsg"></div>
    <div class="row">
      <button class="pri" onclick="doImport()">开始导入</button>
    </div>
    <div class="hint">英文或中文逗号分隔均可；名称省略时自动编号；重复地址自动跳过；超出容量上限自动停止。</div>
  </details>
</div>
</div>

<script>
let volDrag=0,cur=-1,editing=-1;
let LIST=[],BC=0,page=1;const PER=20;
const $=id=>document.getElementById(id);
async function api(u,skip){const r=await fetch(u);if(!skip)refresh();return r;}
function q(s){return encodeURIComponent(s)}
function msg(m,ok){const f=$('fmsg');f.textContent=m;f.className=ok?'fok':'ferr';}
function imsg(m,ok){const f=$('imsg');f.textContent=m;f.className=ok?'iok':'ierr';}
function resetForm(){editing=-1;$('fname').value='';$('furl').value='';msg('');
  $('ftitle').innerHTML='&#10133; 添加自定义电台';
  $('fbtn').textContent='添加电台';$('fcancel').style.display='none';}
function editSta(i,n,u){editing=i;
  $('ftitle').innerHTML='&#9999;&#65039; 编辑电台 #'+(i+1);
  $('fname').value=n;$('furl').value=u;msg('');
  $('fbtn').textContent='保存修改';$('fcancel').style.display='';
  window.scrollTo({top:document.body.scrollHeight,behavior:'smooth'});}
async function submitSta(){
  const name=$('fname').value.trim(),url=$('furl').value.trim();
  if(!name){msg('请填写电台名称');return;}
  if(!url){msg('请填写流媒体地址');return;}
  const lu=url.toLowerCase();
  if(!lu.startsWith('http://')&&!lu.startsWith('https://')){
    msg('不支持该流媒体协议：仅支持 http:// 或 https:// 开头的直播流');return;}
  const ep=editing>=0?('/sta/edit?n='+editing):'/sta/add?';
  try{const j=await(await fetch(ep+'&name='+q(name)+'&url='+q(url))).json();
    if(j.ok){resetForm();msg('已保存',1);loadList();}else msg(j.err||'操作失败');}
  catch(e){msg('请求失败，请重试');}
}
async function delSta(i){
  if(!confirm('确定删除电台 #'+(i+1)+' ？'))return;
  try{const j=await(await fetch('/sta/del?n='+i)).json();
    if(!j.ok)alert(j.err||'删除失败');
    else{if(editing>=i)resetForm();loadList();}}catch(e){alert('请求失败');}
}
async function doImport(){
  const t=$('itext').value.trim();
  if(!t){imsg('请先粘贴要导入的内容');return;}
  imsg('导入中…');
  try{const j=await(await fetch('/sta/import',{method:'POST',body:t})).json();
    if(j.ok){imsg('导入完成：新增 '+j.added+' 个'+(j.dup?'，重复跳过 '+j.dup:'')+
      (j.bad?'，格式错误 '+j.bad:'')+(j.trunc?'，名称过长已截断 '+j.trunc+' 个':''),1);
      $('itext').value='';loadList();}
    else imsg(j.err||'导入失败');}
  catch(e){imsg('请求失败，请重试');}
}
function volStep(d){
  const el=$('volVal');
  let v=parseInt(el.textContent);if(isNaN(v))v=12;
  v=Math.max(0,Math.min(__MAXVOL__,v+d));
  el.textContent=v;$('vol').value=v;
  fetch('/vol?d='+(d>0?1:-1));
}
function filtered(){
  const k=$('srch').value.trim().toLowerCase();
  if(!k)return LIST.map((o,i)=>Object.assign({i:i},o));
  return LIST.map((o,i)=>Object.assign({i:i},o))
    .filter(o=>o.n.toLowerCase().includes(k)||o.u.toLowerCase().includes(k));
}
function pg(d){
  const n=filtered().length,mx=Math.max(1,Math.ceil(n/PER));
  page=Math.min(mx,Math.max(1,page+d));render();
}
function render(){
  const items=filtered(),total=items.length;
  const mx=Math.max(1,Math.ceil(total/PER));
  if(page>mx)page=mx;
  const curName=LIST[cur]?LIST[cur].n:'';
  $('lcnt').textContent=(total===LIST.length?LIST.length:total+'/'+LIST.length)+' 个';
  const L=$('list');L.innerHTML='';
  items.slice((page-1)*PER,page*PER).forEach(o=>{
    const row=document.createElement('div');row.className='strow';
    const b=document.createElement('button');
    b.className='station'+(o.i===cur?' cur':'');
    b.textContent=(o.i+1)+'. '+o.n;b.onclick=()=>api('/play?n='+o.i);
    row.appendChild(b);
    if(!o.r){
      const e=document.createElement('button');e.className='sbtn';e.innerHTML='&#9998;';
      e.title='编辑';e.onclick=()=>editSta(o.i,o.n,o.u);row.appendChild(e);
      const d=document.createElement('button');d.className='sbtn del';d.innerHTML='&#128465;';
      d.title='删除';d.onclick=()=>delSta(o.i);row.appendChild(d);}
    L.appendChild(row);});
  const pager=$('pager');
  if(mx>1){pager.style.display='flex';
    $('pinfo').textContent='第 '+page+' / '+mx+' 页';}
  else pager.style.display='none';
}
$('srch').addEventListener('input',()=>{page=1;render();});
async function loadList(){
  try{const j=await(await fetch('/api/list')).json();
    LIST=j.items;BC=j.bc;page=1;render();}
  catch(e){}
}
async function refresh(){
  try{
    const s=await (await fetch('/api/status')).json();
    if(!volDrag)$('vol').value=s.vol;
    $('volVal').textContent=s.vol;
    $('now').textContent=s.name||'未播放';
    const stEl=$('st');
    stEl.innerHTML=
      (s.playing?'&#9654; 播放中':'&#10074;&#10074; 已暂停')+'  RSSI '+s.rssi+' dBm';
    if(s.err){const d=document.createElement('div');d.className='ehint';
      d.textContent='！'+s.err;stEl.appendChild(d);}
    $('pp').innerHTML=s.playing?'&#10074;&#10074;':'&#9654;';
    $('mu').textContent=s.mute?'已静音(点此恢复)':'静音';
    $('ip').textContent='http://'+s.ip+'/';
    if(s.station!==cur){cur=s.station;render();}
  }catch(e){}
}
loadList();refresh();
setInterval(()=>{if(!volDrag)refresh();},2500);
</script></body></html>)html";

void Control::begin(NetConfig* net) {
    _net = net;
    if (_routeDone) return;
    _routeDone = true;

    ctrlServer.on("/", HTTP_GET, [this]() { handleRoot(); });
    ctrlServer.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
    ctrlServer.on("/api/list", HTTP_GET, [this]() { handleList(); });
    ctrlServer.on("/play", HTTP_GET, [this]() { handlePlay(); });
    ctrlServer.on("/next", HTTP_GET, [this]() { handleStep(1); });
    ctrlServer.on("/prev", HTTP_GET, [this]() { handleStep(-1); });
    ctrlServer.on("/vol", HTTP_GET, [this]() { handleVol(); });
    ctrlServer.on("/mute", HTTP_GET, [this]() { handleMutePause(); });
    ctrlServer.on("/pause", HTTP_GET, [this]() { handleMutePause(); });
    ctrlServer.on("/sta/add",  HTTP_GET, [this]() { handleStaAdd(); });
    ctrlServer.on("/sta/edit", HTTP_GET, [this]() { handleStaEdit(); });
    ctrlServer.on("/sta/del",  HTTP_GET, [this]() { handleStaDel(); });
    ctrlServer.on("/sta/import", HTTP_POST, [this]() { handleImport(); });
    ctrlServer.on("/sta/export", HTTP_GET, [this]() { handleExport(); });
    ctrlServer.on("/api/diag", HTTP_GET, [this]() { handleDiag(); });
    ctrlServer.on("/api/events", HTTP_GET, [this]() {
        ctrlServer.send(200, "application/json; charset=utf-8", EvLog::toJson());
    });
    ctrlServer.on("/favicon.ico", HTTP_GET, [this]() {
        ctrlServer.send_P(200, "image/x-icon", (const char*)FAVICON_PNG, sizeof(FAVICON_PNG));
    });
    ctrlServer.onNotFound([this]() { sendErr("not found"); });
}

void Control::loop() {
    if (!_routeDone || _net == nullptr) return;
    if (portalActive()) return;                 // 配网模式下挂起
    if (!_net->isConnected()) { _begun = false; return; }

    if (!_begun) { ctrlServer.begin(); _begun = true; }
    ctrlServer.handleClient();
    announce();
}

void Control::announce() {
    if (_announced) return;
    _announced = true;
    Serial.println();
    Serial.printf("[网页] 控制台已就绪:  http://%s/   (手机连同一 WiFi 访问)\n",
                  WiFi.localIP().toString().c_str());
}

// 进配网门户前释放 80 端口：否则门户的网页服务绑定失败，
// 手机连上热点后 captive portal 探测无人应答，不会自动弹配置页
void Control::pauseServer() {
    if (_begun) {
        ctrlServer.stop();
        _begun = false;
        Serial.println("[网页] 控制台已暂停，让出 80 端口给配网门户");
    }
}

// ---------------------------------------------------------------------------

void Control::handleRoot() {
    String p = PAGE;
    p.replace("__MAXVOL__", String(MAX_VOLUME));
    ctrlServer.send(200, "text/html; charset=utf-8", p);
}

void Control::handleStatus() {
    bool playing = (player.state() == Player::ST_PLAYING);
    String s = "{";
    s += "\"station\":" + String(g_station);
    s += ",\"name\":\"" + jsonEsc(player.label().length() ? player.label().c_str() : "未播放") + "\"";
    s += ",\"playing\":" + String(playing ? "true" : "false");
    s += ",\"vol\":" + String(player.muted() ? 0 : player.volume());
    s += ",\"mute\":" + String(player.muted() ? "true" : "false");
    s += ",\"rssi\":" + String(WiFi.RSSI());
    s += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    s += ",\"count\":" + String(Stations::count());
    s += ",\"bc\":" + String(Stations::builtinCount());
    // 播放故障说明（地址失效等）：前端在状态行下方用红字提示
    s += ",\"err\":\"" + jsonEsc(player.lastError().c_str()) + "\"";
    s += "}";   // 列表改由 /api/list 提供，状态轮询保持轻量
    ctrlServer.send(200, "application/json; charset=utf-8", s);
}

// 全量列表：打开页面 / 列表变更时请求一次，前端分页 + 本地搜索
void Control::handleList() {
    uint8_t n  = Stations::count();
    uint8_t bc = Stations::builtinCount();

    String s = "{\"bc\":" + String(bc) + ",\"items\":[";
    for (uint8_t i = 0; i < n; ++i) {
        const Station& st = Stations::get(i);
        if (i) s += ",";
        s += "{\"n\":\"" + jsonEsc(st.name) + "\",\"u\":\"" + jsonEsc(st.url) + "\"";
        s += ",\"r\":" + String(i < bc ? "1" : "0") + "}";
    }
    s += "]}";
    ctrlServer.send(200, "application/json; charset=utf-8", s);
}

void Control::handlePlay() {
    if (!ctrlServer.hasArg("n")) { sendErr("need n"); return; }
    int n = ctrlServer.arg("n").toInt();
    if (n < 0 || n >= Stations::count()) { sendErr("bad index"); return; }
    playStation((uint8_t)n);
    sendOk();
}

void Control::handleStep(int dir) {
    if (Stations::count() == 0) { sendErr("no station"); return; }
    int n = (g_station + dir + Stations::count()) % Stations::count();
    playStation((uint8_t)n);
    sendOk();
}

void Control::handleVol() {
    if (ctrlServer.hasArg("v")) {
        int v = ctrlServer.arg("v").toInt();
        if (v < 0) v = 0;
        if (v > MAX_VOLUME) v = MAX_VOLUME;
        player.setMute(false);
        player.setVolume((uint8_t)v);
    } else if (ctrlServer.hasArg("d")) {
        int d = ctrlServer.arg("d").toInt();
        if (d < 0 && player.muted()) player.setMute(false);
        else player.volumeDelta(d > 0 ? +1 : -1);
    }
    sendOk();
}

void Control::handleMutePause() {
    if (ctrlServer.uri() == "/mute") player.setMute(!player.muted());
    else                             player.togglePause();
    sendOk();
}

// ---------------------------------------------------------------------------
//  自定义电台管理
// ---------------------------------------------------------------------------
void Control::handleStaAdd() {
    String name = ctrlServer.arg("name");
    String url  = ctrlServer.arg("url");
    String err;
    if (!Stations::addCustom(name, url, err)) { sendErr(err); return; }
    Serial.printf("[网页] 添加电台: %s  %s\n", name.c_str(), url.c_str());
    sendOk();
}

void Control::handleStaEdit() {
    if (!ctrlServer.hasArg("n")) { sendErr("need n"); return; }
    int n = ctrlServer.arg("n").toInt();
    if (n < 0 || n >= Stations::count()) { sendErr("bad index"); return; }
    String name = ctrlServer.arg("name");
    String url  = ctrlServer.arg("url");
    String err;
    if (!Stations::editCustom((uint8_t)n, name, url, err)) { sendErr(err); return; }
    Serial.printf("[网页] 编辑电台 #%d: %s\n", n + 1, name.c_str());
    sendOk();
}

void Control::handleStaDel() {
    if (!ctrlServer.hasArg("n")) { sendErr("need n"); return; }
    int n = ctrlServer.arg("n").toInt();
    if (n < 0 || n >= Stations::count()) { sendErr("bad index"); return; }
    String err;
    if (!Stations::delCustom((uint8_t)n, err)) { sendErr(err); return; }

    // 修正当前电台索引：删掉前面的台时当前索引前移；越界时夹回最后一个
    if ((uint8_t)n < g_station) g_station--;
    if (Stations::count() > 0 && g_station >= Stations::count()) {
        g_station = Stations::count() - 1;
    }
    Stations::saveIndex(g_station);
    Serial.printf("[网页] 删除电台 #%d，当前电台 -> #%u\n", n + 1, g_station + 1);
    sendOk();
}

void Control::handleImport() {
    String text = ctrlServer.arg("plain");   // POST 原始正文
    text.trim();
    if (!text.length()) { sendErr("没有收到导入内容"); return; }

    int dup = 0, bad = 0, trunc = 0;
    int added = Stations::importText(text, dup, bad, trunc);
    Serial.printf("[网页] 批量导入: 新增 %d，重复跳过 %d，格式错误 %d，名称截断 %d\n",
                  added, dup, bad, trunc);

    if (added == 0 && dup == 0 && bad == 0) { sendErr("没有解析到任何电台"); return; }
    String s = "{\"ok\":true,\"added\":" + String(added);
    s += ",\"dup\":" + String(dup) + ",\"bad\":" + String(bad);
    s += ",\"trunc\":" + String(trunc) + "}";
    ctrlServer.send(200, "application/json; charset=utf-8", s);
}

void Control::handleExport() {
    String m3u = Stations::exportM3U();
    ctrlServer.sendHeader("Content-Disposition",
                          "attachment; filename=stations_backup.m3u");
    ctrlServer.send(200, "audio/x-mpegurl; charset=utf-8", m3u);
}

// 音频诊断接口：无声故障时远程取证（不必打开串口，串口会重启设备冲掉现场）
// 判读思路：
//   fill=0             -> 服务器不给数据（网络/会话问题）
//   fill 恒定不变       -> 解码任务卡死（数据没人消费）
//   fill 在变但 vu=0    -> 解码正常但 I2S 输出异常（硬件/驱动）
//   stalls 递增         -> 看门狗反复介入，说明源不稳定
void Control::handleDiag() {
    String s = "{";
    s += String("\"state\":\"") + Player::stateName(player.state()) + "\"";
    s += ",\"fill\":" + String(player.inFilled());
    s += ",\"insize\":" + String(player.inSize());
    s += ",\"vu\":" + String(player.vuLevel());
    s += ",\"bitrate\":" + String(player.bitRate());
    s += ",\"rate\":" + String(player.sampleRate());
    s += ",\"bits\":" + String(player.bitsPerSample());
    s += ",\"ch\":" + String(player.channels());
    s += ",\"retries\":" + String(player.retries());
    s += ",\"fail\":" + String(player.failStreak());
    s += ",\"err\":\"" + jsonEsc(player.lastError().c_str()) + "\"";
    s += ",\"stalls\":" + String(player.stallCount());
    s += ",\"clock\":" + String(player.audioClock());
    s += ",\"run\":" + String(player.libRunning() ? "true" : "false");
    s += ",\"rst\":\"" + String(EvLog::resetReasonText()) + "\"";
    s += ",\"uptimeS\":" + String(millis() / 1000);
    s += ",\"playingS\":" + String(player.playingForMs() / 1000);
    s += ",\"rssi\":" + String(WiFi.RSSI());
    s += ",\"heap\":" + String(ESP.getFreeHeap());
    s += ",\"vol\":" + String(player.volume());
    s += ",\"mute\":" + String(player.muted() ? "true" : "false");
    s += ",\"sta\":\"" + jsonEsc(player.label().c_str()) + "\"";
    s += "}";
    ctrlServer.send(200, "application/json; charset=utf-8", s);
}

void Control::sendOk() { ctrlServer.send(200, "application/json", "{\"ok\":true}"); }

void Control::sendErr(const String& m) {
    ctrlServer.send(400, "application/json; charset=utf-8",
                    String("{\"ok\":false,\"err\":\"") + jsonEsc(m.c_str()) + "\"}");
}
