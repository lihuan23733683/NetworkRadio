// ============================================================================
//  Portal.h  --  手机配网门户
//  未配置 WiFi（或连接失败）时开放 AP 热点 + 网页配置界面
//  手机连上热点后浏览器访问 http://192.168.4.1/ 即可配置
// ============================================================================
#pragma once
#include <Arduino.h>

class NetConfig;

class Portal {
public:
    void begin(NetConfig* net);   // 开启热点 + 网页服务
    void loop();                  // 处理 HTTP / DNS / 连接状态机
    void stop();                  // 关闭热点与网页服务

    bool   active() const { return _active; }
    String apSsid() const { return _apSsid; }
    String apIp() const;

private:
    enum State : uint8_t { PS_IDLE, PS_CONNECTING, PS_OK, PS_FAIL };

    void setupRoutes();
    void handleRoot();
    void handleScan();
    void handleSave();
    void handleStatus();
    void handleNotFound();

    NetConfig* _net   = nullptr;
    String     _apSsid;
    String     _msg;
    bool       _active = false;
    State      _state  = PS_IDLE;
    uint32_t   _staStart = 0;
    uint32_t   _okAt     = 0;
};
