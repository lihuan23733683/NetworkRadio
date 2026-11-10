// ============================================================================
//  AppConfig.h  --  网络收音机 全局配置
//  本项目为"无屏幕 / 无旋钮"调试阶段，所有交互走串口控制台
// ============================================================================
#pragma once

#define FW_NAME     "NetworkRadio"
#define FW_VERSION  "0.7.7"

// ---------------------------------------------------------------------------
//  硬件接线（MAX98357A  I2S 功放）—— 按实际接线修改这三行即可
//  实测接线：BCLK=16（黑） LRC=17（白） DIN=18（灰），位于板顶一排
// ---------------------------------------------------------------------------
#define PIN_I2S_BCLK   16      // MAX98357A BCLK  (位时钟)
#define PIN_I2S_LRC    17      // MAX98357A LRC   (左右声道时钟 / WS)
#define PIN_I2S_DOUT   18      // MAX98357A DIN   (数据)

//  MAX98357A 其余引脚：
//    VIN  -> 5Vin（3.3V 也能响，但 5V 音量大、失真小）
//    GND  -> GND（必须与 ESP32 共地）
//    SD   -> 必须拉高！接 3V3（就在 16/17/18 旁边那排）
//            SD 悬空/接地 = 芯片关断（Shutdown），完全无声
//            >1.4V 输出左声道；0.16~0.77V 为 (L+R)/2 混音
//    GAIN -> 悬空 = 9dB；接 VDD=15dB；接 GND=12dB；100k 到 GND=3dB
//    注意：喇叭不要接到 3.3V，功放输出是 BTL 桥接，喇叭两端直接接 +/-

// ---------------------------------------------------------------------------
//  板载 BOOT 按键（长按进配网）
//  多数 ESP32-S3 开发板的 BOOT 键接在 GPIO0，按下为低电平
// ---------------------------------------------------------------------------
#define PIN_BOOT_BTN        0       // 换别的按键就改这里
#define BTN_DEBOUNCE_MS     30      // 去抖窗口
#define BTN_LONGPRESS_MS    5000    // 长按 5 秒 -> 进/出配网模式
#define BTN_SHORT_MIN_MS    60      // 短按最短有效时长（低于此算抖动）
#define BTN_SHORT_PRESS_NEXT 1      // 短按 = 下一台；置 0 关闭该行为

// ---------------------------------------------------------------------------
//  2.4 寸 TFT 屏（ST7789V，320x240 横屏）+ EC11 旋钮（20 脉冲/20 定位）+ 独立按键
//  接线：屏幕 8 根线走 SPI；旋钮 A/B/按下 + 独立按键 K0（照片丝印 KO）共 4 根信号线
//  ！！VCC 必须接 5V ！！模块丝印写 "VCC: 3.3V/5V"，但实测接 3.3V 时板载稳压后
//  逻辑电压不足 -> 背光亮但永不显示（纯白），接 5V 才正常。这是白屏排查三天的真凶。
//  信号线走 47/21/42/41/40（原 10~14 一组与板载 RGB 灯/其他外设冲突，多次排查后换组）
// ---------------------------------------------------------------------------
#define PIN_TFT_SCLK   47      // 屏幕 SCL  SPI 时钟
#define PIN_TFT_MOSI   21      // 屏幕 SDA  SPI 数据（MOSI）
#define PIN_TFT_CS     42      // 屏幕 CS   片选
#define PIN_TFT_DC     41      // 屏幕 DC   数据/命令
#define PIN_TFT_RST    40      // 屏幕 RES  复位
#define PIN_TFT_BL      9      // 屏幕 BLK  背光（PWM 可调亮度）

