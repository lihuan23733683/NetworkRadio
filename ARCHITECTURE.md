# NetworkRadio 架构说明

> ESP32-S3 + MAX98357A 网络收音机 · v0.7.2
> 本文档描述代码分层与模块职责。新增功能时，先在本文找到归属层，再动手。

## 目录结构

```
NetworkRadio/
├── NetworkRadio.ino            # 入口：初始化编排 + 主循环 + 串口命令分发
├── ARCHITECTURE.md             # 本文档
├── README.md                   # 项目主页（GitHub）
├── docs/
│   ├── HARDWARE.md             # 硬件清单、接线表、避坑指南
│   └── USAGE.md                # 功能与使用手册
└── src/
    ├── core/
    │   └── AppConfig.h         # 全局配置：引脚、常量、存储规范、命名空间
    ├── net/                    # ── 网络层：一切与 WiFi 连接性相关
    │   ├── NetConfig.h/.cpp    #   WiFi 多网络记忆（≤5组）、自动切换、状态查询
    │   └── Portal.h/.cpp       #   AP 配网门户（captive portal，DNS 捕获）
    ├── audio/                  # ── 音频层：一切与"播出声音"相关
    │   ├── Player.h/.cpp       #   播放引擎：连接、断流看门狗、指数退避重连、音量
    │   ├── Stations.h/.cpp     #   电台库：内置 6 台(只读) + 自定义≤200(LittleFS)
    │   └── Tone.h/.cpp         #   I2S 硬件自检（绕开解码库直接推正弦波）
    └── hmi/                    # ── 人机交互层：一切与"用户操作"相关
        ├── Display.h/.cpp      #   彩屏 UI（ST7789 自适应布局、配网页/信息页/走马灯）
        ├── Knob.h/.cpp         #   EC11 旋钮（中断计数、按压长短按）
        ├── Button.h/.cpp       #   BOOT 按键状态机（短按换台/长按配网，非阻塞）
        ├── Control.h/.cpp      #   网页控制台：播放/音量/电台管理/导入导出/分页搜索
        └── Console.h/.cpp      #   串口命令台（调试用，与网页功能互补）
```

## 分层规则（新增功能必读）

1. **依赖方向单向**：`hmi → audio → net → core`，禁止反向引用（例如
   Player 不允许 include Control）。core 被所有人依赖，自身不依赖任何人。
2. **模块间通信**：下层不主动调上层。音频状态变化通过 `Player::state()` 查询、
   事件通过 `.ino` 主循环轮询分发，不搞回调穿透。
3. **配置集中**：所有魔法数字（引脚、超时、上限）一律进 `core/AppConfig.h`，
   模块内不写死可调参数。
4. **存储分工**（用户 2026-11-03 拍板的规范，详见 AppConfig.h 注释块）：
   - 小键值配置 → NVS/Preferences（WiFi、音量、上次电台、未来设置项）
   - 大体量/变长/成列表数据 → LittleFS（`/stations.json` 电台库、未来的节目单/录音）
5. **新增模块放哪**：
   - 屏幕/OLED 显示 → `src/hmi/`（新建 Display.h/.cpp，只读其他层状态）
   - EC11 旋钮 → `src/hmi/`（新建 Knob.h/.cpp，产出的操作调 Player/Control 的公开接口）
   - 定时播放/闹钟 → `src/core/`（纯逻辑）+ `.ino` 编排
   - 录音功能 → `src/audio/`（新模块），数据文件进 LittleFS

## 主程序流程（NetworkRadio.ino）

```
setup:  Console → NetConfig(载入多WiFi) → Stations(挂载LittleFS+迁移) →
        Player → Control(注册路由) → Button
        └─ 有已存WiFi? ──是→ 连接(失败快速轮换下一个网络) → playStation(上次电台)
                        └─否→ enterPortal()（AP 热点 + 配网页）

loop:   Console.loop()     串口命令
        net.loop()         掉线重连 / 多网络轮换
        portal.loop()      配网页处理（仅配网模式）
        control.loop()     网页请求处理（仅联网模式）
        player.loop()      解码泵 + 断流看门狗
        button.poll()      按键事件
```

## 关键接口速查

| 需求 | 接口 |
|---|---|
| 播放指定台 | `playStation(idx)`（.ino，全局） |
| 音量 | `player.setVolume(v)` / `player.stepVolume(±1)` |
| 暂停/恢复 | `player.togglePause()`（直播流：恢复=重连） |
| 电台库 | `Stations::count()/get(i)/addCustom()/editCustom()/delCustom()/importText()/exportM3U()` |
| WiFi | `net.add(ssid,pass)/remove(ssid)/list()/status()` |

## 构建要点（详见 MEMORY.md）

- arduino-cli + `-j 2`；FQBN 必须 `PSRAM=opi` + `huge_app` + `EraseFlash=none`
- ESP32-audioI2S **固定 3.4.7**，本地补丁：HLS `no_host_timer` 10s→1s（源码搜 "PATCH"）
- include 约定：同目录直接 `#include "Xxx.h"`；跨目录 `#include "../net/NetConfig.h"`
  （arduino-cli 不把工程根目录加入头文件搜索路径）
- 所有库头文件必须在 .ino 顶部先 include（库依赖检测需要）
