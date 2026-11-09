// ============================================================================
//  Button.h  --  板载 BOOT 键（GPIO0）非阻塞按键检测
//  功能：去抖 + 短按 + 长按（长按在按住途中触发一次，不用松手）
//  只读 GPIO，不占用任何定时器，调用 loop() 即可
// ============================================================================
#pragma once
#include <Arduino.h>

#include "../core/AppConfig.h"

class Button {
public:
    void begin();
    void loop();          // 每轮主循环调用，内部 10ms 采样 + 去抖

    // 事件取用（取出后自动清零，保证只触发一次）
    bool consumeLongPress();
    bool consumeShortPress();

    bool     isPressed() const { return _stable; }
    uint32_t heldMs() const { return _stable ? (millis() - _pressAt) : 0; }
    uint32_t longPressMs() const { return (uint32_t)BTN_LONGPRESS_MS; }

    // 调试用计数
    uint16_t longCount() const { return _longCount; }
    uint16_t shortCount() const { return _shortCount; }

private:
    bool     _raw         = false;   // 最近一次采样电平（按下=true）
    bool     _stable      = false;   // 去抖后的稳定状态
    bool     _longFired   = false;   // 本次按下是否已经触发过长按
    bool     _longPending = false;
    bool     _shortPending = false;
    uint32_t _lastChange  = 0;       // 电平最后一次变化的时间
    uint32_t _pressAt     = 0;       // 稳定按下时刻
    uint16_t _longCount   = 0;
    uint16_t _shortCount  = 0;
};
