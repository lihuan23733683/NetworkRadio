// ============================================================================
//  Display.cpp  --  屏幕 UI 实现（纯黑高对比主题 + 分区局部刷新）
//  防闪烁要点：
//   1. 不再整页 fillScreen 重绘——每个区域（顶栏/卡片/音量/列表/提示）独立脏标记
//   2. 码率按 8kbps 量化：HLS 分段间 189/190kbps 抖动不再触发重绘（旧版闪烁主因）
//   3. VU 条等高频变化只做局部 fillRect
//  配色：纯黑底 + 白字（黑色背景下清晰可读）；正在播行深绿底
// ============================================================================
#include "Display.h"

#include <WiFi.h>
#include <Preferences.h>     // 熄屏时长持久化（ns=radio-app，key=dim）

#include "../audio/Stations.h"
#include "../core/EventLog.h"

// ---- 配色（RGB565，柔和低饱和：暗蓝灰底 + 灰绿强调，久看不刺眼）----
//  设计原则：不用纯白/纯饱和色；文字用奶白，强调色降饱和压低亮度；
//  相邻层级明度差控制在很小区间（卡片只比底色亮一点点），避免小块屏幕上"跳"
static constexpr uint16_t C_BG      = 0x1105;   // 页面底色：暗蓝灰 (#17212B)
static constexpr uint16_t C_TOP     = 0x10C4;   // 顶栏（略深 #131B24）
static constexpr uint16_t C_CARD    = 0x1946;   // 卡片底色（#1E2A36，只亮一档）
static constexpr uint16_t C_ROW     = 0x1926;   // 列表普通行（#1A2531）
static constexpr uint16_t C_GREENBG = 0x2A47;   // 正在播放行：灰绿 (#2E4A3C，哑光不扎眼)
static constexpr uint16_t C_WHITE   = 0xE77E;   // 主文字：奶白 (#E6EDF3)，不用 #FFFFFF
static constexpr uint16_t C_GRAY    = 0x9537;   // 次要文字：灰蓝 (#93A6B8)
static constexpr uint16_t C_ACCENT  = 0x6DB3;   // 强调色：灰薄荷 (#6FB79A)
static constexpr uint16_t C_AMBER   = 0xDD28;   // 黄（警告/暂停，已压暗 #D9A441）
static constexpr uint16_t C_RED     = 0xC2EB;   // 红（静音/错误，砖红 #C75C5C）

// ---------------------------------------------------------------------------
//  自适应布局：按 _lcd 当前分辨率（受旋转影响）计算各分区位置
//  基准 320x240 横屏；小屏自动压缩行高、大屏按比例放大
// ---------------------------------------------------------------------------
void Display::computeLayout() {
    Layout L;
    L.w = _lcd.width();
    L.h = _lcd.height();

    L.hTop  = (L.h * 22) / 240;  if (L.hTop  < 18) L.hTop  = 18;
    L.hCard = (L.h * 72) / 240;  if (L.hCard < 44) L.hCard = 44;
    L.hVol  = (L.h * 26) / 240;  if (L.hVol  < 22) L.hVol  = 22;
    L.hHint = (L.h * 16) / 240;  if (L.hHint < 14) L.hHint = 14;

    L.yTop  = 0;
    L.yCard = L.yTop + L.hTop + 2;
    L.yVol  = L.yCard + L.hCard + 2;
    L.yLbl  = L.yVol + L.hVol + 4;

    L.rowH     = (L.h * 18) / 240;  if (L.rowH < 14) L.rowH = 14;
    L.rowPitch = L.rowH + 2;
    L.yList    = L.yLbl + 12;

    L.yHint = L.h - L.hHint;
    L.listRows = (L.yHint - L.yList) / L.rowPitch;
    if (L.listRows < 1) L.listRows = 1;

    _lay = L;
}

// 按像素宽度截断字符串（保持 UTF-8 不切半个字）
static String fitTextW(LGFX_Device& lcd, const String& s, int maxW) {
    if ((int)lcd.textWidth(s) <= maxW) return s;
    String out = s;
    while (out.length() > 2 && (int)lcd.textWidth(out + "..") > maxW) {
        out.remove(out.length() - 1);          // 逐字节回退，UTF-8 尾字节安全
    }
    out += "..";
    return out;
}

// 初始化面板（含重试：接触不良/上电时序问题多试几次）
static bool lcdInitWithRetry(LGFX_Device& lcd) {
    delay(120);                          // 上电/复位后留足稳定时间（ST7789 需要）
    for (int i = 0; i < 3; i++) {
        if (lcd.init()) return true;
        Serial.printf("[屏幕] 初始化失败，重试 %d/2\n", i + 1);
        delay(120);
    }
    return false;
}