// 板载 RGB 灯（WS2812）：曾因 SDA 误接 GPIO48 把它当数据线，灯会残留随机颜色乱闪。
// 启动时主动发一帧全黑关掉它。不同版开发板灯脚不同：48（多数）/ 38（旧版），两个都发
#define PIN_RGB_LED     48
#define PIN_RGB_LED_ALT 38
#define TFT_BRIGHT_DEFAULT 165 // 开机默认背光 0..255（PWM 占空）；小屏不刺眼取 165，串口 bright 可调
#define TFT_SPI_FREQ   16000000 // SPI 刷新速率；跳线接触不稳时 16MHz 最稳（白屏先降这里）
// 屏幕保活看门狗：ST7789 无读回线，长时间运行偶发白屏无法检测，定时静默重初始化兜底。
// 每次保活屏幕闪约 1 秒；若把屏幕 VCC 接 5V 后白屏根治，可把间隔调大或注释掉 loop 里的保活块
#define TFT_KEEPALIVE_MS  30UL * 60UL * 1000UL   // 30 分钟
// 无操作自动熄屏（节电+护屏）：默认 10 秒。串口 dim <秒> 可改（0=常亮），存 NVS 掉电记忆
#define TFT_IDLE_OFF_MS   10000UL
#define TFT_INVERT     false    // 实测此屏不需要反色（true 时白底黑字）

// ---- 屏幕驱动型号（换屏只改这里，UI 布局会按分辨率自动适配）----
//   0 = ST7789  240x320（当前 2.4 寸屏，默认）
//   1 = ILI9341 240x320（2.4/2.8 寸常见替代驱动）
//   2 = ST7796  320x480（3.5/4 寸大屏）
#ifndef TFT_DRIVER
#define TFT_DRIVER 0
#endif
#if TFT_DRIVER == 1
#define TFT_NATIVE_W 240
#define TFT_NATIVE_H 320
#define TFT_PANEL_NAME "ILI9341"
#elif TFT_DRIVER == 2
#define TFT_NATIVE_W 320
#define TFT_NATIVE_H 480
#define TFT_PANEL_NAME "ST7796"
#else
#define TFT_NATIVE_W 240
#define TFT_NATIVE_H 320
#define TFT_PANEL_NAME "ST7789"
#endif

#define PIN_ENC_A       5      // 旋钮 A（相位 A）
#define PIN_ENC_B       4      // 旋钮 B（相位 B），方向反了把这两个对调
#define PIN_ENC_PUSH    6      // 旋钮按下
#define ENC_DETENT_STEPS 4     // 每个定位格的中断步进数（EC11 20 定位 = 每 4 步一格）
#define PIN_KEY_K0      7      // 独立按键（板上丝印 KO）
#define KNOB_LONGPRESS_MS  1000 // 旋钮/按键长按阈值
#define KNOB_DEBOUNCE_MS   30   // 旋钮按键去抖窗口
#define KNOB_VOLMODE_TIMEOUT_MS 5000 // 音量模式无旋转自动退回列表模式的时长
#define KNOB_SEL_COMMIT_MS  250      // 选台预览后停手多久才真正切台（快速连转只发一次请求）

// ---------------------------------------------------------------------------
//  串口控制台
// ---------------------------------------------------------------------------
#define SERIAL_BAUD        115200
#define CONSOLE_MAX_LINE   256

// ---------------------------------------------------------------------------
//  网络
// ---------------------------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   20000     // 单网络连接超时（多网络时每个压缩到 10s）
#define WIFI_RETRY_INTERVAL_MS    10000     // 掉线后重连间隔
#define WIFI_HOSTNAME             "esp32-radio"
#define WIFI_MAX_SAVED            5         // 最多记住的 WiFi 个数（超出挤掉最旧的）
#define WIFI_ROTATE_AFTER         5         // 掉线重连连续失败 N 次后，轮换下一个已存网络

// ---------------------------------------------------------------------------
//  手机配网门户（AP 模式）
// ---------------------------------------------------------------------------
#define AP_SSID_PREFIX            "Radio-"  // 实际热点名 = 前缀 + MAC 后 4 位
#define STA_TEST_TIMEOUT_MS       20000     // 门户里测试 STA 连接的等待时间

// ---------------------------------------------------------------------------
//  局域网网页控制台（STA 模式，与配网门户互斥）
// ---------------------------------------------------------------------------
#define CTRL_PORT                 80

