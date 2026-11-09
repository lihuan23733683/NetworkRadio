# 硬件与接线说明书

> NetworkRadio v0.7.2 · ESP32-S3 网络收音机
> 所有引脚定义集中在 `src/core/AppConfig.h`，改接线只需改这一个文件。

## 一、硬件清单

| 模块 | 规格 | 数量 | 说明 |
|---|---|---|---|
| 主控板 | ESP32-S3 Dev Module（4MB Flash + OPI PSRAM） | 1 | 推荐 N16R8；PSRAM 用于流缓冲 |
| 功放 | MAX98357A I2S 数字功放板 | 1 | 3W Class-D，BTL 输出直推喇叭 |
| 喇叭 | 4Ω 或 8Ω 小喇叭，3W 左右 | 1 | 两根线直接接功放 +/-，不分正负极性 |
| 屏幕 | 2.4" TFT，ST7789V，SPI 接口，240×320 | 1 | 带背光控制脚（BLK）的模块 |
| 旋钮 | EC11 旋转编码器（带按压开关） | 1 | 20 脉冲/20 定位 |
| 按键 | 独立轻触按键（K0） | 1 | |
| 杜邦线 | 若干 | | 信号线建议短线直连，减少虚接 |

## 二、完整接线表

### 1. MAX98357A 功放

| 功放引脚 | 接 ESP32 | 线色（参考） | 说明 |
|---|---|---|---|
| **VIN** | 5V (VIN) | 红 | 3.3V 也能响，但 5V 音量大、失真小 |
| **GND** | GND | 黑 | 必须与 ESP32 共地 |
| **BCLK** | GPIO16 | 黑/棕 | 位时钟 |
| **LRC** | GPIO17 | 白 | 左右声道时钟 (WS) |
| **DIN** | GPIO18 | 灰 | 数据 |
| **SD** | **3V3**（拉高！） | | ⚠️ **悬空/接地 = 芯片关断，完全无声**。这是最常见的"不响"原因。>1.4V 输出左声道 |
| **GAIN** | 悬空 | | 悬空=9dB；接 VDD=15dB；接 GND=12dB；100kΩ 到 GND=3dB |

> 💡 喇叭直接接功放输出 + / − 两个焊点，**不要**把喇叭一端接 3.3V 或 GND（输出是 BTL 桥接）。

### 2. 2.4" ST7789 屏幕（SPI）

| 屏幕丝印 | 接 ESP32 | 说明 |
|---|---|---|
| **VCC** | **5V** | ⚠️⚠️ **必须接 5V！** 模块丝印虽写 "VCC: 3.3V/5V"，但实测 3.3V 时逻辑电压不足 → **背光亮但永久白屏**。这是本项目白屏排查数日的真凶 |
| **GND** | GND | |
| **SCL** | GPIO47 | SPI 时钟 |
| **SDA** | GPIO21 | SPI 数据 (MOSI) |
| **CS** | GPIO42 | 片选 |
| **DC** | GPIO41 | 数据/命令 |
| **RES** | GPIO40 | 复位 |
| **BLK** | GPIO9 | 背光（PWM 调光） |

**屏幕避坑三条血泪经验：**

1. **VCC 接 5V**——"3.3V/5V 兼容"的模块接 3.3V 可能出现"有背光但永不显示"，且软件层完全无感（ST7789 无读回线，驱动报告 init 成功≠面板收到）；
2. **避开板载外设脚**——ESP32-S3 板载 RGB 灯（WS2812）在 **GPIO48**（部分旧版 38），别把 SDA 等信号接上去，否则灯乱闪且屏不亮。本表选 47/21/42/41/40 就是为此；
3. **接触优先直连**——杜邦线经面包板转接易虚接，白屏排查时优先换线/直连，再用固件自带的 `tft scan` / `tft pins` / `tft set` 诊断命令（见 USAGE.md）。

**换屏**：`AppConfig.h` 中 `TFT_DRIVER` 改 `1`（ILI9341）或 `2`（ST7796 320×480 大屏），UI 布局按分辨率自动适配。SPI 频率 `TFT_SPI_FREQ` 默认 16MHz，接触不稳可再降。

### 3. EC11 旋钮 + K0 按键

| 元件引脚 | 接 ESP32 | 说明 |
|---|---|---|
| 旋钮 A | GPIO5 | 相位 A，方向反了与 B 对调 |
| 旋钮 B | GPIO4 | 相位 B |
| 旋钮 PUSH | GPIO6 | 按压开关（固件内部上拉） |
| K0 | GPIO7 | 独立按键（固件内部上拉） |
| 公共端 | GND | 旋钮 A/B/PUSH 和 K0 的另一端全部接 GND |

### 4. 板载资源

| 项 | 引脚/说明 |
|---|---|
| BOOT 按键 | GPIO0（多数 ESP32-S3 板）：短按=下一台，长按 5 秒=进/出配网 |
| 板载 RGB 灯 | GPIO48（旧版 38）：固件启动时自动关灭 |

## 三、供电

- **USB 供电即可**（5V/500mA 以上）。屏幕背光 + 功放同时工作电流约 200–300mA；
- 5V 需要多路分配时，建议经面包板**电源轨**（注意电源轨中间可能有断点，跨断点不通）；
- 所有模块必须与 ESP32 **共地**。

## 四、IDE 编译选项

Arduino IDE（1.8 工具菜单 / 2.x Tools 菜单）按如下设置：

| 菜单项 | 选择 |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| PSRAM | **OPI PSRAM** |
| Flash Mode | QIO 80MHz |
| USB CDC On Boot | **Enabled** |
| Erase All Flash Before Sketch Upload | **Disabled** ⚠️ 选了会清掉已存 WiFi 和电台库 |
| Upload Speed | 921600 |

**依赖库**（放 sketchbook 的 `libraries/` 目录）：

| 库 | 版本 | 说明 |
|---|---|---|
| [ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) | **3.4.7** 固定 | 4.x API 不兼容且 HLS 断流 bug 未修 |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | 最新 | 屏幕驱动 |
| LittleFS | 随 ESP32 核心 | ESP32 核心 3.x 自带 |
| ESP32 核心包 | 3.3.x | |

> 注意：本工程所有库均在 `NetworkRadio.ino` 顶部先 include（否则 arduino-cli/IDE 的库依赖检测可能卡死）。

## 五、串口调试

- 波特率 **115200**，USB CDC；
- ⚠️ 打开串口监视器会让板子重启一次，启动后约 12 秒内不响应命令属正常；
- 命令列表见 [USAGE.md · 串口命令](USAGE.md#五串口命令)。
