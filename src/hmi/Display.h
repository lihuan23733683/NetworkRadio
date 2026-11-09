// ============================================================================
//  Display.h  --  2.4 寸 TFT 屏（ST7789V 320x240 横屏）UI  v3 纯黑高对比
//  布局：
//    ┌────────────────────────────────────────┐
//    │ 网络收音机                     已连接    │ ← 顶栏 22px
//    │ ┌────────────────────────────────────┐ │
//    │ │ 正在播放 · 01              播放中   │ │
//    │ │ 中国之声（大字）                     │ │ ← 卡片 72px
//    │ │ HLS/AAC · 192kbps                  │ │
//    │ │ ▁▂▅█ 小型 VU 电平条                │ │
//    │ └────────────────────────────────────┘ │
//    │ 音量  ▮▮▮▮▮▮▮▮▯▯▯▯▯▯▯▯        65%     │ ← 音量行
//    │ 电台列表                                │
//    │ ▌01 中国之声        （正在播：绿标）    │ ← 列表 4 行可见
//    │  02 音乐之声                            │
//    │  03 经济之声                            │
//    │  04 经典音乐广播                        │
//    │  旋转音量 · 短按暂停 · 长按下一台        │ ← 底部提示 16px
//    └────────────────────────────────────────┘
//  配色：纯黑底 + 白字高对比；正在播行深绿底白字
//  刷新：分区脏标记——每个区域只在内容变化时局部重绘，消除整页闪烁
//  页面：主页面 / 信息页（K0 长按切换）
// ============================================================================
#pragma once
#include <Arduino.h>
#include <LovyanGFX.h>

#include "../core/AppConfig.h"
#include "../audio/Player.h"

class Display {
public:
    bool begin();
    bool reinit();                          // 屏幕异常（白屏）时串口 "tft" 重初始化
    void colorTest();                       // 串口 "tft test"：全屏红绿蓝白黑循环，判断信号是否到达面板
    void freqScan();                        // 串口 "tft scan"：依次用多个 SPI 频率各刷一次红屏，找出面板能收下的频率
    void pinSweep();                        // 串口 "tft pins"：CS/DC/RST 引脚全排列 + SCLK/MOSI 对调，找能点亮面板的组合
    bool applyPins(int sclk, int mosi, int cs, int dc, int rst);  // 串口 "tft set ..."：运行时换引脚并刷红屏验证
    void setInvert(bool on);                // 串口 "tft inv 0/1"：反色开关（白底黑字 <-> 黑底白字）
    bool setRgbOrder(bool bgr);             // 串口 "tft rgb 0/1"：红蓝顺序（颜色偏色时切换）
    void setRotationN(uint8_t r);           // 串口 "tft rot 0..3"：旋转方向
    void loop(Player& p, uint8_t curIdx, bool portalOn);
    void setPortalInfo(const String& ssid, const String& ip);  // 配网前由 .ino 填入热点名与 IP

    void toggleInfoPage();                  // 主页面 <-> 信息页
    void poke();                            // 用户交互（旋钮/按键/串口）：点亮屏幕并重置熄屏计时
    void setIdleOff(uint32_t ms);           // 无操作熄屏时长(ms)，0=常亮；存 NVS
    uint32_t idleOff() const { return _idleOffMs; }
    bool screenOn() const { return _screenOn; }
    void scrollInfo(int dir);               // 信息页滚动（信息页时旋钮旋转调用）
    void setVolumeMode(bool on);            // 音量模式：音量行高亮 + 底部提示（旋钮按住时）
    uint8_t page() const { return _page; }
    void setBrightness(uint8_t b);          // 0..255（PWM 背光）
    uint8_t brightness() const { return _bright; }

private:
    // ---- LovyanGFX 自定义配置：驱动按 TFT_DRIVER 宏选择，SPI 总线共用 ----
    class LGFX : public lgfx::LGFX_Device {
#if TFT_DRIVER == 1
        lgfx::Panel_ILI9341 _panel;
#elif TFT_DRIVER == 2
        lgfx::Panel_ST7796  _panel;
#else
        lgfx::Panel_ST7789  _panel;
#endif
        lgfx::Bus_SPI      _bus;
    public:
        LGFX() {
            {
                auto cfg = _bus.config();
                cfg.spi_host  = SPI2_HOST;          // ESP32-S3 的 FSPI
                cfg.pin_sclk  = PIN_TFT_SCLK;
                cfg.pin_mosi  = PIN_TFT_MOSI;
                cfg.pin_dc    = PIN_TFT_DC;
                cfg.spi_mode  = 0;
                cfg.spi_3wire = false;              // 4 线 SPI（DC 独立接线）
                cfg.freq_write= TFT_SPI_FREQ;
                cfg.freq_read = 8000000;
                _bus.config(cfg);
                _panel.setBus(&_bus);
            }
            {
                auto cfg = _panel.config();
                cfg.pin_cs       = PIN_TFT_CS;
                cfg.pin_rst      = PIN_TFT_RST;
                cfg.pin_busy     = -1;
                cfg.panel_width  = TFT_NATIVE_W;    // 控制器原生竖屏尺寸
                cfg.panel_height = TFT_NATIVE_H;
                cfg.offset_x     = 0;
                cfg.offset_y     = 0;
                cfg.offset_rotation = 0;
                cfg.invert       = TFT_INVERT;      // IPS 屏一般要反色
                cfg.rgb_order    = false;           // 颜色红蓝反了改 true
                _panel.config(cfg);
            }
            setPanel(&_panel);
        }

