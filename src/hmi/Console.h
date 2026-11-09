// ============================================================================
//  Console.h  --  串口控制台（行输入）
//  调试阶段靠它替代屏幕和旋钮
// ============================================================================
#pragma once
#include <Arduino.h>

class Console {
public:
    void begin();
    void loop();                    // 收字节、拼行
    bool available(String& line);   // 取一整行（非阻塞）
    void prompt();                  // 打印提示符
    void banner();                  // 开机横幅

private:
    String _buf;
    String _line;
    bool   _ready = false;
};