bool Display::begin() {
    bool ok = lcdInitWithRetry(_lcd);
    _lcd.setRotation(_rot);                 // 横屏：320 宽 x 240 高
    setBrightness(_bright);

    // 熄屏时长：从 NVS 读用户设置（无记录用默认 10 秒）
    _lastPoke = millis();
    {
        Preferences p;
        p.begin(NVS_NS_APP, true);
        _idleOffMs = p.getULong("dim", TFT_IDLE_OFF_MS);
        p.end();
    }

    Serial.printf("[屏幕] %s %dx%d SPI(SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d) %s  熄屏=%lus\n",
                  TFT_PANEL_NAME, _lcd.width(), _lcd.height(),
                  PIN_TFT_SCLK, PIN_TFT_MOSI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST,
                  PIN_TFT_BL, ok ? "就绪" : "初始化失败(连试3次)",
                  (unsigned long)(_idleOffMs / 1000));
    if (!ok) return false;

    drawSplash();
    return true;
}

// 白屏救急：串口发 "tft" 重新走一遍初始化 + 全量重绘
bool Display::reinit() {
    bool ok = lcdInitWithRetry(_lcd);
    _lcd.setRotation(_rot);
    analogWrite(PIN_TFT_BL, _screenOn ? _bright : 0);   // 熄屏状态下保活不能把背光点亮
    _haveSnap = false;
    _lastVUW  = -1;
    _portalDrawn = false;                // 下一拍按当前模式重画（主界面或配网页）
    if (ok) drawSplash();
    return ok;
}

// ---------------------------------------------------------------------------
//  无操作自动熄屏（节电）：只关背光，面板保持初始化，点亮瞬间恢复内容
// ---------------------------------------------------------------------------
void Display::poke() {
    if (!_screenOn) {
        _screenOn = true;
        analogWrite(PIN_TFT_BL, _bright);
        Serial.println("[屏幕] 点亮");
    }
    _lastPoke = millis();
}

void Display::setIdleOff(uint32_t ms) {
    _idleOffMs = ms;
    poke();                              // 设置后先点亮，让用户看到生效
    Preferences p;
    p.begin(NVS_NS_APP, false);
    p.putULong("dim", ms);
    p.end();
    Serial.printf("[屏幕] 熄屏时长 = %lu 秒%s（已保存）\n",
                  (unsigned long)(ms / 1000), ms == 0 ? "（常亮）" : "");
}

// 信号自检：全屏依次红/绿/蓝/白/黑 各 0.8 秒
//  能看到颜色 -> SPI 信号通，问题在初始化时序；仍然全白 -> 信号线/电源没接通
void Display::colorTest() {
    static const uint16_t cols[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
    static const char* names[]   = { "红", "绿", "蓝", "白", "黑" };
    Serial.println("[屏幕] 颜色自检开始：红 绿 蓝 白 黑 各0.8秒");
    for (int i = 0; i < 5; i++) {
        _lcd.fillScreen(cols[i]);
        Serial.printf("  -> %s\n", names[i]);
        delay(800);
    }
    _haveSnap = false;                   // 结束后自动重绘 UI
    Serial.println("[屏幕] 颜色自检结束，恢复 UI");
}

// SPI 频率扫描：从低到高逐档重新初始化并刷红屏，哪档屏幕变红哪档能用
// 用途：跳线接触不良/线长导致高频失效时，快速定位可用频率，无需反复改码烧录
void Display::freqScan() {
    static const uint32_t freqs[] = { 4000000, 8000000, 16000000, 27000000, 40000000 };
    Serial.println("[屏幕] SPI 频率扫描开始：每档重新初始化并刷红屏 1.5 秒，盯屏幕看哪档变红");
    for (uint32_t f : freqs) {
        bool ok = _lcd.reinitAt(f);
        _lcd.setRotation(_rot);
        _lcd.fillScreen(0xF800);            // 红
        Serial.printf("  -> %lu Hz  init=%s  红屏1.5秒\n",
                      (unsigned long)f, ok ? "ok" : "fail");
        delay(1500);
    }
    // 扫完恢复默认频率 + 重绘 UI
    bool ok = _lcd.reinitAt(TFT_SPI_FREQ);
    _lcd.setRotation(_rot);
    setBrightness(_bright);
    _haveSnap = false;
    Serial.printf("[屏幕] 扫描结束，恢复默认 %d Hz %s\n", TFT_SPI_FREQ, ok ? "ok" : "fail");
}

// 引脚组合扫描：CS/DC/RST 在 GPIO10/11/12 上做全排列（6 种），再乘 SCLK/MOSI 正反（2 种）= 12 组
// 每组刷红屏 2 秒、黑屏 0.5 秒；哪一组让屏幕变红，就说明实际接线是这个组合
void Display::pinSweep() {
    const int pin[3] = { PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST };   // 10, 11, 12
    static const uint8_t perm[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
    };
    const int sm[2][2] = {
        { PIN_TFT_SCLK, PIN_TFT_MOSI },
        { PIN_TFT_MOSI, PIN_TFT_SCLK }
    };

    Serial.println("[屏幕] 引脚组合扫描开始：共 12 组，每组红屏 2 秒后黑屏 0.5 秒");
    int n = 0;
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < 6; i++) {
            n++;
            int cs  = pin[perm[i][0]];
            int dc  = pin[perm[i][1]];
            int rst = pin[perm[i][2]];
            bool ok = _lcd.tryPins(sm[s][0], sm[s][1], cs, dc, rst, 8000000);
            _lcd.setRotation(_rot);
            _lcd.fillScreen(0xF800);
            Serial.printf("  第 %02d 组: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d init=%s（红屏2秒）\n",
                          n, sm[s][0], sm[s][1], cs, dc, rst, ok ? "ok" : "fail");
            delay(2000);
            _lcd.fillScreen(0x0000);
            delay(500);
        }
    }
    // 恢复原始配置
    _lcd.tryPins(PIN_TFT_SCLK, PIN_TFT_MOSI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, TFT_SPI_FREQ);
    _lcd.setRotation(_rot);
    setBrightness(_bright);
    _haveSnap = false;
    Serial.println("[屏幕] 引脚扫描结束，已恢复原配置");
}

