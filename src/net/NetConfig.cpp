// ============================================================================
//  NetConfig.cpp  --  WiFi 多网络记忆与连接管理
// ============================================================================
#include "NetConfig.h"

#include <Preferences.h>

#include "../core/AppConfig.h"

static const char* statusText(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "空闲";
        case WL_NO_SSID_AVAIL:   return "找不到该热点(SSID错/信号太弱/5G频段)";
        case WL_SCAN_COMPLETED:  return "扫描完成";
        case WL_CONNECTED:       return "已连接";
        case WL_CONNECT_FAILED:  return "连接失败(密码错误)";
        case WL_CONNECTION_LOST: return "连接丢失";
        case WL_DISCONNECTED:    return "未连接";
        default:                 return "未知状态";
    }
}

// ---------------------------------------------------------------------------
//  NVS 键名（ns = radio-wifi）
//    cnt      : 已存网络数
//    w{N}s / w{N}p : 第 N 个网络的 SSID / 密码
//    lg       : 首选网络（最近成功连接）的下标
//    ssid/pass: 旧版单网络键（迁移后删除）
// ---------------------------------------------------------------------------
static String keyS(uint8_t i) { return String("w") + i + "s"; }
static String keyP(uint8_t i) { return String("w") + i + "p"; }

void NetConfig::persist() {
    Preferences p;
    if (!p.begin(NVS_NS_WIFI, false)) {
        Serial.println("[NVS] 打开命名空间失败，配置无法保存");
        return;
    }
    p.putUChar("cnt", _cnt);
    p.putUChar("lg", _lastGood);
    for (uint8_t i = 0; i < _cnt; i++) {
        p.putString(keyS(i).c_str(), _creds[i].ssid);
        p.putString(keyP(i).c_str(), _creds[i].pass);
    }
    p.end();
}

bool NetConfig::begin() {
    Preferences p;
    bool ok = p.begin(NVS_NS_WIFI, false);   // 读写模式，便于旧数据迁移

    uint8_t cnt = p.getUChar("cnt", 0);
    if (cnt > WIFI_MAX_SAVED) cnt = WIFI_MAX_SAVED;
    for (uint8_t i = 0; i < cnt; i++) {
        _creds[i].ssid = p.getString(keyS(i).c_str(), "");
        _creds[i].pass = p.getString(keyP(i).c_str(), "");
    }
    _cnt      = cnt;
    _lastGood = p.getUChar("lg", 0);
    if (_cnt == 0 || _lastGood >= _cnt) _lastGood = 0;

    // 旧版（单网络）数据迁移：读到新结构后删除旧键
    if (_cnt == 0) {
        String os = p.getString(NVS_KEY_SSID, "");
        if (os.length() > 0) {
            _creds[0].ssid = os;
            _creds[0].pass = p.getString(NVS_KEY_PASS, "");
            _cnt = 1;
            _lastGood = 0;
            persist();
            p.remove(NVS_KEY_SSID);
            p.remove(NVS_KEY_PASS);
            Serial.println("[WiFi] 已从旧版单网络配置自动迁移");
        }
    }
    p.end();

    Serial.printf("[NVS] 读取配置 ns=\"%s\" open=%d 已存网络 %u/%u 首选=\"%s\"\n",
                  NVS_NS_WIFI, ok, _cnt, WIFI_MAX_SAVED,
                  _cnt ? _creds[_lastGood].ssid.c_str() : "(无)");

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.setSleep(false);          // 关省电，音频流更稳（代价是功耗略高）
    WiFi.setAutoReconnect(true);

    return hasCredentials();
}

