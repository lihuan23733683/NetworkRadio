// ============================================================================
//  Knob.h  --  EC11 旋转编码器（20 脉冲/20 定位）+ 旋钮按压 + 独立按键 K0
//  旋转：引脚中断 + 状态表解码，不占主循环；4 个中断步进 = 1 个定位格
//  按键：轮询去抖 + 短按/长按（长按按住途中触发，与 BOOT 键行为一致）
// ============================================================================
#pragma once
#include <Arduino.h>

#include "../core/AppConfig.h"

class Knob {
public:
    void begin();
    void loop();

    // 事件取用（取出后清零，只触发一次）
    int8_t consumeRotation();        // 本次累计的定位格数（+顺时针 / -逆时针）
    bool   consumePushShort();       // 旋钮按下后松开（短按）
    bool   consumePushLong();        // 旋钮按住达到时长（长按，不必松手）
    bool   consumeKeyShort();        // 独立按键短按
    bool   consumeKeyLong();         // 独立按键长按

    bool   pushed() const { return _push.stable; }   // 旋钮当前是否被按住（按住旋转=调音量）

    // 调试
    uint32_t rotCount() const { return _rotTotal; }   // 累计定位格数
    uint32_t rawEdges() const { return _rawTotal; }   // 旋转中断总边沿数（=0 说明 A/B 线没接通）
    uint16_t pushCount() const { return _pushShort; }
    uint16_t keyCount() const { return _keyShort; }

private:
    struct Key {                     // 通用去抖按键（低电平按下）
        int16_t  pin = -1;
        bool     raw = false, stable = false;
        bool     longFired = false, shortPend = false, longPend = false;
        uint32_t lastChange = 0, pressAt = 0;
        uint16_t shortN = 0, longN = 0;
        void begin(int16_t p, uint32_t now);
        void loop(uint32_t now);
    };

    static void IRAM_ATTR isrEnc();  // 旋转中断（A/B 两脚共用一个 ISR）

    volatile int16_t _steps = 0;     // 中断累计的中断步进（4 步 = 1 格）
    volatile uint32_t _rawTotal = 0; // 中断边沿总数（诊断接线用）
    uint32_t _rotTotal  = 0;         // 累计定位格数（调试）
    uint16_t _pushShort = 0;
    uint16_t _keyShort  = 0;

    Key _push;                       // 旋钮按压
    Key _key;                        // 独立按键 K0（板上丝印 KO）
};