// 运行时换引脚并刷红屏：改接线后无需重新烧录即可验证（RST 传 -1 表示不用硬件复位）
bool Display::applyPins(int sclk, int mosi, int cs, int dc, int rst) {
    bool ok = _lcd.tryPins(sclk, mosi, cs, dc, rst, 8000000);
    _lcd.setRotation(_rot);
    setBrightness(_bright);
    _lcd.fillScreen(0xF800);
    _haveSnap = false;
    Serial.printf("[屏幕] 引脚改为 SCLK=%d MOSI=%d CS=%d DC=%d RST=%d -> init=%s（当前红屏，发 tft 可重绘 UI）\n",
                  sclk, mosi, cs, dc, rst, ok ? "ok" : "fail");
    return ok;
}

// 反色开关：白底黑字 <-> 黑底白字，运行时立即生效
void Display::setInvert(bool on) {
    _lcd.invertDisplay(on);
    _haveSnap = false;                   // 立即全量重绘，肉眼马上能看到结果
    Serial.printf("[屏幕] 反色 %s\n", on ? "开（黑底白字）" : "关");
}

// 红蓝顺序：颜色整体偏色（红看成蓝）时切换
bool Display::setRgbOrder(bool bgr) {
    bool ok = _lcd.reinitWith(TFT_INVERT, bgr);
    _lcd.setRotation(_rot);
    setBrightness(_bright);
    _haveSnap = false;
    Serial.printf("[屏幕] 颜色顺序 %s -> init=%s\n", bgr ? "BGR" : "RGB", ok ? "ok" : "fail");
    return ok;
}

// 旋转方向：0=竖屏 1=横屏 2=横屏倒转 3=竖屏倒转
void Display::setRotationN(uint8_t r) {
    _rot = r & 3;
    _lcd.setRotation(_rot);
    _haveSnap = false;
    Serial.printf("[屏幕] 旋转 %u（%s）\n", _rot,
                  _rot == 1 ? "横屏 320x240" : _rot == 3 ? "竖屏 240x320" :
                  _rot == 2 ? "横屏倒转" : "竖屏倒转");
}

// 音量模式状态（由旋钮按住/长按切换）：音量行加描边、底部提示改为音量提示
void Display::setVolumeMode(bool on) {
    _volMode = on;
}

void Display::setBrightness(uint8_t b) {
    _bright = b;
    // BLK 走 PWM；analogWrite 在 ESP32 核心 3.x 由 LEDC 实现
    analogWrite(PIN_TFT_BL, _bright);
}

void Display::toggleInfoPage() {
    _page = (_page == 0) ? 1 : 0;
    _infoScroll = 0;                     // 每次进入信息页从头看
    _haveSnap = false;                   // 回到主页时全量重绘一次
    _lastInfo = 0;                       // 信息页立即刷新
}

// ---------------------------------------------------------------------------
//  开机画面
// ---------------------------------------------------------------------------
void Display::drawSplash() {
    computeLayout();
    _lcd.fillScreen(C_BG);
    _lcd.setTextDatum(textdatum_t::middle_center);
    _lcd.setFont(&fonts::efontCN_24);
    _lcd.setTextColor(C_WHITE);
    _lcd.drawString(FW_NAME " 网络收音机", _lay.w / 2, _lay.h * 38 / 100);
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_GRAY);
    _lcd.drawString(String("v" FW_VERSION "  正在启动..."), _lay.w / 2, _lay.h * 54 / 100);
}

const char* Display::stateCN(Player::State s) {
    switch (s) {
        case Player::ST_IDLE:       return "待机";
        case Player::ST_CONNECTING: return "连接中";
        case Player::ST_PLAYING:    return "播放中";
        case Player::ST_PAUSED:     return "已暂停";
        case Player::ST_LOST:       return "重连中";
        case Player::ST_STOPPED:    return "已停止";
    }
    return "?";
}