bool NetConfig::save(const String& ssid, const String& pass) {
    if (ssid.length() == 0) return false;

    // 同名网络：更新密码并设为首选
    for (uint8_t i = 0; i < _cnt; i++) {
        if (_creds[i].ssid == ssid) {
            _creds[i].pass = pass;
            _lastGood      = i;
            persist();
            Serial.printf("[WiFi] 更新已存网络 #%u \"%s\"，设为首选\n", i + 1, ssid.c_str());
            return true;
        }
    }

    if (_cnt < WIFI_MAX_SAVED) {
        _creds[_cnt].ssid = ssid;
        _creds[_cnt].pass = pass;
        _lastGood         = _cnt;
        _cnt++;
        persist();
        Serial.printf("[WiFi] 新增网络 \"%s\"（已存 %u/%u），设为首选\n",
                      ssid.c_str(), _cnt, WIFI_MAX_SAVED);
        return true;
    }

    // 列表满：挤掉最旧的（槽 0），整体前移，新网络放末尾并设为首选
    for (uint8_t i = 1; i < WIFI_MAX_SAVED; i++) _creds[i - 1] = _creds[i];
    _creds[WIFI_MAX_SAVED - 1].ssid = ssid;
    _creds[WIFI_MAX_SAVED - 1].pass = pass;
    _lastGood = WIFI_MAX_SAVED - 1;
    persist();
    Serial.printf("[WiFi] 列表已满(%u)，挤掉最旧的 \"%s\"，新网络 \"%s\" 设为首选\n",
                  WIFI_MAX_SAVED, _creds[0].ssid.c_str(), ssid.c_str());
    return true;
}

bool NetConfig::del(const String& ssid) {
    for (uint8_t i = 0; i < _cnt; i++) {
        if (_creds[i].ssid != ssid) continue;
        for (uint8_t k = i + 1; k < _cnt; k++) _creds[k - 1] = _creds[k];
        _cnt--;
        if (_lastGood >= _cnt) _lastGood = 0;
        persist();
        Serial.printf("[WiFi] 已移除 \"%s\"（剩 %u 个）\n", ssid.c_str(), _cnt);
        return true;
    }
    Serial.printf("[WiFi] 列表里没有 \"%s\"\n", ssid.c_str());
    return false;
}

void NetConfig::clear() {
    Preferences p;
    p.begin(NVS_NS_WIFI, false);
    p.clear();
    p.end();
    _cnt      = 0;
    _lastGood = 0;
    Serial.println("[WiFi] 已清除全部已存网络");
}

// 尝试连接单个网络；确定性失败（SSID 不存在/密码错）立即返回
bool NetConfig::tryOne(uint8_t idx, uint32_t timeoutMs) {
    Serial.printf("[WiFi] 正在连接 \"%s\" ", _creds[idx].ssid.c_str());
    Serial.flush();

    WiFi.begin(_creds[idx].ssid.c_str(), _creds[idx].pass.c_str());

    uint32_t t0 = millis();
    uint32_t lastDot = 0;
    while (millis() - t0 < timeoutMs) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            _everConnected = true;
            _lastGood      = idx;
            _failRun       = 0;
            persist();   // 记住首选
            Serial.printf("\n[WiFi] 连接成功   IP: %s   RSSI: %d dBm   耗时 %lu ms\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          (unsigned long)(millis() - t0));
            return true;
        }
        // 密码错 / SSID 不存在时不必等满超时
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            Serial.printf("\n[WiFi] \"%s\" 连不上：%s\n", _creds[idx].ssid.c_str(),
                          statusText(st));
            WiFi.disconnect(true, false);
            delay(100);
            return false;
        }
        if (millis() - lastDot > 500) {
            lastDot = millis();
            Serial.print('.');
            Serial.flush();
        }
        delay(50);
    }

    Serial.printf("\n[WiFi] \"%s\" 连接超时\n", _creds[idx].ssid.c_str());
    WiFi.disconnect(true, false);
    delay(100);
    return false;
}

bool NetConfig::connect(uint32_t timeoutMs) {
    if (!hasCredentials()) {
        Serial.println("[WiFi] 没有保存过网络配置，请先执行: wifi <SSID> <密码>");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);
    delay(100);

    // 多个网络时压缩单次超时，避免逐个傻等
    uint32_t perTry = (_cnt > 1 && timeoutMs > 10000) ? 10000 : timeoutMs;

    Serial.printf("[WiFi] 已存 %u 个网络，优先 \"%s\"%s\n", _cnt,
                  _creds[_lastGood].ssid.c_str(), _cnt > 1 ? "，失败自动换下一个" : "");

    for (uint8_t k = 0; k < _cnt; k++) {
        uint8_t idx = (_lastGood + k) % _cnt;   // 首选 -> 依次轮转
        if (k > 0) Serial.printf("[WiFi] 换下一个已存网络 ...\n");
        if (tryOne(idx, perTry)) return true;
    }

    Serial.println("[WiFi] 所有已存网络均未连上");
    return false;
}

