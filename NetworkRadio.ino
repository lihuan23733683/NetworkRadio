// ============================================================================
//  NetworkRadio.ino  --  ESP32-S3 网络收音机（无屏 / 无旋钮，串口控制台调试版）
//
//  运行流程：
//    [1/3] 配置网络   -> 串口输入 SSID/密码，存入 NVS，之后开机自动连
//    [2/3] 获取节目地址 -> 从内置节目表按上次收听的序号取播放地址
//    [3/3] 播放       -> 通过 I2S 推给 MAX98357A，掉流自动重连
//
//  依赖库：ESP32-audioI2S (schreibfaul1) v3.4.7
//          已放在 D:\Arduino\libraries\ESP32-audioI2S
// ============================================================================
//  Audio.h 内部会 include <SD.h> -> "SPI.h"，这里显式先包含，
//  否则 arduino-cli 的库依赖检测阶段会找不到 SPI.h 而静默失败。
//  WebServer / DNSServer 同理：必须在这里先 include，
//  否则 libsdetect 处理 Portal.cpp 时找不到头文件会把编译器卡死。
#include <DNSServer.h>
#include <SPI.h>
#include <WebServer.h>
#include <ESPmDNS.h>      // radio.local 域名（手机浏览器直接访问，免背 IP）
#include <LovyanGFX.h>    // 屏幕驱动（必须在此先 include，原因同上）

#include "src/core/AppConfig.h"
#include "src/core/EventLog.h"
#include "src/hmi/Button.h"
#include "src/hmi/Console.h"
#include "src/hmi/Control.h"
#include "src/hmi/Display.h"
#include "src/hmi/Knob.h"
#include "src/net/NetConfig.h"
#include "src/audio/Player.h"
#include "src/net/Portal.h"
#include "src/audio/Stations.h"
#include "src/audio/Tone.h"

Console   con;
NetConfig net;
Player    player;
Portal    portal;
Button    btn;
Control   control;
Display   disp;
Knob      knob;

uint8_t   g_station = 0;      // 当前节目索引（0 基）
uint32_t  g_bootMs  = 0;
bool      g_mdnsOn  = false;  // radio.local 已注册（WiFi 断开时撤销，重连后重注册）

// 注册 mDNS：局域网内用 http://radio.local 访问网页控制台，不用背 IP
void ensureMdns() {
    if (WiFi.isConnected()) {
        if (g_mdnsOn) return;
        if (MDNS.begin("radio")) {
            MDNS.addService("http", "tcp", 80);
            g_mdnsOn = true;
            Serial.printf("[mDNS] 控制台地址: http://radio.local （或 http://%s/）\n",
                          WiFi.localIP().toString().c_str());
        }
    } else if (g_mdnsOn) {
        MDNS.end();
        g_mdnsOn = false;
    }
}

// ---------------------------------------------------------------------------
//  旋钮交互状态机（v0.6.6 重新设计）
//
//    旋转（默认）        = 选台：列表高亮跟着走，停手 250ms 后自动切台播放
//                         （不每格都发 HTTP，快速转也不会把播放器打爆）
//    按住旋钮 + 旋转      = 调音量（不必先进入任何模式，按下去就能调）
//    长按旋钮 1 秒        = 锁定/退出"音量模式"（不想一直按着时用）
//    短按旋钮            = 暂停 / 继续（音量模式下 = 退出音量模式）
//    K0 短按             = 静音切换
//    K0 长按             = 主页面 / 信息页
// ---------------------------------------------------------------------------
int16_t   g_pendSel   = -1;      // 旋转预览到的台号（-1 = 没有待切台）
uint32_t  g_pendAt    = 0;       // 最后一次旋转时刻（停手计时）
bool      g_volMode   = false;   // 音量模式
uint32_t  g_volAt     = 0;       // 最后一次音量调整时刻（超时退出用）
bool      g_volByKnob = false;   // 本次按压期间调过音量 -> 抑制随后的短按暂停

// 供 Control.cpp 查询配网是否开启
bool portalActive() { return portal.active(); }

// ---------------------------------------------------------------------------
//  函数声明
// ---------------------------------------------------------------------------
void step1_network();
void step2_3_fetchAndPlay();
void pollButton();
void pollKnob();
void commitPendingStation();
void expireVolumeMode();
void printKnob();
void togglePortal();
void enterPortal();
void exitPortal();
void handleCommand(const String& line);
void cmdWifi(const String& arg);
void cmdPlay(const String& arg);
void cmdVolume(const String& arg);
void cmdMute(const String& arg);
void cmdTone(const String& arg);
void printHelp();
void printStatus();
void printButton();
void playStation(uint8_t idx);
void onStationDead();