// ---------------------------------------------------------------------------
//  播放
// ---------------------------------------------------------------------------
#define DEFAULT_VOLUME     12       // 0..21
#define MAX_VOLUME         21
#define AUDIO_RETRY_MAX    5        // 连续重连失败上限（成功后会清零重新计）
#define AUDIO_RETRY_DELAY  4000     // 掉流后第 1 次重连前的等待(ms)，之后指数退避
#define AUDIO_STALL_TIMEOUT_MS  6000 // 缓冲区持续为空多久判定"静默断流"并强制重连
#define AUDIO_CLOCK_STALL_MS    6000 // 解码时钟多久不前进判定"解码卡死"并重启音频
                                     // （时钟=库里解出 PCM 的累计秒数，只有真出声音才走）
#define AUDIO_CONNECT_GRACE_MS  8000 // 连上后多久内必须有音频数据，否则视为失败
#define AUDIO_STALL_RECONNECT_MS 8000 // 播到一半僵死后，隔多久重连（别立刻，服务器常拒绝）
// 连续多少次"一个音频字节都没解出来"就判定地址已失效（403/404/下线）。
// 退避 4+8+16+30 秒，约 1 分钟确认一次；判定后自动跳下一台，避免整机永久无声。
#define AUDIO_DEAD_STREAK       4
// 开机不足这么久不执行"自发重启"自愈（防止重启风暴；此时改为判死地址换台）
#define REBOOT_MIN_UPTIME_MS    180000
// 重启风暴保护：NVS 记录连续自发重启次数。一次重启没治好、紧接着又重启到这个次数，
// 说明重启也救不了（整段解码状态反复坏死），本次开机禁用"自发重启"，改走判死地址自动跳台。
#define STORM_REBOOT_LIMIT      2
#define NVS_KEY_STORM      "stormCnt"
#define VOLUME_SAVE_DELAY_MS 2000   // 音量停止变化多久后写入 NVS（连续点击不阻塞）
#define HTTP_TIMEOUT_MS    6000
#define HTTP_TIMEOUT_SSL   9000

//  自定义电台（网页控制端管理，存 LittleFS 文件 /stations.json，NVS 仅作旧数据迁移）
// ---------------------------------------------------------------------------
#define MAX_CUSTOM_STATIONS  200    // 最多可添加的自定义电台数（文件分区 896KB 足够）
#define STA_NAME_MAX         48     // 名称缓冲（字节，约 15 个汉字；超长自动截断不拒收）
#define STA_URL_MAX          184    // 流媒体地址缓冲（字节）
#define STA_PAGE_SIZE        20     // 网页列表每页显示条数（分页浏览）

// ---------------------------------------------------------------------------
//  存储架构（项目规范，新功能一律按此划分，不再改动整体结构）
//
//  【Preferences / NVS】小体量的键值配置——读快、无需挂载、写入原子
//      ns="radio-wifi": WiFi 凭据（最多 5 组，多网络记忆）
//      ns="radio-app" : 音量、上次播放的电台索引、（未来）亮度/主题/等化器等设置项
//      规则：单个键 ≤ 数百字节；只有"标量或短字符串"才进 NVS
//
//  【LittleFS 文件系统】(分区 896KB, 挂载点 /)  大体量 / 变长 / 成列表的数据
//      /stations.json : 自定义电台库（最多 200 个，网页批量导入导出）
//      规则：任何"数量可能增长、需要整体导入导出、或含长文本"的数据都放这里，
//            例如未来的节目单、录音索引、字体/图片资源、设置备份文件
//
//  迁移原则：NVS 里出现过的旧键保留兼容读取逻辑，读到后迁入新家并删除旧键
// ---------------------------------------------------------------------------
#define NVS_NS_WIFI        "radio-wifi"
#define NVS_NS_APP         "radio-app"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"
#define NVS_KEY_STATION    "station"
#define NVS_KEY_VOLUME     "volume"
#define NVS_KEY_CUSTOM     "custom"   // 旧版自定义电台（v0.4.x），仅用于迁移读取

//  LittleFS 存储文件
#define STA_FILE_PATH      "/stations.json"   // 自定义电台库