void NetConfig::beginConnect() {
    if (!hasCredentials()) return;
    WiFi.mode(WIFI_AP_STA);  // 保持配网热点可用
    WiFi.setSleep(false);
    WiFi.begin(_creds[_lastGood].ssid.c_str(), _creds[_lastGood].pass.c_str());
    _lastTry = millis();
    Serial.printf("[WiFi] 正在连接 \"%s\" ...\n", _creds[_lastGood].ssid.c_str());
}

void NetConfig::loop() {
    if (!hasCredentials()) return;
    if (isConnected()) {
        _everConnected = true;
        return;
    }
    if (millis() - _lastTry < WIFI_RETRY_INTERVAL_MS) return;

    _lastTry = millis();

    // 先盯着首选网络试 WIFI_ROTATE_AFTER 次，仍不行就轮换到下一个已存网络
    uint8_t idx;
    if (_failRun < WIFI_ROTATE_AFTER) {
        idx = _lastGood;
    } else {
        uint32_t step = (_failRun - WIFI_ROTATE_AFTER) / WIFI_ROTATE_AFTER;
        idx = (_lastGood + 1 + step) % _cnt;
    }
    _failRun++;

    Serial.printf("[WiFi] 掉线，尝试重连 \"%s\"（第 %u 次，已存 %u 个网络）...\n",
                  _creds[idx].ssid.c_str(), _failRun, _cnt);
    WiFi.begin(_creds[idx].ssid.c_str(), _creds[idx].pass.c_str());
}

void NetConfig::printStatus() const {
    Serial.println("---- WiFi ----");
    Serial.printf("  SSID     : %s\n", isConnected() ? String(WiFi.SSID()).c_str()
                                                     : (hasCredentials() ? _creds[_lastGood].ssid.c_str() : "(未配置)"));
    Serial.printf("  状态     : %s\n", statusText(WiFi.status()));
    if (isConnected()) {
        Serial.printf("  IP       : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  网关     : %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("  RSSI     : %d dBm\n", WiFi.RSSI());
        Serial.printf("  MAC      : %s\n", WiFi.macAddress().c_str());
    }
    Serial.printf("  已存网络 : %u/%u（list 查看，首选自动切换）\n", _cnt, WIFI_MAX_SAVED);
}

void NetConfig::printList() const {
    if (_cnt == 0) {
        Serial.println("[WiFi] 还没有保存任何网络（wifi <SSID> <密码> 添加）");
        return;
    }
    Serial.printf("---- 已存 WiFi（%u/%u）----\n", _cnt, WIFI_MAX_SAVED);
    for (uint8_t i = 0; i < _cnt; i++) {
        Serial.printf(" %u. %s%s\n", i + 1, _creds[i].ssid.c_str(),
                      i == _lastGood ? "   <- 首选（最近成功）" : "");
    }
    Serial.println("说明：连接时先试首选，失败自动换下一个；wifi <SSID> <密码> 添加/更新，wifi del <SSID> 移除");
}

String NetConfig::ssid() const {
    if (isConnected()) return String(WiFi.SSID());
    return hasCredentials() ? _creds[_lastGood].ssid : String("");
}

int NetConfig::scan() {
    WiFi.mode(WIFI_STA);
    Serial.println("[WiFi] 扫描周边热点，约需 3~6 秒 ...");
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.println("[WiFi] 没扫到任何热点（检查天线 / 5G 频段 / 距离）");
        return 0;
    }
    Serial.printf("\n共 %d 个热点：\n", n);
    Serial.println(" 序号  RSSI  加密    SSID");
    for (int i = 0; i < n; i++) {
        Serial.printf(" %2d   %4d  %-5s  %s\n", i + 1, WiFi.RSSI(i),
                      (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "lock",
                      WiFi.SSID(i).c_str());
    }
    Serial.println("提示：ESP32-S3 只支持 2.4GHz，5G 热点不会出现在列表里");
    WiFi.scanDelete();
    return n;
}