// ---------------------------------------------------------------------------
//  主循环刷新：50ms 一拍，分区脏标记
// ---------------------------------------------------------------------------
void Display::loop(Player& p, uint8_t curIdx, bool portalOn) {
    uint32_t now = millis();
    if (now - _lastTick < 50) return;
    _lastTick = now;

    // ---- 保活看门狗：面板无读回线，白屏无法检测，定时静默重初始化兜底 ----
    //  （熄屏期间也照常跑，防止长熄屏后面板失联、点亮时白屏）
    if (now - _lastKeepalive >= TFT_KEEPALIVE_MS) {
        Serial.println("[屏幕] 定时保活：重新初始化面板");
        reinit();
        _lastKeepalive = now;
        return;                          // 下一拍自动全量重绘界面
    }

    // ---- 无操作自动熄屏 ----
    if (_screenOn && _idleOffMs && now - _lastPoke >= _idleOffMs) {
        _screenOn = false;
        analogWrite(PIN_TFT_BL, 0);
        Serial.println("[屏幕] 无操作熄屏（任意按键/旋钮点亮）");
    }
    if (!_screenOn) return;              // 熄屏时跳过全部绘制

    // ---- 配网模式：切专属指导页（热点名 + IP），退出后强制整页重绘回主界面 ----
    if (portalOn) {
        if (!_portalDrawn || now - _lastInfo >= 1000) {
            _portalDrawn = true;
            _lastInfo = now;
            drawPortalPage();
        }
        return;
    } else if (_portalDrawn) {
        _portalDrawn = false;
        _haveSnap = false;               // 下一拍全量重绘主界面
    }

    if (_page == 1) {
        if (now - _lastInfo >= 1000) { drawInfo(p, curIdx); _lastInfo = now; }
        return;
    }

    Snap s;
    s.cur     = curIdx;
    s.state   = (uint8_t)p.state();
    s.vol     = p.muted() ? 0 : p.volume();
    s.mute    = p.muted();
    s.portal  = portalOn;
    s.wifi    = WiFi.isConnected();
    s.rssi    = s.wifi ? (int8_t)((WiFi.RSSI() / 3) * 3) : 127;  // 量化 3dB 档，信号微抖不触发重绘
    s.br8     = (uint16_t)(p.bitRate() / 8000);
    s.err     = p.lastError();
    s.volMode = _volMode;

    if (!_haveSnap) {                    // 首帧：全量绘制
        _snap = s; _haveSnap = true;
        _lastVUW = -1;
        drawAll(p, curIdx, portalOn);
        return;
    }

    // ！！先算出"哪些区域变了"，再更新快照 ！！
    // 旧代码先写 _snap.xxx 再比较，比较结果永远为假 -> 列表从不跟随换台（bug）
    bool chTop  = (s.portal != _snap.portal) || (s.wifi != _snap.wifi) || (s.rssi != _snap.rssi);
    bool chCard = (s.cur    != _snap.cur)    || (s.state != _snap.state) || (s.br8 != _snap.br8);
    bool chList = (s.cur    != _snap.cur);
    bool chVol  = (s.vol    != _snap.vol)    || (s.mute != _snap.mute) || (s.volMode != _snap.volMode);
    bool chHint = (s.err    != _snap.err)    || (s.portal != _snap.portal) || (s.volMode != _snap.volMode);

    _snap = s;

    if (chTop)  drawTop(portalOn, s.wifi);
    if (chCard) drawCard(p, curIdx);
    if (chList) drawList(curIdx);
    if (chVol)  drawVolume(p);
    if (chHint) drawHint(p, portalOn);

    drawMiniVU(p);                       // 小 VU 条局部刷新（内部自带去重）
    tickMarquee(now);                    // 标签行走马灯（控制台地址滚动）
}

// ---------------------------------------------------------------------------
//  走马灯：标签行右侧滚动显示 "控制台 http://IP · http://radio.local"
//  宽度够就静态居中显示，不够就持续左移循环
// ---------------------------------------------------------------------------
void Display::tickMarquee(uint32_t now) {
    if (_mqW <= 0 || !WiFi.isConnected()) return;
    if (_mqText.isEmpty()) return;

    _lcd.setFont(&fonts::efontCN_14);
    int tw = (int)_lcd.textWidth(_mqText);

    if (tw <= _mqW) {                    // 放得下：静态居中（只画一次）
        if (_mqOff != -1) {
            _lcd.fillRect(_mqX0, _lay.yLbl, _mqW, 16, C_BG);
            _lcd.setTextDatum(textdatum_t::middle_left);
            _lcd.setTextColor(C_GRAY);
            _lcd.drawString(_mqText, _mqX0 + (_mqW - tw) / 2, _lay.yLbl + 4);
            _lcd.setTextDatum(textdatum_t::middle_left);
            _mqOff = -1;
        }
        return;
    }

    // 放不下："停-走"式走马灯——文字贴左完整停 3 秒供阅读，再向左滚出，
    // 滚出后立即回到起点再停。"控制台"三个字在静止期始终完整可见。
    const uint32_t HOLD_MS = 3000;
    if (_mqOff == 0) {                   // 静止期
        if (!_mqRestDrawn) {
            _lcd.setClipRect(_mqX0, _lay.yLbl, _mqW, 16);
            _lcd.fillRect(_mqX0, _lay.yLbl, _mqW, 16, C_BG);
            _lcd.setTextDatum(textdatum_t::middle_left);
            _lcd.setTextColor(C_GRAY);
            _lcd.drawString(_mqText, _mqX0, _lay.yLbl + 4);
            _lcd.clearClipRect();
            _mqRestDrawn = true;
        }
        if (now - _mqLast < HOLD_MS) return;
        _mqOff = 1;                      // 静止期结束，进入滚动
    }

    // 滚动期：每 50ms 左移 3px
    if (now - _mqLast < 50) return;
    _mqLast = now;
    _mqOff += 3;

    _lcd.setClipRect(_mqX0, _lay.yLbl, _mqW, 16);
    _lcd.fillRect(_mqX0, _lay.yLbl, _mqW, 16, C_BG);
    _lcd.setTextDatum(textdatum_t::middle_left);
    _lcd.setTextColor(C_GRAY);
    _lcd.drawString(_mqText, _mqX0 - _mqOff, _lay.yLbl + 4);
    _lcd.clearClipRect();
    _lcd.setTextDatum(textdatum_t::middle_left);

    if (_mqOff >= tw + 12) {             // 文字完全移出：回到起点进入下一个静止期
        _mqOff = 0;
        _mqRestDrawn = false;
        _mqLast = now;
    }
}

