## 直接下载烧录，5 分钟听上电台

本页附件是 **v0.7.5 完整固件**，不需要安装 Arduino IDE 或编译，下载后用 esptool 一条命令烧进 ESP32-S3 即可。

### 下载哪个文件？

| 文件 | 大小 | 烧录地址 | 适用场景 |
|---|---|---|---|
| `NetworkRadio-v0.7.5-ESP32S3-full.bin` | 4.0 MB | **0x0** | **推荐**。新板、救砖、从零开始，含引导程序 + 分区表 + 应用 |
| `NetworkRadio-v0.7.5-app-only.bin` | 2.9 MB | **0x10000** | 已烧过本固件的板子升级，保留 WiFi 与电台设置 |

> ⚠️ 注意：`full.bin` 会整片覆盖 Flash，**WiFi 凭据和自定义电台库会被清空**，需要重新配网。

### 硬件要求

- 主控：**ESP32-S3**（4 MB Flash + 8 MB OPI PSRAM，即 N4R8 模组）
- 音频：MAX98357A（I2S：BCLK=16、LRC=17、DIN=18）
- 屏幕：2.4" ST7789 320×240（SCLK=47、MOSI=21、CS=42、DC=41、RST=40、BL=9）
- 旋钮：EC11（A=5、B=4、PUSH=6），独立按键 K0=7
- 详细接线见仓库 [docs/HARDWARE.md](https://github.com/lihuan23733683/NetworkRadio/blob/main/docs/HARDWARE.md)

### 烧录方法 A：esptool 命令（推荐）

```bash
pip install esptool

# 整片烧录（新板）
esptool.py --chip esp32s3 -p COM5 -b 921600 write_flash -z 0x0 NetworkRadio-v0.7.5-ESP32S3-full.bin

# 仅升级应用（保留设置）
esptool.py --chip esp32s3 -p COM5 -b 921600 write_flash -z 0x10000 NetworkRadio-v0.7.5-app-only.bin
```

- `-p COM5` 换成你电脑上的实际串口（Windows 设备管理器可见 CH340）
- 若连接失败，先按住 BOOT 再按 RST 进入下载模式，或把波特率降到 `115200`
- macOS / Linux 串口形如 `/dev/cu.usbserial-xxxx`、`/dev/ttyUSB0`

### 烧录方法 B：Flash Download Tool（图形界面）

1. 下载乐鑫官方 [Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
2. ChipType 选 **ESP32-S3**，WorkMode 选 **Develop**，LoadMode 选 **UART**
3. 添加 `NetworkRadio-v0.7.5-ESP32S3-full.bin`，地址填 **0x0**
4. 波特率 `921600`，选好串口，点 **START**

### 烧完怎么用

1. **上电**：屏幕显示开机动画，若无 WiFi 记录会自动进入配网模式
2. **配网**：手机搜热点 **`Radio-XXXX`**（无密码，XXXX 是 MAC 后四位）→ 连接后浏览器打开 **192.168.4.1** → 选你家 WiFi、输密码 → 保存
3. **播放**：设备自动重启并开始播放上次的电台
4. **控制台**：同一局域网内浏览器访问 **http://radio.local**（或设备 IP），可换台、调音量、增删电台、批量导入
5. **操作**：旋转旋钮换台 / 短按暂停 / 长按 5 秒重新配网 / 短按 K0 切换信息页

### 附件校验

```
0fce4227b3e05343bc57aa4157c5c1f29218e59ca017ba1a11dc4a4ccb5c5d68  NetworkRadio-v0.7.5-ESP32S3-full.bin
e37c39f729edd4a07505f5501881d03cde82b94de813ca40d81698df086988ef  NetworkRadio-v0.7.5-app-only.bin
```

### v0.7.5 版本要点

- 网页控制台：播放/暂停、音量、换台、电台增删改查、批量导入导出、分页搜索
- 多 WiFi 记忆（最多 5 组自动择优切换）
- 断流自愈：解码时钟看门狗 + 指数退避重连 + 死地址自动跳台
- 2.4" 彩屏 UI：主页面 / 信息页滚动、配网指导页、控制台地址走马灯
- 自适应多分辨率布局，EC11 旋钮 250 ms 确认防抖
- 熄屏节电：默认 10 秒无操作熄屏，旋钮/按键即亮
- mDNS：`radio.local` 直达控制台

---

问题反馈请提 [Issue](https://github.com/lihuan23733683/NetworkRadio/issues)。源码与完整文档见 [仓库主页](https://github.com/lihuan23733683/NetworkRadio)。