// ---------------------------------------------------------------------------
//  setup
// ---------------------------------------------------------------------------
void setup() {
    con.begin();
    delay(200);
    g_bootMs = millis();

    con.banner();

    // 开机第一件事：报告本次复位原因。间歇性断音如果伴随"它自己重启"，
    // 这里能直接看出是崩溃(PANIC)/看门狗/供电不足，比事后猜快得多。
    Serial.printf("[系统] 本次复位原因: %s\n", EvLog::resetReasonText());
    EvLog::add("开机，复位原因: %s", EvLog::resetReasonText());
    Serial.printf("[系统] 固件 %s v%s\n", FW_NAME, FW_VERSION);

    btn.begin();     // 板载 BOOT 键：长按进/出配网，短按下一台

    // 关掉板载 RGB 灯（WS2812）：发一帧全黑。之前 SDA 误接 GPIO48 时数据灌进灯里，
    // 灯会残留随机颜色一直乱闪。两个可能的灯脚都发一次，无效的引脚上不会有副作用。
    neopixelWrite(PIN_RGB_LED,     0, 0, 0);
    neopixelWrite(PIN_RGB_LED_ALT, 0, 0, 0);

    // 屏幕 + 旋钮：先于网络初始化，开机画面立即可见
    disp.begin();
    knob.begin();

    // 先初始化 I2S，即使网络没通也能确认音频链路
    player.begin();
    // 地址彻底失效时自动跳下一台（否则整机会守着一个死地址永久无声）
    player.setDeadHandler(onStationDead);

    control.begin(&net);   // 注册网页控制台路由

    Stations::loadCustom();   // 先读自定义电台，索引范围依赖总数
    Stations::loadIndex(g_station);

    net.begin();     // 从 NVS 读取已保存的 WiFi 凭据（漏掉这行就永远"未配置"）

    step1_network();

    if (net.isConnected() && !portal.active()) {
        step2_3_fetchAndPlay();
    }

    con.prompt();
}

// ---------------------------------------------------------------------------
//  loop
// ---------------------------------------------------------------------------
void loop() {
    con.loop();

    String line;
    if (con.available(line)) {
        handleCommand(line);
        con.prompt();
    }

    pollButton();    // 按键：长按 5s 进/出配网，短按下一台

    pollKnob();      // 旋钮：转=选台，按住转=音量，按=暂停，长按=音量模式
    commitPendingStation();   // 停手 0.25s 后真正切台
    expireVolumeMode();       // 音量模式 5s 无操作自动退出

    // 列表高亮跟随旋钮预览（还没切台时也先高亮，操作才跟手）
    uint8_t viewIdx = (g_pendSel >= 0) ? (uint8_t)g_pendSel : g_station;
    disp.loop(player, viewIdx, portal.active());   // 屏幕 UI（分区局部刷新）

    portal.loop();   // 配网热点：HTTP / DNS / 连接状态机

    control.loop();  // 局域网网页控制台（配网时自动挂起）

    if (!portal.active()) net.loop();   // 掉线自动重连（配网时不抢）
    ensureMdns();                       // radio.local 注册/维护
    player.loop();                      // 音频任务 + 掉流重连

    delay(1);
}

// ---------------------------------------------------------------------------
//  [1/3] 配置网络
// ---------------------------------------------------------------------------
void step1_network() {
    Serial.println();
    Serial.println("[1/3] 配置网络");

    if (!net.hasCredentials()) {
        Serial.println("      未检测到已保存的 WiFi，进入手机配网模式");
        portal.begin(&net);
        disp.setPortalInfo(portal.apSsid(), portal.apIp());
        return;
    }

    Serial.printf("      已保存配置: %s\n", net.ssid().c_str());
    Serial.println("      正在连接 ...");
    if (net.connect(WIFI_CONNECT_TIMEOUT_MS)) {
        Serial.println("      网络就绪。");
    } else {
        Serial.println("      连接失败，进入手机配网模式（也可继续用串口配置）：");
        Serial.println("        wifi           查看当前配置与状态");
        Serial.println("        wifi scan      扫描周边热点");
        Serial.println("        wifi <SSID> <密码>  重新配置");
        Serial.println("        wifi clear     清除已保存的配置");
        portal.begin(&net);
        disp.setPortalInfo(portal.apSsid(), portal.apIp());
    }
}