void Display::setPortalInfo(const String& ssid, const String& ip) {
    _apSsid = ssid;
    _apIp   = ip;
}

// ---------------------------------------------------------------------------
//  配网模式专属页：告诉用户连哪个热点、浏览器打开哪个地址
// ---------------------------------------------------------------------------
void Display::drawPortalPage() {
    computeLayout();
    const int w = _lay.w, h = _lay.h;
    _lcd.fillScreen(C_BG);

    _lcd.setTextDatum(textdatum_t::middle_center);
    const lgfx::v1::IFont* fBig = &fonts::efontCN_24;

    // 标题
    _lcd.setFont(fBig);
    _lcd.setTextColor(C_AMBER);
    _lcd.drawString("配 网 模 式", w / 2, h * 11 / 100);

    // 步骤 1：热点名
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_GRAY);
    _lcd.drawString("第一步：手机连接 WiFi 热点（无密码）", w / 2, h * 27 / 100);

    _lcd.setFont(fBig);
    _lcd.setTextColor(C_ACCENT);
    String ssid = fitTextW(_lcd, _apSsid.length() ? _apSsid : "Radio-XXXX", w - 40);
    _lcd.drawString(ssid, w / 2, h * 40 / 100);

    // 步骤 2：IP
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_GRAY);
    _lcd.drawString("第二步：浏览器打开", w / 2, h * 57 / 100);

    _lcd.setFont(fBig);
    _lcd.setTextColor(C_ACCENT);
    _lcd.drawString(_apIp.length() ? _apIp : "192.168.4.1", w / 2, h * 70 / 100);

    // 底部提示
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_GRAY);
    _lcd.drawString("在网页里选择家里的 WiFi 并输入密码", w / 2, h * 86 / 100);
    _lcd.setTextColor(C_AMBER);
    _lcd.drawString("长按 BOOT 键可退出配网", w / 2, h * 94 / 100);
}

void Display::drawAll(Player& p, uint8_t curIdx, bool portalOn) {
    computeLayout();                     // 整页重绘前按当前分辨率重算布局
    const Layout& L = _lay;
    _lcd.fillScreen(C_BG);
    _cardPainted = false;                // 整页重绘：卡片外形需要重新铺一次
    drawTop(portalOn, WiFi.isConnected());
    drawCard(p, curIdx);
    drawVolume(p);
    _lcd.setTextDatum(textdatum_t::middle_left);
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_ACCENT);
    _lcd.drawString("电台列表", 8, L.yLbl + 4);
    if (WiFi.isConnected()) {            // 常驻提醒：这里有网页控制台（内容放不下就走马灯滚动）
        _lcd.setFont(&fonts::efontCN_14);
        _mqX0 = 8 + (int)_lcd.textWidth("电台列表") + 14;
        _mqW  = L.w - 8 - _mqX0;
        _mqText = String("控制台 http://") + WiFi.localIP().toString() + " · http://radio.local";
        _mqOff  = 0;
        _mqLast = millis();
        _mqRestDrawn = false;
        tickMarquee(millis());           // 立即画出第一帧
    } else {
        _mqW = 0; _mqText = "";
    }
    drawList(curIdx);
    drawHint(p, portalOn);
}

