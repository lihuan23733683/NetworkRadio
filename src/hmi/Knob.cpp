// ============================================================================
//  Knob.cpp  --  EC11 旋转编码器 + 按键实现
//  旋转解码：中断里按格雷码状态表累计 ±1 步，主循环里除以 4 得到定位格。
//  机械抖动大多落在"非法跳变"上，状态表自动丢弃，实测无需额外 RC 滤波。
//  如果旋转方向与预期相反：把 PIN_ENC_A / PIN_ENC_B 的接线（或定义）对调。
// ============================================================================
#include "Knob.h"

#include <driver/gpio.h>    // ISR 里用 gpio_get_level（寄存器直读，NVS 写入期间安全）

// 格雷码状态表：索引 = 上次AB(2bit)<<2 | 当前AB(2bit)，AB = A*2+B
// 序列 00->10->11->01->00 为 +1（顺时针），非法跳变（两位同变）记 0
static const int8_t kQuadTbl[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

Knob* g_knob = nullptr;   // ISR 需要访问实例成员，注册中断时保存 this 指针

void Knob::begin() {
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    _push.begin(PIN_ENC_PUSH, millis());
    _key.begin(PIN_KEY_K0, millis());

    g_knob = this;
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), isrEnc, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), isrEnc, CHANGE);

    Serial.printf("[旋钮] EC11 A=%d B=%d PUSH=%d  独立按键 K0=%d\n",
                  PIN_ENC_A, PIN_ENC_B, PIN_ENC_PUSH, PIN_KEY_K0);
}

void IRAM_ATTR Knob::isrEnc() {
    if (!g_knob) return;
    g_knob->_rawTotal++;
    static uint8_t prev = 0;
    // 必须用 gpio_get_level：digitalRead 不在 IRAM，NVS 写入期间被调用会崩溃
    uint8_t cur = (uint8_t)(((uint8_t)gpio_get_level((gpio_num_t)PIN_ENC_A) << 1)
                            | (uint8_t)gpio_get_level((gpio_num_t)PIN_ENC_B));
    uint8_t idx = (uint8_t)((prev << 2) | cur);
    prev = cur;
    int8_t d = kQuadTbl[idx];
    if (d) {
        int16_t s = g_knob->_steps + d;
        if (s > 800)  s = 800;      // 防溢出：转太快也封顶
        if (s < -800) s = -800;
        g_knob->_steps = s;
    }
}

int8_t Knob::consumeRotation() {
    int16_t s;
    noInterrupts();
    s = _steps;
    _steps = 0;
    interrupts();
    int d = s / ENC_DETENT_STEPS;
    int16_t rem = (int16_t)(s - d * ENC_DETENT_STEPS);   // 保留余数，不丢慢转的步进
    if (rem) {
        noInterrupts();
        _steps = rem;
        interrupts();
    }
    if (d != 0) _rotTotal += (d < 0 ? -d : d);
    return (int8_t)d;
}

// ---------------------------------------------------------------------------
//  通用去抖按键
// ---------------------------------------------------------------------------
void Knob::Key::begin(int16_t p, uint32_t now) {
    pin        = p;
    raw        = (digitalRead(p) == LOW);
    stable     = raw;
    pressAt    = now;
    lastChange = now;
    pinMode(p, INPUT_PULLUP);
}

void Knob::Key::loop(uint32_t now) {
    bool r = (digitalRead(pin) == LOW);
    if (r != raw) { raw = r; lastChange = now; }
    if (now - lastChange < KNOB_DEBOUNCE_MS) return;

    if (raw != stable) {
        stable = raw;
        if (stable) {
            pressAt   = now;
            longFired = false;
        } else {
            // 松开：没触发过长按且时长够 -> 短按
            if (!longFired && now - pressAt >= 60) {
                shortPend = true;
                shortN++;
            }
        }
        return;
    }
    // 按住途中：达到长按阈值立刻触发（不必等松手）
    if (stable && !longFired && now - pressAt >= KNOB_LONGPRESS_MS) {
        longFired = true;
        longPend  = true;
        longN++;
    }
}

void Knob::loop() {
    uint32_t now = millis();
    _push.loop(now);
    _key.loop(now);
}

bool Knob::consumePushShort() { if (_push.shortPend) { _push.shortPend = false; return true; } return false; }
bool Knob::consumePushLong()  { if (_push.longPend)  { _push.longPend  = false; return true; } return false; }
bool Knob::consumeKeyShort()  { if (_key.shortPend)  { _key.shortPend  = false; _keyShort++; return true; } return false; }
bool Knob::consumeKeyLong()   { if (_key.longPend)   { _key.longPend   = false; return true; } return false; }