// ---------------------------------------------------------------------------
//  [2/3] 获取节目地址  +  [3/3] 播放
// ---------------------------------------------------------------------------
void step2_3_fetchAndPlay() {
    const Station& s = Stations::get(g_station);

    Serial.println();
    Serial.println("[2/3] 获取播放节目地址");
    Serial.printf("      节目源 : 内置节目表（共 %u 个）\n", Stations::count());
    Serial.printf("      选中   : #%u  %s  [%s]\n", g_station + 1, s.name, s.note);
    Serial.printf("      地址   : %s\n", s.url);

    if (!net.isConnected()) {
        Serial.println("      网络未连接，暂不播放（连上后用 play 开始）");
        return;
    }

    Serial.println();
    Serial.println("[3/3] 开始播放");
    if (!player.playUrl(s.url, s.name)) {
        Serial.println("      提示：用 list 换个台，或 url <地址> 临时测试别的流");
    }
}

// ---------------------------------------------------------------------------
//  按键处理（BOOT 键 / GPIO0）
//   长按 5 秒  : 未配网时 -> 进入配网模式；配网中 -> 退出配网并重连
//   短按       : 下一台（BTN_SHORT_PRESS_NEXT 可关）
// ---------------------------------------------------------------------------
void pollButton() {
    btn.loop();
    if (btn.isPressed()) disp.poke();    // BOOT 按下即点亮屏幕（熄屏时轻点一下就能看）

    // 按住过程中的进度提示（没有屏幕，只能靠串口看）
    static uint32_t lastSec = 0;
    if (btn.isPressed()) {
        uint32_t sec = btn.heldMs() / 1000;
        if (sec > lastSec) {
            lastSec = sec;
            if (sec == 1) {
                Serial.printf("\n[按键] 按住中 ... 继续按到 %u 秒可%s配网模式\n",
                              (unsigned)(BTN_LONGPRESS_MS / 1000),
                              portal.active() ? "退出" : "进入");
            } else if (sec * 1000 < BTN_LONGPRESS_MS) {
                Serial.printf("[按键] %lu 秒 ...\n", (unsigned long)sec);
            }
        }
    } else {
        lastSec = 0;
    }

    if (btn.consumeLongPress()) {
        togglePortal();
        con.prompt();
    }

#if BTN_SHORT_PRESS_NEXT
    if (btn.consumeShortPress()) {
        if (portal.active()) {
            Serial.println("[按键] 短按：配网模式下不响应（长按可退出配网）");
        } else if (Stations::count() == 0) {
            Serial.println("[按键] 短按：节目表为空");
        } else {
            uint8_t n = (g_station + 1) % Stations::count();
            Serial.printf("[按键] 短按：切换到下一台 #%u\n", n + 1);
            playStation(n);
        }
        con.prompt();
    }
#endif
}

// ---------------------------------------------------------------------------
//  旋钮 / 独立按键（K0）—— v0.6.6 交互
//   旋转（默认）  : 选台，列表高亮跟随，停手 0.25s 自动切台播放
//   按住 + 旋转   : 调音量（随手按下去转，无需先切模式）
//   旋钮长按 1s   : 锁定 / 退出"音量模式"（5 秒无操作自动退出）
//   旋钮短按      : 暂停 / 继续（音量模式下 = 退出音量模式）
//   K0 短按       : 静音切换
//   K0 长按       : 屏幕 主页面 <-> 信息页
// ---------------------------------------------------------------------------