// ---------------------------------------------------------------------------
//  顶栏
// ---------------------------------------------------------------------------
void Display::drawTop(bool portalOn, bool wifiOk) {
    const Layout& L = _lay;
    _lcd.fillRect(0, L.yTop, L.w, L.hTop, C_TOP);
    _lcd.setFont(&fonts::efontCN_14);

    _lcd.setTextDatum(textdatum_t::middle_left);
    _lcd.setTextColor(C_WHITE);
    _lcd.drawString("网络收音机", 10, L.yTop + L.hTop / 2);

    // 右侧状态必须用右对齐基准，否则从右边缘向左画会超出屏幕被裁掉（只剩一个"已"）
    _lcd.setTextDatum(textdatum_t::middle_right);
    if (portalOn)      { _lcd.setTextColor(C_AMBER);  _lcd.drawString("配网模式", L.w - 10, L.yTop + L.hTop / 2); }
    else if (wifiOk)   {   // 顶栏右侧显示 WiFi 信号强度（dBm 实时值），按强弱变色：>=-60 绿 / -75~-60 琥珀 / <-75 红
        int r = WiFi.RSSI();
        uint16_t rc = (r >= -60) ? C_ACCENT : (r >= -75) ? C_AMBER : C_RED;
        char b[12]; snprintf(b, sizeof(b), "%d dBm", r);
        _lcd.setTextColor(rc); _lcd.drawString(b, L.w - 10, L.yTop + L.hTop / 2);
    }
    else               { _lcd.setTextColor(C_RED);    _lcd.drawString("未连接",   L.w - 10, L.yTop + L.hTop / 2); }
    _lcd.setTextDatum(textdatum_t::middle_left);
}

// ---------------------------------------------------------------------------
//  正在播放卡片
// ---------------------------------------------------------------------------
void Display::drawCard(Player& p, uint8_t curIdx) {
    const Station& s = Stations::get(curIdx);
    const Layout& L = _lay;
    const int yc = L.yCard, hc = L.hCard, w = L.w;
    uint8_t num = (uint8_t)(curIdx + 1);
    Player::State st = p.state();

    // 卡片的圆角外形只在整页重绘时铺一次；之后只改内容区，避免整块重铺产生闪动
    if (!_cardPainted) {
        _lcd.fillRoundRect(4, yc, w - 8, hc, 6, C_CARD);
        _cardPainted = true;
        _cNum = 0xFF; _cState = 0xFF; _cName = ""; _cNote = "";
    }

    _lcd.setFont(&fonts::efontCN_14);
    char buf[64];

    // ---- 第一行：正在播放 · NN + 状态（只在台号/状态变化时重画这一行）----
    if (num != _cNum || (uint8_t)st != _cState) {
        _lcd.fillRect(8, yc + 2, w - 16, 18, C_CARD);
        _lcd.setTextDatum(textdatum_t::middle_left);
        snprintf(buf, sizeof(buf), "正在播放 · %02u", num);
        _lcd.setTextColor(C_GRAY);
        _lcd.drawString(buf, 16, yc + 10);

        uint16_t sc = (st == Player::ST_PLAYING) ? C_ACCENT
                    : (st == Player::ST_PAUSED || st == Player::ST_LOST) ? C_AMBER : C_GRAY;
        _lcd.setTextDatum(textdatum_t::middle_right);
        _lcd.setTextColor(sc);
        _lcd.drawString(stateCN(st), w - 16, yc + 10);
        _lcd.setTextDatum(textdatum_t::middle_left);
        _cNum = num; _cState = (uint8_t)st;
    }

    // ---- 台名：只有台名真的变了才擦这一块重画（换台时不会闪）----
    if (String(s.name) != _cName) {
        int nameH = (hc * 26) / 72;
        _lcd.fillRect(8, yc + (hc * 22) / 72, w - 20, nameH, C_CARD);
        _lcd.setTextDatum(textdatum_t::middle_left);
        _lcd.setTextColor(C_WHITE);
        bool bigOk = (hc >= 60);                       // 卡片够高才用大字号
        const lgfx::v1::IFont* f = bigOk ? &fonts::efontCN_24 : &fonts::efontCN_14;
        _lcd.setFont(f);
        String name = fitTextW(_lcd, s.name, w - 40);
        if ((int)_lcd.textWidth(name) > w - 40) {
            f = &fonts::efontCN_14; _lcd.setFont(f);
            name = fitTextW(_lcd, s.name, w - 40);
        }
        _lcd.drawString(name, 16, yc + (hc * 34) / 72);
        _cName = s.name;
    }

    // ---- 备注行（编码/码率）----
    String note = s.note;
    if (p.bitRate()) note += String(" · ") + ((p.bitRate() / 8000) * 8) + "kbps";
    if (note != _cNote) {
        _lcd.fillRect(8, yc + (hc * 48) / 72, (w * 13) / 20, 20, C_CARD);
        _lcd.setFont(&fonts::efontCN_14);
        _lcd.setTextColor(C_GRAY);
        _lcd.drawString(fitTextW(_lcd, note, (w * 13) / 20 - 8), 16, yc + (hc * 58) / 72);
        _cNote = note;
    }

    // 小 VU 条（内容由 drawMiniVU 刷）
    _lastVUW = -1;
}

// ---------------------------------------------------------------------------
//  小型 VU 电平条（卡片右下角，局部刷新）
// ---------------------------------------------------------------------------
void Display::drawMiniVU(Player& p) {
    const Layout& L = _lay;
    const int vuW = L.w / 4, vuH = 6;
    const int vuX = L.w - vuW - 16, vuY = L.yCard + (L.hCard * 56) / 72;
    if (p.state() != Player::ST_PLAYING) {
        if (_lastVUW != 0) { _lcd.fillRect(vuX, vuY, vuW, vuH, C_CARD); _lastVUW = 0; }
        return;
    }
    uint32_t vu = p.vuLevel();                     // 0..65535
    int w = (int)((uint64_t)vu * vuW / 65535);
    if (w == _lastVUW) return;
    _lcd.fillRect(vuX, vuY, vuW, vuH, C_CARD);
    if (w > 0) _lcd.fillRect(vuX, vuY, w, vuH, C_ACCENT);
    _lastVUW = w;
}

