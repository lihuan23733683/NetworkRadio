// ============================================================================
//  Portal.cpp  --  手机配网门户实现
// ============================================================================
#include "Portal.h"

#include <DNSServer.h>
#include <WebServer.h>

#include "../core/AppConfig.h"
#include "NetConfig.h"

static WebServer g_web(80);
static DNSServer g_dns;
static const byte DNS_PORT = 53;

// ---------------------------------------------------------------------------
//  配置页面（存 flash，不占 RAM）
// ---------------------------------------------------------------------------
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>网络收音机 · 配网</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:22px 16px;min-height:100vh;background:#f2f4f8;color:#1a1d24;
font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
display:flex;justify-content:center;align-items:flex-start}
.card{width:100%;max-width:420px;background:#fff;border-radius:18px;padding:26px 22px 22px;
box-shadow:0 8px 28px rgba(20,30,60,.08)}
.badge{display:inline-block;font-size:12px;color:#2563eb;background:#e8f0fe;
border-radius:999px;padding:4px 10px;margin-bottom:14px}
h1{font-size:21px;margin:0 0 8px}
.sub{font-size:13px;color:#6b7280;margin:0 0 8px;line-height:1.65}
label{display:block;font-size:13px;font-weight:600;color:#374151;margin:18px 0 8px}
input{width:100%;height:46px;border:1.5px solid #e2e6ee;border-radius:11px;padding:0 13px;
font-size:15px;background:#fbfcfe;outline:none;transition:.15s}
input:focus{border-color:#2563eb;background:#fff;box-shadow:0 0 0 3px rgba(37,99,235,.1)}
button{width:100%;height:48px;border:0;border-radius:11px;font-size:16px;font-weight:600;
color:#fff;background:#2563eb;margin-top:22px;transition:.15s}
button:active{transform:scale(.985);background:#1d4ed8}
button:disabled{background:#c7cdd8}
.msg{display:none;font-size:13.5px;line-height:1.55;border-radius:10px;padding:11px 13px;margin-top:16px}
.msg.show{display:block}
.msg.info{background:#eef4ff;color:#1e40af}
.msg.ok{background:#e9f9ef;color:#047857}
.msg.err{background:#fdecec;color:#b91c1c}
.hint{font-size:12.5px;color:#9aa1ae;margin-top:14px;line-height:1.65}
.nets{display:flex;flex-wrap:wrap;gap:8px;margin-top:4px;min-height:34px}
.net{font-size:13px;border:1px solid #e2e6ee;background:#fbfcfe;border-radius:9px;
padding:7px 11px;display:flex;gap:7px;align-items:center;cursor:pointer}
.net:active{background:#eef4ff;border-color:#bfd3fb}
.rssi{font-size:11px;color:#9aa1ae}
</style></head>
<body><div class="card">
<div class="badge">ESP32-S3 网络收音机</div>
<h1>配置 WiFi</h1>
<p class="sub">选择 2.4G 网络并输入密码。可先后配置多个网络（如家里、公司各一个），设备全部记住，连不上时自动换下一个。</p>

<label>可用网络 <span class="rssi" id="tip">搜索中…</span></label>
<div class="nets" id="nets"></div>

<label for="ssid">WiFi 名称</label>
<input id="ssid" placeholder="点击上方网络，或手动输入" autocapitalize="off" autocorrect="off" spellcheck="false">
<label for="pass">WiFi 密码</label>
<input id="pass" type="password" placeholder="输入密码">
<button id="btn" onclick="save()">保存并连接</button>
<div class="msg" id="msg"></div>
<p class="hint">仅支持 2.4GHz 频段，5G 热点不会出现。配置成功后设备自动重启。</p>
</div>
<script>
var $=function(id){return document.getElementById(id)};
function show(t,c){var m=$('msg');m.className='msg show '+c;m.textContent=t;}
function pick(n){$('ssid').value=n;$('pass').focus();}

fetch('/scan').then(function(r){return r.json()}).then(function(list){
  var box=$('nets');box.innerHTML='';
  if(!list.length){$('tip').textContent='没扫到，请手动输入';return;}
  $('tip').textContent=list.length+' 个';
  list.forEach(function(o){
    var d=document.createElement('div');
    d.className='net';
    d.onclick=function(){pick(o.n)};
    var a=document.createElement('span');a.textContent=o.n;
    var b=document.createElement('span');b.className='rssi';b.textContent=o.s;
    d.appendChild(a);d.appendChild(b);box.appendChild(d);
  });
}).catch(function(){$('tip').textContent='扫描失败，请手动输入';});

function save(){
  var ssid=$('ssid').value.trim(), pass=$('pass').value;
  if(!ssid){show('请先填写 WiFi 名称','err');return;}
  $('btn').disabled=true;$('btn').textContent='连接中…';
  show('正在连接「'+ssid+'」，最多等 20 秒…','info');
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
  .then(function(r){ if(!r.ok) throw 0; poll(); })
  .catch(function(){show('提交失败，请重试','err');
    $('btn').disabled=false;$('btn').textContent='保存并连接';});
}

function poll(){
  var t=setInterval(function(){
    fetch('/status').then(function(r){return r.json()}).then(function(j){
      if(j.s==='ok'){
        clearInterval(t);show('连接成功！IP '+j.ip+'，设备正在重启…','ok');
        $('btn').textContent='已完成';
      }else if(j.s==='fail'){
        clearInterval(t);show(j.msg||'连接失败，请检查密码','err');
        $('btn').disabled=false;$('btn').textContent='重新连接';
      }
    }).catch(function(){});
  },1000);
}
</script>
</body></html>)HTML";

// ---------------------------------------------------------------------------
//  路由
// ---------------------------------------------------------------------------
void Portal::setupRoutes() {
    g_web.on("/", HTTP_GET, [this]() { this->handleRoot(); });
    g_web.on("/scan", HTTP_GET, [this]() { this->handleScan(); });
    g_web.on("/save", HTTP_POST, [this]() { this->handleSave(); });
    g_web.on("/status", HTTP_GET, [this]() { this->handleStatus(); });
    g_web.onNotFound([this]() { this->handleNotFound(); });
}

void Portal::handleRoot() {
    g_web.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void Portal::handleScan() {
    // AP_STA 模式下可以扫描，约 3~6 秒
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        s.replace("\\", "\\\\");
        s.replace("\"", "\\\"");
        if (s.length() == 0) continue;
        if (json.length() > 1) json += ",";
        json += "{\"n\":\"";
        json += s;
        json += "\",\"s\":";
        json += String(WiFi.RSSI(i));
        json += "}";
    }
    json += "]";
    WiFi.scanDelete();
    if (json == "[") json = "[]";
    g_web.send(200, "application/json", json);
}

void Portal::handleSave() {
    if (!_net) {
        g_web.send(500, "text/plain", "not ready");
        return;
    }
    String ssid = g_web.hasArg("ssid") ? g_web.arg("ssid") : String("");
    String pass = g_web.hasArg("pass") ? g_web.arg("pass") : String("");
    ssid.trim();

    if (ssid.length() == 0) {
        g_web.send(400, "text/plain", "ssid required");
        return;
    }

    Serial.printf("[配网] 收到配置: SSID=\"%s\" %s\n", ssid.c_str(),
                  pass.length() ? "(含密码)" : "(无密码)");

    _net->save(ssid, pass);
    _net->beginConnect();

    _state    = PS_CONNECTING;
    _staStart = millis();
    _msg      = "";

    g_web.send(200, "application/json", "{\"ok\":1}");
}

void Portal::handleStatus() {
    const char* st = "idle";
    if (_state == PS_CONNECTING) st = "connecting";
    if (_state == PS_OK)         st = "ok";
    if (_state == PS_FAIL)       st = "fail";

    String ip  = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
    String msg = _msg;
    msg.replace("\"", "'");

    String json = "{\"s\":\"";
    json += st;
    json += "\",\"ip\":\"";
    json += ip;
    json += "\",\"msg\":\"";
    json += msg;
    json += "\"}";
    g_web.send(200, "application/json", json);
}

void Portal::handleNotFound() {
    // 引导所有探测请求（captive portal）回配置页
    String url = "http://";
    url += WiFi.softAPIP().toString();
    url += "/";
    g_web.sendHeader("Location", url, true);
    g_web.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------
void Portal::begin(NetConfig* net) {
    _net = net;
    if (_active) return;

    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);

    // 用芯片 efuse ID 生成稳定后缀（此时 WiFi.macAddress() 可能还没就绪）
    uint64_t chip = ESP.getEfuseMac();
    char     suf[8];
    snprintf(suf, sizeof(suf), "%04X", (uint16_t)(chip & 0xFFFF));
    _apSsid = String(AP_SSID_PREFIX) + suf;

    if (!WiFi.softAP(_apSsid.c_str())) {
        Serial.println("[配网] 热点启动失败");
        return;
    }

    IPAddress ip = WiFi.softAPIP();
    g_dns.start(DNS_PORT, "*", ip);
    setupRoutes();
    g_web.begin();

    _active = true;
    _state  = PS_IDLE;

    Serial.println();
    Serial.println("=========== 手机配网模式 ===========");
    Serial.printf("  热点名称 : %s   (无密码)\n", _apSsid.c_str());
    Serial.printf("  配置地址 : http://%s/\n", ip.toString().c_str());
    Serial.println("  手机连上热点后会自动弹出配置页；");
    Serial.println("  也可以继续用串口: wifi <SSID> <密码>");
    Serial.println("====================================");
}

void Portal::loop() {
    if (!_active) return;

    g_dns.processNextRequest();
    g_web.handleClient();

    if (_state == PS_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            _state = PS_OK;
            _okAt  = millis();
            Serial.printf("[配网] 连接成功，IP=%s，2 秒后重启\n",
                          WiFi.localIP().toString().c_str());
        } else if (millis() - _staStart > STA_TEST_TIMEOUT_MS) {
            _state = PS_FAIL;
            _msg   = "连接超时：请确认密码正确、且是 2.4G 网络";
            WiFi.disconnect(true, false);
            Serial.println("[配网] 连接超时，热点保持开启，可重新填写");
        }
    }

    if (_state == PS_OK && millis() - _okAt > 2000) {
        Serial.println("[配网] 配置完成，重启进入正常模式 ...");
        delay(100);
        ESP.restart();
    }
}

void Portal::stop() {
    if (!_active) return;
    g_web.stop();
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    _active = false;
}

String Portal::apIp() const {
    return WiFi.softAPIP().toString();
}