void pollKnob() {
    knob.loop();

    // ---- 旋转：音量模式或按住时调音量；否则选台预览（停手后切台）----
    int8_t r = knob.consumeRotation();
    if (r != 0) {
        disp.poke();                       // 交互点亮屏幕（熄屏时旋转同时生效）
        if (portal.active()) {
            Serial.println("[旋钮] 配网模式下不响应");
        } else if (disp.page() == 1) {
            disp.scrollInfo(r);                // 信息页：旋转 = 滚动查看详情
        } else if (g_volMode || knob.pushed()) {
            if (!g_volMode) {                      // 按住直接转 = 临时进入音量模式
                g_volMode = true;
                disp.setVolumeMode(true);
                Serial.println("[旋钮] 按住旋转 -> 音量模式");
            }
            if (player.muted()) player.setMute(false);   // 转动即恢复声音
            int v = (int)player.volume() + r;
            if (v < 0) v = 0;
            if (v > MAX_VOLUME) v = MAX_VOLUME;
            player.setVolume((uint8_t)v);
            g_volAt = millis();
            g_volByKnob = true;                    // 松手时不要误触发暂停
            Serial.printf("[旋钮] 音量 %u/%u\n", player.volume(), MAX_VOLUME);
        } else {
            uint8_t n = Stations::count();
            if (n > 0) {
                int16_t base = (g_pendSel >= 0) ? g_pendSel : (int16_t)g_station;
                int16_t sel  = base + r;
                while (sel < 0)      sel += n;
                while (sel >= (int16_t)n) sel -= n;
                g_pendSel = sel;
                g_pendAt  = millis();
                Serial.printf("[旋钮] 选台 #%d/%u（停手 0.25s 播放）\n", sel + 1, n);
            }
        }
    }

    // ---- 长按：锁定 / 退出音量模式 ----
    if (knob.consumePushLong()) {
        disp.poke();
        if (portal.active()) {
            Serial.println("[旋钮] 配网模式下不响应");
        } else {
            g_volMode = !g_volMode;
            disp.setVolumeMode(g_volMode);
            g_volAt = millis();
            g_volByKnob = false;                   // 长按本身不算调过音量
            Serial.printf("[旋钮] 长按：%s音量模式\n", g_volMode ? "进入" : "退出");
        }
    }

    // ---- 短按：暂停/继续；音量模式下先退出模式 ----
    if (knob.consumePushShort()) {
        disp.poke();
        if (portal.active()) {
            Serial.println("[旋钮] 配网模式下不响应");
        } else if (g_volByKnob) {
            g_volByKnob = false;                   // 按住旋转后松手，不当作暂停
        } else if (g_volMode) {
            g_volMode = false;
            disp.setVolumeMode(false);
            Serial.println("[旋钮] 短按：退出音量模式");
        } else {
            Player::State st = player.state();
            if (st == Player::ST_PLAYING || st == Player::ST_PAUSED) {
                Serial.printf("[旋钮] 短按：%s\n", player.togglePause() ? "继续" : "暂停");
            } else if (Stations::count() > 0) {
                Serial.printf("[旋钮] 短按：播放 #%u\n", g_station + 1);
                playStation(g_station);
            }
        }
    }

    if (knob.consumeKeyShort()) {
        disp.poke();
        if (portal.active()) {
            Serial.println("[K0] 配网模式下不响应");
        } else {
            player.setMute(!player.muted());
            Serial.printf("[K0] 短按：静音 %s\n", player.muted() ? "开" : "关");
        }
    }

    if (knob.consumeKeyLong()) {
        disp.poke();
        disp.toggleInfoPage();
        Serial.println("[K0] 长按：切换信息页");
    }
}

// 旋钮停下来 0.25 秒才真正切台：快速连转只发一次请求
void commitPendingStation() {
    if (g_pendSel < 0) return;
    if (millis() - g_pendAt < KNOB_SEL_COMMIT_MS) return;
    uint8_t sel = (uint8_t)g_pendSel;
    g_pendSel = -1;
    if (sel == g_station) return;
    Serial.printf("[旋钮] 切台 -> #%u %s\n", sel + 1, Stations::get(sel).name);
    playStation(sel);
}

// 音量模式 5 秒无操作自动退出，避免"以为在调音量结果在换台"
void expireVolumeMode() {
    if (!g_volMode) return;
    if (millis() - g_volAt < KNOB_VOLMODE_TIMEOUT_MS) return;
    g_volMode = false;
    disp.setVolumeMode(false);
    Serial.println("[旋钮] 音量模式超时退出");
}

// 旋钮/按键诊断计数（旋转不动时先看 rawEdges 是否为 0 -> A/B 线没接通）
void printKnob() {
    Serial.println();
    Serial.println("---------- 旋钮 / K0 按键诊断 ----------");
    Serial.printf(" 旋转中断边沿 : %lu  （转一下应为 4 的倍数；保持 0 = A/B 线未接通）\n",
                  (unsigned long)knob.rawEdges());
    Serial.printf(" 有效定位格   : %lu  （rawEdges 有数而它为 0 = 解码/脉冲数不匹配）\n",
                  (unsigned long)knob.rotCount());
    Serial.printf(" 旋钮短按计数 : %u    K0 短按计数 : %u\n", knob.pushCount(), knob.keyCount());
    Serial.println("----------------------------------------");
}

void togglePortal() {
    if (portal.active()) {
        exitPortal();
    } else {
        enterPortal();
    }
}

// 进入配网：断开当前 WiFi（凭据保留在 NVS），开热点等手机配置
void enterPortal() {
    Serial.println();
    Serial.println("[配网] 进入配网模式（长按 BOOT 或 wifi portal off 可退出）");

    if (player.state() == Player::ST_PLAYING || player.state() == Player::ST_PAUSED) {
        player.stop();          // 先停播，免得扫描热点时卡顿 / 爆音
    }
    WiFi.disconnect(false, false);   // 只断开连接，不擦除已保存的凭据
    delay(300);

    control.pauseServer();   // 关键：释放 80 端口，否则配网门户网页服务绑定失败

    portal.begin(&net);
    disp.setPortalInfo(portal.apSsid(), portal.apIp());
    if (portal.active()) {
        Serial.printf("[配网] 热点 %s 已开启，手机连上后访问 http://%s/\n",
                      portal.apSsid().c_str(), portal.apIp().c_str());
    }
}