// ---------------------------------------------------------------------------
//  音量行
// ---------------------------------------------------------------------------
void Display::drawVolume(Player& p) {
    const Layout& L = _lay;
    _lcd.fillRect(0, L.yVol, L.w, L.hVol, C_BG);
    if (_volMode) _lcd.drawRect(0, L.yVol, L.w, L.hVol, C_ACCENT);   // 音量模式：整行描边提示
    uint8_t vol = p.muted() ? 0 : p.volume();

    _lcd.setTextDatum(textdatum_t::middle_center);
    _lcd.setFont(&fonts::efontCN_14);
    _lcd.setTextColor(C_WHITE);
    _lcd.drawString("音量", L.w * 22 / 320, L.yVol + L.hVol / 2);

    int bh = L.hVol - 10;  if (bh < 12) bh = 12;
    int bx = L.w * 40 / 320, bw = L.w * 216 / 320, by = L.yVol + 5;
    _lcd.fillRect(bx, by, bw, bh, C_ROW);                    // 轨道（暗蓝灰）
    _lcd.drawRoundRect(bx, by, bw, bh, 4, C_TOP);            // 轻描边
    int w = (int)((uint32_t)vol * (bw - 4) / MAX_VOLUME);
    if (w > 0) _lcd.fillRoundRect(bx + 2, by + 2, w, bh - 4, 3, C_ACCENT);

    // 右侧：百分比 / 静音
    _lcd.setTextDatum(textdatum_t::middle_right);
    if (p.muted()) {
        _lcd.setTextColor(C_RED);
        _lcd.drawString("静音", L.w - 8, L.yVol + L.hVol / 2);
    } else {
        char pc[8];
        snprintf(pc, sizeof(pc), "%u%%", (unsigned)(vol * 100 / MAX_VOLUME));
        _lcd.setTextColor(C_WHITE);
        _lcd.drawString(pc, L.w - 8, L.yVol + L.hVol / 2);
    }
    _lcd.setTextDatum(textdatum_t::middle_left);
}

// ---------------------------------------------------------------------------
//  电台列表（4 行可见，窗口跟随正在播的台）
// ---------------------------------------------------------------------------
void Display::drawList(uint8_t curIdx) {
    const Layout& L = _lay;
    uint8_t n = Stations::count();
    if (n == 0) {
        _lcd.fillRect(0, L.yList, L.w, L.listRows * L.rowPitch, C_BG);
        _lcd.setTextDatum(textdatum_t::middle_center);
        _lcd.setFont(&fonts::efontCN_14);
        _lcd.setTextColor(C_GRAY);
        _lcd.drawString("暂无电台（网页控制台可添加）", L.w / 2, L.yList + L.hHint);
        return;
    }

    // 窗口起点：让正在播的行保持在可见范围内
    uint8_t start = 0;
    if (n > L.listRows) {
        start = (curIdx >= 1) ? curIdx - 1 : 0;
        if (start > n - L.listRows) start = n - L.listRows;
    }

    _lcd.setTextDatum(textdatum_t::middle_left);
    _lcd.setFont(&fonts::efontCN_14);
    char num[8];

    // 逐行擦写（不再整块铺页面底色）——整块铺底就是换台时列表闪动的来源
    for (int i = 0; i < L.listRows; i++) {
        int y = L.yList + i * L.rowPitch;
        if (start + i >= n) {                        // 空行
            _lcd.fillRect(0, y, L.w, L.rowPitch, C_BG);
            continue;
        }
        uint8_t idx = start + (uint8_t)i;
        bool playing = (idx == curIdx);

        _lcd.fillRect(4, y, L.w - 8, L.rowH, playing ? C_GREENBG : C_ROW);
        if (playing) _lcd.drawRect(4, y, L.w - 8, L.rowH, C_ACCENT);
        _lcd.fillRect(0, y + L.rowH, L.w, L.rowPitch - L.rowH, C_BG);   // 行间缝隙

        snprintf(num, sizeof(num), "%02u", idx + 1);
        _lcd.setTextColor(playing ? C_WHITE : C_GRAY);
        _lcd.drawString(num, 14, y + L.rowH / 2);

        _lcd.setTextColor(C_WHITE);
        _lcd.drawString(fitTextW(_lcd, Stations::get(idx).name, L.w - 74), 44, y + L.rowH / 2);
    }
}

