// ============================================================================
//  Control.h  --  局域网网页控制台（EC11 到货前的换台 / 音量方案）
//  手机或电脑连同一 WiFi，浏览器访问 http://<设备IP>/ 即可：
//    换台 / 上一台 / 下一台 / 音量 +/- / 静音 / 暂停继续 / 查看状态
//  与配网门户(Portal)互斥：配网模式开启时本模块自动挂起
// ============================================================================
#pragma once
#include <Arduino.h>

#include "../net/NetConfig.h"

class Control {
public:
    void begin(NetConfig* net);   // 注册路由；服务器在首次联网成功后自动启动
    void loop();                  // 主循环轮询；配网模式下不工作
    void pauseServer();           // 释放 80 端口（进配网门户前调用，避免端口冲突）

private:
    void handleRoot();
    void handleStatus();
    void handlePlay();
    void handleStep(int dir);
    void handleVol();
    void handleMutePause();
    void handleStaAdd();     // 网页端添加自定义电台
    void handleStaEdit();    // 网页端编辑自定义电台
    void handleStaDel();     // 网页端删除自定义电台
    void handleList();       // 全量电台列表（分页 UI 用，仅打开/变更时请求）
    void handleImport();     // 批量导入（POST 文本：名称,URL 或 M3U）
    void handleExport();     // 导出全部自定义电台为 M3U 文本
    void handleDiag();       // 音频诊断（缓冲/解码/VU/重连次数），排查无声问题用
    void sendOk();
    void sendErr(const String& msg);
    void announce();

    NetConfig* _net        = nullptr;
    bool       _routeDone  = false;
    bool       _begun      = false;
    bool       _announced  = false;
};
