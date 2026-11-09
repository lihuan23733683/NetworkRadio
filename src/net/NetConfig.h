// ============================================================================
//  NetConfig.h  --  WiFi 多网络记忆与连接管理
//  支持保存多个 WiFi（最多 WIFI_MAX_SAVED 个）：
//    连接时优先试"最近成功"的那个，失败自动换下一个；
//    运行中掉线先重试首选，连续失败 WIFI_ROTATE_AFTER 次后轮换其他网络
// ============================================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>

#include "../core/AppConfig.h"

class NetConfig {
public:
    // 读取 NVS（含旧版单网络数据自动迁移），返回是否已有可用配置
    bool begin();

    bool   hasCredentials() const { return _cnt > 0; }
    // 新增或更新一个网络（同 SSID 更新密码），并将其设为首选；超出上限挤掉最旧的
    bool   save(const String& ssid, const String& pass);
    // 从列表移除指定 SSID
    bool   del(const String& ssid);
    void   clear();                       // 清除全部

    // 阻塞式连接：按"首选 -> 其他"顺序逐个尝试；成功返回 true
    bool   connect(uint32_t timeoutMs = WIFI_CONNECT_TIMEOUT_MS);

    // 非阻塞发起连接（配网门户里用，连首选网络，保持 AP 开着）
    void   beginConnect();

    // 主循环里调用：掉线自动重连（非阻塞，连续失败自动轮换网络）
    void   loop();

    bool   isConnected() const { return WiFi.status() == WL_CONNECTED; }
    bool   everConnected() const { return _everConnected; }
    void   printStatus() const;
    void   printList() const;             // 打印已存网络列表
    int    scan();   // 扫描并打印周边热点，返回数量

    String ssid() const;                  // 当前连接中 / 首选网络的 SSID
    int    rssi() const { return isConnected() ? WiFi.RSSI() : 0; }
    String ip() const { return isConnected() ? WiFi.localIP().toString() : String("-"); }

private:
    struct Cred { String ssid, pass; };

    Cred     _creds[WIFI_MAX_SAVED];
    uint8_t  _cnt      = 0;    // 已存网络数
    uint8_t  _lastGood = 0;    // 最近成功连接的网络（首选）
    uint8_t  _failRun  = 0;    // loop() 掉线重连连续失败次数（用于轮换）
    uint32_t _lastTry  = 0;
    bool     _everConnected = false;

    void persist();                                  // 全量写回 NVS
    bool tryOne(uint8_t idx, uint32_t timeoutMs);    // 尝试单个网络
};