// 退出配网：关热点，用 NVS 里已保存的凭据重连并恢复播放
void exitPortal() {
    Serial.println();
    Serial.println("[配网] 退出配网模式，用已保存的配置重连");

    portal.stop();

    if (!net.hasCredentials()) {
        Serial.println("[配网] 还没有保存过 WiFi，无法退出配网");
        Serial.println("       继续用手机配置，或串口执行: wifi <SSID> <密码>");
        portal.begin(&net);
        return;
    }

    if (net.connect(WIFI_CONNECT_TIMEOUT_MS)) {
        Serial.println("[配网] 已重新连上，恢复播放");
        step2_3_fetchAndPlay();
    } else {
        Serial.println("[配网] 重连失败，重新开启配网热点");
        portal.begin(&net);
        disp.setPortalInfo(portal.apSsid(), portal.apIp());
    }
}

// ---------------------------------------------------------------------------
//  命令分发
// ---------------------------------------------------------------------------
void handleCommand(const String& line) {
    disp.poke();                         // 串口有输入 = 用户在操作，点亮屏幕
    int    sp  = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? String("") : line.substring(sp + 1);
    arg.trim();
    cmd.toLowerCase();

    if (cmd == "help" || cmd == "?" || cmd == "h") {
        printHelp();
    } else if (cmd == "status" || cmd == "st") {
        printStatus();
    } else if (cmd == "wifi" || cmd == "w") {
        cmdWifi(arg);
    } else if (cmd == "list" || cmd == "ls") {
        Stations::printAll(g_station);
    } else if (cmd == "play" || cmd == "p") {
        cmdPlay(arg);
    } else if (cmd == "next" || cmd == "n") {
        playStation((g_station + 1) % Stations::count());
    } else if (cmd == "prev" || cmd == "b") {
        playStation((g_station + Stations::count() - 1) % Stations::count());
    } else if (cmd == "stop") {
        player.stop();
    } else if (cmd == "pause") {
        player.togglePause();
    } else if (cmd == "vol" || cmd == "v") {
        cmdVolume(arg);
    } else if (cmd == "mute") {
        cmdMute(arg);
    } else if (cmd == "tone" || cmd == "beep") {
        cmdTone(arg);
    } else if (cmd == "url") {
        if (arg.length() == 0) {
            Serial.println("用法: url http://xxx/stream.mp3");
        } else if (!net.isConnected()) {
            Serial.println("[播放] 网络未连接，无法播放");
        } else {
            Serial.println("[播放] 使用临时地址（不写入节目表）");
            player.playUrl(arg, "临时地址");
        }
    } else if (cmd == "info" || cmd == "i") {
        player.printInfo();
    } else if (cmd == "btn" || cmd == "button" || cmd == "key") {
        printButton();
    } else if (cmd == "knob") {
        printKnob();
    } else if (cmd == "tft") {
        if (arg == "test") {
            disp.colorTest();
        } else if (arg == "scan") {
            disp.freqScan();
        } else if (arg == "pins") {
            disp.pinSweep();
        } else if (arg.startsWith("set")) {
            int a, b, c, d, e;
            if (sscanf(arg.c_str(), "set %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
                disp.applyPins(a, b, c, d, e);
            } else {
                Serial.println("用法: tft set <sclk> <mosi> <cs> <dc> <rst>   （rst 填 -1 表示不用复位脚）");
            }
        } else if (arg.startsWith("inv")) {
            static bool inv = TFT_INVERT;
            if (arg.length() > 4) inv = (arg.substring(4).toInt() != 0);
            else                  inv = !inv;
            disp.setInvert(inv);
        } else if (arg.startsWith("rgb")) {
            static bool bgr = false;
            if (arg.length() > 4) bgr = (arg.substring(4).toInt() != 0);
            else                  bgr = !bgr;
            disp.setRgbOrder(bgr);
        } else if (arg.startsWith("rot")) {
            int r = (arg.length() > 4) ? arg.substring(4).toInt() : 1;
            disp.setRotationN((uint8_t)constrain(r, 0, 3));
        } else {
            Serial.printf("[屏幕] 重新初始化... %s\n", disp.reinit() ? "成功" : "失败");
        }
    } else if (cmd == "log") {
        if (arg == "on")  player.setVerbose(true);
        if (arg == "off") player.setVerbose(false);
        Serial.printf("[日志] 音频详细日志: %s\n", player.verbose() ? "开" : "关");
    } else if (cmd == "bright") {
        if (arg.length() && isDigit(arg[0])) {
            disp.setBrightness((uint8_t)constrain(arg.toInt(), 0, 255));
        }
        Serial.printf("[屏幕] 背光亮度 %u/255\n", disp.brightness());
    } else if (cmd == "dim") {
        if (arg.length() && isDigit(arg[0])) {
            long sec = constrain(arg.toInt(), 0, 3600);
            disp.setIdleOff((uint32_t)sec * 1000UL);
        }
        Serial.printf("[屏幕] 当前熄屏时长: %lu 秒（0=常亮）\n",
                      (unsigned long)(disp.idleOff() / 1000));
    } else if (cmd == "reboot" || cmd == "restart") {
        Serial.println("重启中 ...");
        delay(200);
        ESP.restart();
    } else if (cmd.length() == 0) {
        // 空行忽略
    } else {
        Serial.printf("未知命令: %s   （输入 help 查看命令）\n", cmd.c_str());
    }
}

// ---------------------------------------------------------------------------
//  wifi 子命令
// ---------------------------------------------------------------------------
void cmdWifi(const String& arg) {
    String a = arg;
    a.trim();

    if (a.length() == 0) {
        net.printStatus();
        return;
    }

    String sub = a;
    int    sp  = a.indexOf(' ');
    if (sp > 0) sub = a.substring(0, sp);
    sub.toLowerCase();

    if (sub == "portal") {
        String rest = (sp > 0) ? a.substring(sp + 1) : String("");
        rest.trim();
        rest.toLowerCase();
        if (rest == "off" || rest == "close" || rest == "exit") {
            if (portal.active()) {
                exitPortal();
            } else {
                Serial.println("[配网] 当前没有开启配网模式");
            }
        } else {
            enterPortal();
        }
        return;
    }
    if (sub == "list") {
        net.printList();
        return;
    }
    if (sub == "del") {
        String name = (sp > 0) ? a.substring(sp + 1) : String("");
        name.trim();
        if (name.length() == 0) {
            Serial.println("用法: wifi del <SSID>     先用 wifi list 查看已存网络");
        } else {
            net.del(name);
        }
        return;
    }
    if (sub == "scan") {
        net.scan();
        return;
    }
    if (sub == "clear") {
        net.clear();
        Serial.println("重启后进入手机配网模式 ...");
        delay(1200);
        ESP.restart();
    }
    if (sub == "reconnect") {
        if (net.connect(WIFI_CONNECT_TIMEOUT_MS)) {
            portal.stop();   // 串口配好了就关掉配网热点
            step2_3_fetchAndPlay();
        }
        return;
    }

    // wifi <SSID> [密码]
    String ssid = a;
    String pass = "";
    if (sp > 0) {
        ssid = a.substring(0, sp);
        pass = a.substring(sp + 1);
        pass.trim();
    }

    if (ssid.length() == 0) {
        Serial.println("用法: wifi <SSID> <密码>   密码为空表示开放网络");
        return;
    }

    Serial.printf("[WiFi] 保存配置 SSID=\"%s\" %s（可保存多个，自动切换）\n", ssid.c_str(),
                  pass.length() ? "(密码已记录)" : "(开放网络)");
    if (!net.save(ssid, pass)) {
        Serial.println("[WiFi] 警告：配置没能写入 NVS，重启后会丢失");
    }

    if (net.connect(WIFI_CONNECT_TIMEOUT_MS)) {
        portal.stop();   // 串口配好了就关掉配网热点
        step2_3_fetchAndPlay();
    } else {
        Serial.println("      连接失败，检查密码 / 热点是否为 2.4G / 信号强度");
    }
}

// ---------------------------------------------------------------------------
//  play / vol / mute
// ---------------------------------------------------------------------------
void cmdPlay(const String& arg) {
    if (arg.length() == 0) {
        playStation(g_station);
        return;
    }

    if (!isDigit(arg[0])) {
        Serial.println("用法: play <序号>   序号从 1 开始，list 查看节目表");
        return;
    }

    int n = arg.toInt();
    if (n < 1 || n > (int)Stations::count()) {
        Serial.printf("序号超范围，有效范围 1~%u\n", Stations::count());
        return;
    }
    playStation((uint8_t)(n - 1));
}

void playStation(uint8_t idx) {
    if (idx >= Stations::count()) idx = 0;
    g_station = idx;
    Stations::saveIndex(idx);
    EvLog::add("换台 #%u %s", idx + 1, Stations::get(idx).name);

    if (!net.isConnected()) {
        Serial.println("[播放] 网络未连接，先配置 WiFi: wifi <SSID> <密码>");
        return;
    }
    step2_3_fetchAndPlay();
}

// 连续多次连不上（地址失效）时由 Player 回调：自动跳到下一台。
// 单台电台失效不应该让整机永久无声 —— 这是收音机该有的行为。
void onStationDead() {
    if (Stations::count() == 0) return;
    uint8_t next = (uint8_t)((g_station + 1) % Stations::count());
    Serial.printf("[播放] 当前电台疑似失效，自动跳到 #%u %s\n", next + 1,
                  Stations::get(next).name);
    EvLog::add("自动跳台 -> #%u", next + 1);
    playStation(next);
}

void cmdVolume(const String& arg) {
    if (arg.length() == 0) {
        Serial.printf("[音量] 当前 %u/%u\n", player.volume(), MAX_VOLUME);
        return;
    }
    if (arg == "+") {
        player.volumeDelta(+1);
    } else if (arg == "-") {
        player.volumeDelta(-1);
    } else if (isDigit(arg[0])) {
        int v = arg.toInt();
        if (v < 0 || v > MAX_VOLUME) {
            Serial.printf("音量范围 0~%d\n", MAX_VOLUME);
            return;
        }
        player.setVolume((uint8_t)v);
    } else {
        Serial.println("用法: vol 0~21 | vol + | vol -");
    }
}

void cmdMute(const String& arg) {
    if (arg == "on") {
        player.setMute(true);
    } else if (arg == "off") {
        player.setMute(false);
    } else {
        player.setMute(!player.muted());
    }
}

// ---------------------------------------------------------------------------
//  硬件测试音：tone [频率] [秒]
//  绕开音频库，确认 功放 / 喇叭 / I2S 接线 是否正常
// ---------------------------------------------------------------------------
void cmdTone(const String& arg) {
    uint32_t freq = 1000;
    uint32_t sec  = 3;

    String a = arg;
    a.trim();
    if (a.length()) {
        int    sp = a.indexOf(' ');
        String f  = (sp < 0) ? a : a.substring(0, sp);
        String s  = (sp < 0) ? String("") : a.substring(sp + 1);
        f.trim();
        s.trim();
        if (f.toInt() > 0) freq = (uint32_t)f.toInt();
        if (s.toInt() > 0) sec  = (uint32_t)s.toInt();
    }
    if (freq < 50)   freq = 50;
    if (freq > 8000) freq = 8000;
    if (sec == 0)    sec = 1;
    if (sec > 20)    sec = 20;

    Serial.println();
    Serial.println("[测试] 硬件自检：绕开音频库，直接向 MAX98357A 推正弦波");

    Player::State st = player.state();
    if (st == Player::ST_PLAYING || st == Player::ST_PAUSED || st == Player::ST_CONNECTING) {
        player.stop();
        delay(150);
    }

    bool ok = tonePlay(freq, sec * 1000, 90);

    Serial.println();
    if (ok) {
        Serial.println("[测试] 听到连续的“嘟——” -> 功放 / 喇叭 / I2S 接线都正常");
        Serial.println("[测试] 完全没声音        -> 查 VIN 5V、GND 共地、SD 引脚是否接高、喇叭接 OUT+/OUT-");
    } else {
        Serial.println("[测试] 测试通道创建失败：I2S 引脚可能被占用或冲突");
    }
    Serial.println("[测试] 5 秒后自动重启，恢复正常收音 ...");
    delay(5000);
    ESP.restart();
}

// ---------------------------------------------------------------------------
//  help / status
// ---------------------------------------------------------------------------
void printHelp() {
    Serial.println();
    Serial.println("================= 命令列表 =================");
    Serial.println(" 网络（可保存多个 WiFi，优先连最近成功的，失败自动换）");
    Serial.println("   wifi                    查看 WiFi 状态");
    Serial.println("   wifi list               列出已保存的所有 WiFi");
    Serial.println("   wifi <SSID> <密码>      添加/更新并立即连接");
    Serial.println("   wifi del <SSID>         移除某个已存 WiFi");
    Serial.println("   wifi scan               扫描周边热点");
    Serial.println("   wifi portal             开启手机配网热点");
    Serial.println("   wifi portal off         关闭配网热点并重连");
    Serial.println("   wifi reconnect          用已保存配置重连");
    Serial.println("   wifi clear              清除所有已存 WiFi 并进入配网模式");
    Serial.println(" 按键（板载 BOOT，GPIO0）");
    Serial.println("   长按 5 秒               进入 / 退出配网模式");
    Serial.println("   短按                    下一台");
    Serial.println("   btn                     查看按键状态与计数");
    Serial.println(" 旋钮 / 独立按键 K0（接屏幕板）");
    Serial.println("   旋转                    音量增减（静音时转动恢复声音）");
    Serial.println("   旋转                    选台（停手 0.25 秒自动播放）");
    Serial.println("   按住旋钮 + 旋转          调音量");
    Serial.println("   旋钮短按                暂停 / 继续（音量模式下=退出）");
    Serial.println("   旋钮长按 1 秒            锁定/退出音量模式");
    Serial.println("   K0 短按                 静音切换");
    Serial.println("   K0 长按 1 秒            屏幕 主页面/信息页 切换");
    Serial.println("   knob                    旋钮/按键诊断计数");
    Serial.println(" 屏幕");
    Serial.println("   tft                     重新初始化屏幕（白屏时可试）");
    Serial.println("   tft test                颜色自检（红绿蓝白黑，判断信号是否到达面板）");
    Serial.println("   tft scan                SPI 频率扫描（4/8/16/27/40MHz 各刷红屏，找可用频率）");
    Serial.println("   tft pins                引脚组合扫描（CS/DC/RST 全排列，排查接反）");
    Serial.println("   tft set <sclk> <mosi> <cs> <dc> <rst>   运行时换引脚（改接线后免烧录验证）");
    Serial.println("   tft inv [0/1]           反色开关（白底黑字 <-> 黑底白字）");
    Serial.println("   tft rgb [0/1]           颜色顺序（红蓝偏色时切换）");
    Serial.println("   tft rot <0-3>           旋转方向（1=横屏）");
    Serial.println("   bright <0-255>          背光亮度");
    Serial.println("   dim <0-3600>            无操作熄屏秒数（0=常亮，默认 10）");
    Serial.println(" 节目");
    Serial.println("   list                    列出电台表（含网页端添加的自定义台）");
    Serial.println("   play <n>                播放第 n 个节目（从 1 开始）");
    Serial.println("   play                    重播当前节目");
    Serial.println("   next / prev             下一个 / 上一个节目");
    Serial.println("   url <地址>              临时播放任意流地址");
    Serial.println(" 播放控制");
    Serial.println("   stop                    停止");
    Serial.println("   pause                   暂停 / 继续");
    Serial.println("   vol <0-21> | vol + | -  音量");
    Serial.println("   mute [on|off]           静音切换");
    Serial.println(" 调试");
    Serial.println("   status                  总状态（网络+音频）");
    Serial.println("   tone [频率] [秒]        硬件测试音（默认 1000Hz 3 秒，测完自动重启）");
    Serial.println("   info                    音频详细参数");
    Serial.println("   log on|off              库的详细日志开关");
    Serial.println("   reboot                  重启");
    Serial.println("   help                    本帮助");
    Serial.println("============================================");
}

void printStatus() {
    Serial.println();
    Serial.println("================= 状态 =================");
    Serial.printf(" 固件     : %s v%s\n", FW_NAME, FW_VERSION);
    Serial.printf(" 运行时间 : %lu s\n", (unsigned long)((millis() - g_bootMs) / 1000));
    Serial.printf(" 空闲内存 : %lu B   历史最低: %lu B\n", (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap());
    Serial.println();
    net.printStatus();
    Serial.printf("  BOOT 键  : GPIO%d %s（长按 %u 秒进/出配网）\n", PIN_BOOT_BTN,
                  btn.isPressed() ? "按下中" : "松开", (unsigned)(BTN_LONGPRESS_MS / 1000));
    if (portal.active()) {
        Serial.printf("  配网热点 : %s    http://%s/\n", portal.apSsid().c_str(),
                      portal.apIp().c_str());
    }
    Serial.println();
    player.printInfo();
    Serial.println("========================================");
}

void printButton() {
    Serial.println();
    Serial.println("----------- BOOT 按键（GPIO0）-----------");
    Serial.printf(" 当前状态 : %s", btn.isPressed() ? "按下中" : "松开");
    if (btn.isPressed()) Serial.printf("，已按住 %lu ms", (unsigned long)btn.heldMs());
    Serial.println();
    Serial.printf(" 长按阈值 : %lu ms（进/出配网模式）\n", (unsigned long)btn.longPressMs());
    Serial.println(" 短按行为 : 下一台");
    Serial.printf(" 触发计数 : 长按 %u 次，短按 %u 次\n", btn.longCount(), btn.shortCount());
    Serial.printf(" 配网模式 : %s\n", portal.active() ? "已开启" : "未开启");
    Serial.println(" 提示     : 按住不放，串口会打印秒数进度，到阈值自动触发");
    Serial.println("-----------------------------------------");
}