        // 运行时改 SPI 频率并重新初始化（供 freqScan 逐档试）
        bool reinitAt(uint32_t freq) {
            auto cfg = _bus.config();
            cfg.freq_write = freq;
            _bus.config(cfg);
            return init();
        }

        // 运行时改颜色顺序 / 反色并重新初始化
        bool reinitWith(bool invert, bool bgr) {
            auto p = _panel.config();
            p.invert    = invert;
            p.rgb_order = bgr;
            _panel.config(p);
            return init();
        }

        // 运行时改引脚分配并重新初始化（供 pinSweep 排除 CS/DC/RST 接反）
        bool tryPins(int sclk, int mosi, int cs, int dc, int rst, uint32_t freq) {
            auto b = _bus.config();
            b.pin_sclk   = sclk;
            b.pin_mosi   = mosi;
            b.pin_dc     = dc;
            b.freq_write = freq;
            _bus.config(b);

            auto p = _panel.config();
            p.pin_cs  = cs;
            p.pin_rst = rst;
            _panel.config(p);
            return init();
        }
    };

    void drawSplash();
    void drawPortalPage();                  // 配网模式专属页：热点名 + IP 大字指导
    void drawInfo(Player& p, uint8_t curIdx);
    // ---- 自适应布局：按 _lcd 当前分辨率计算各分区位置（drawAll 等整页重绘前刷新）----
    struct Layout {
        int w, h;                                        // 画布
        int yTop, hTop;                                  // 顶栏
        int yCard, hCard;                                // 正在播放卡片
        int yVol, hVol;                                  // 音量行
        int yLbl;                                        // "电台列表" 标签
        int yList, rowH, rowPitch; int listRows;         // 列表
        int yHint, hHint;                                // 底部提示
    };
    void computeLayout();
    // ---- 主页面分区 ----
    void drawTop(bool portalOn, bool wifiOk);
    void drawCard(Player& p, uint8_t curIdx);
    void drawMiniVU(Player& p);
    void drawVolume(Player& p);
    void drawList(uint8_t curIdx);
    void drawHint(Player& p, bool portalOn);
    void drawAll(Player& p, uint8_t curIdx, bool portalOn);
    const char* stateCN(Player::State s);

    LGFX     _lcd;
    Layout   _lay{};                       // 自适应布局缓存（computeLayout 刷新）
    uint8_t  _bright   = TFT_BRIGHT_DEFAULT;
    uint8_t  _page     = 0;       // 0=主页面 1=信息页
    uint8_t  _rot      = 1;       // 旋转（1=横屏 320x240）
    bool     _volMode  = false;   // 音量模式（按住/锁定旋钮调音量）
    uint32_t _lastTick = 0;
    uint32_t _lastInfo = 0;
    uint32_t _lastKeepalive = 0;         // 上次保活重初始化时间
    uint32_t _idleOffMs = TFT_IDLE_OFF_MS; // 无操作熄屏时长（0=常亮）
    bool     _screenOn = true;           // 屏幕亮/熄状态（只关背光，面板保持初始化）
    uint32_t _lastPoke  = 0;             // 上次用户交互时刻
    int      _lastVUW  = -1;
    bool     _portalDrawn = false;         // 配网页是否已铺开
    String   _apSsid, _apIp;               // 配网热点名 / IP（setPortalInfo 填入）
    int      _infoScroll = 0;              // 信息页滚动偏移（行数）

    // 走马灯：标签行右侧滚动显示 控制台 IP / radio.local
    int      _mqX0 = 0, _mqW = 0;          // 走马灯区域（drawAll 布局时设置）
    int      _mqOff = 0;                   // 滚动偏移（像素）
    uint32_t _mqLast = 0;                  // 上次滚动/开始静止的时间
    bool     _mqRestDrawn = false;         // 静止帧已画（避免反复重绘闪烁）
    String   _mqText;                      // 滚动内容
    void tickMarquee(uint32_t now);        // loop 每拍调用，推进滚动

    // 卡片内容缓存：只重画真正变化的行，避免换台整块刷新造成闪动
    bool     _cardPainted = false;
    String   _cName, _cNote;
    uint8_t  _cNum = 0xFF, _cState = 0xFF;

    // 分区变化快照（每个区域只在对应字段变化时重绘）
    struct Snap {
        uint8_t  cur, state, vol;
        bool     mute, portal, wifi, volMode;
        uint16_t br8;           // 码率/8kbps 量化，防止 HSL 段间抖动触发重绘
        String   err;
    } _snap{};
    bool _haveSnap = false;
};