// ---------------------------------------------------------------------------
//  底部提示行
// ---------------------------------------------------------------------------
void Display::drawHint(Player& p, bool portalOn) {
    const Layout& L = _lay;
    _lcd.fillRect(0, L.yHint, L.w, L.hHint, C_TOP);
    _lcd.setTextDatum(textdatum_t::middle_center);
    _lcd.setFont(&fonts::efontCN_14);

    String e = p.lastError();
    if (portalOn) {
        _lcd.setTextColor(C_AMBER);
        _lcd.drawString("配网模式中 · 长按 BOOT 退出", L.w / 2, L.yHint + L.hHint / 2);
    } else if (_volMode) {
        _lcd.setTextColor(C_ACCENT);
        _lcd.drawString("音量模式 · 旋转调节 · 5 秒后自动退出", L.w / 2, L.yHint + L.hHint / 2);
    } else if (e.length()) {
        _lcd.setTextColor(C_RED);
        _lcd.drawString(fitTextW(_lcd, e, L.w - 20), L.w / 2, L.yHint + L.hHint / 2);
    } else {
        _lcd.setTextColor(C_GRAY);
        _lcd.drawString("旋转换台 · 按住旋转调音量 · 短按暂停", L.w / 2, L.yHint + L.hHint / 2);
    }
    _lcd.setTextDatum(textdatum_t::middle_left);
}

// ---------------------------------------------------------------------------
//  信息页（诊断）：K0 长按进入/退出，1 秒刷新
//  行距紧凑排版；内容超过一屏时用旋钮滚动查看（scrollInfo）
// ---------------------------------------------------------------------------
void Display::scrollInfo(int dir) {
    _infoScroll += dir;
}

void Display::drawInfo(Player& p, uint8_t curIdx) {
    computeLayout();
    const int w = _lay.w, h = _lay.h;
    _lcd.fillScreen(C_BG);
    _lcd.setTextDatum(textdatum_t::middle_left);
    _lcd.setFont(&fonts::efontCN_14);

    // ---- 先把所有行备齐（键/值/颜色）----
    String keys[14], vals[14];
    uint16_t cols[14];
    int n = 0;
    auto add = [&](const char* k, const String& v, uint16_t col = C_WHITE) {
        if (n < 14) { keys[n] = k; vals[n] = v; cols[n] = col; n++; }
    };

    uint32_t up = millis() / 1000;
    char buf[64];

    snprintf(buf, sizeof(buf), "%s v%s", FW_NAME, FW_VERSION);
    add("固件", buf);

    add("复位", EvLog::resetReasonText());

    snprintf(buf, sizeof(buf), "%s  %d dBm", WiFi.SSID().c_str(), (int)WiFi.RSSI());
    add("WiFi", WiFi.isConnected() ? buf : "未连接", WiFi.isConnected() ? C_WHITE : C_RED);

    add("IP", WiFi.localIP().toString());

    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    add("开机", buf);

    snprintf(buf, sizeof(buf), "%lu B (低 %lu B)",
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    add("内存", buf);

    snprintf(buf, sizeof(buf), "停滞 %lu / 重连 %u / 败 %u",
             (unsigned long)p.stallCount(), p.retries(), p.failStreak());
    add("自愈", buf);

    snprintf(buf, sizeof(buf), "#%u %s", curIdx + 1, Stations::get(curIdx).name);
    add("电台", buf);

    snprintf(buf, sizeof(buf), "%lu kbps  %lukHz", (unsigned long)(p.bitRate() / 1000),
             (unsigned long)(p.sampleRate() / 1000));
    add("码率", buf);

    add("控制台", String("http://") + WiFi.localIP().toString(), C_ACCENT);
    add("同义", "http://radio.local", C_ACCENT);

    // ---- 紧凑排版 + 滚动窗口 ----
    int dy = (h * 18) / 240;  if (dy < 16) dy = 16;
    int y0 = 20;                              // 标题行之下
    int visible = (h - 6 - y0) / dy;  if (visible < 1) visible = 1;
    int maxScroll = n - visible;  if (maxScroll < 0) maxScroll = 0;
    if (_infoScroll > maxScroll) _infoScroll = maxScroll;
    if (_infoScroll < 0) _infoScroll = 0;

    // 标题行：左标题，右侧滚动提示/进度
    _lcd.setTextColor(C_ACCENT);
    _lcd.drawString("设备信息", 10, 4);
    _lcd.setTextDatum(textdatum_t::middle_right);
    _lcd.setTextColor(C_GRAY);
    if (maxScroll > 0) {
        snprintf(buf, sizeof(buf), "%d-%d/%d 旋钮滚动", _infoScroll + 1,
                 _infoScroll + visible, n);
        _lcd.drawString(buf, w - 8, 4);
    } else {
        _lcd.drawString("K0 长按返回", w - 8, 4);
    }
    _lcd.setTextDatum(textdatum_t::middle_left);

    int x2 = w * 3 / 8;
    for (int i = 0; i < visible; i++) {
        int idx = _infoScroll + i;
        if (idx >= n) break;
        int y = y0 + i * dy;
        _lcd.setTextColor(C_GRAY);
        _lcd.drawString(keys[idx], 10, y);
        _lcd.setTextColor(cols[idx]);
        _lcd.drawString(fitTextW(_lcd, vals[idx], w - x2 - 10), x2, y);
    }
}
