// ============================================================================
//  Button.cpp  --  板载 BOOT 键检测实现
// ============================================================================
#include "Button.h"

void Button::begin() {
    // GPIO0 在多数 ESP32-S3 开发板上接 BOOT 键，按下拉低；外部已有上拉，
    // 这里再开内部上拉，防止个别板子省掉外部电阻。
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

    bool low = (digitalRead(PIN_BOOT_BTN) == LOW);
    _raw        = low;
    _stable     = low;
    _pressAt    = millis();
    _lastChange = millis();
    _longFired  = false;

    Serial.printf("[按键] BOOT 键初始化  GPIO%d  %s  (长按 %u 秒进配网)\n",
                  PIN_BOOT_BTN, low ? "当前已按下" : "当前未按下",
                  (unsigned)(BTN_LONGPRESS_MS / 1000));
}

void Button::loop() {
    uint32_t now = millis();

    // 按下 = 低电平（BTN_ACTIVE_LOW 固定为低有效，若你的板子相反请改这里）
    bool raw = (digitalRead(PIN_BOOT_BTN) == LOW);

    if (raw != _raw) {          // 电平有变化，重新计时去抖
        _raw        = raw;
        _lastChange = now;
    }

    if (now - _lastChange < BTN_DEBOUNCE_MS) return;   // 抖动窗口内不认

    if (_raw != _stable) {
        _stable = _raw;
        if (_stable) {
            // ---- 按下 ----
            _pressAt   = now;
            _longFired = false;
        } else {
            // ---- 松开 ----
            uint32_t held = now - _pressAt;
            if (!_longFired && held >= BTN_SHORT_MIN_MS) {
                _shortPending = true;
                _shortCount++;
            }
        }
        return;
    }

    // ---- 按住途中判断长按：达到时长立刻触发，不必等松手 ----
    if (_stable && !_longFired && (now - _pressAt >= BTN_LONGPRESS_MS)) {
        _longFired   = true;
        _longPending = true;
        _longCount++;
    }
}

bool Button::consumeLongPress() {
    if (!_longPending) return false;
    _longPending = false;
    return true;
}

bool Button::consumeShortPress() {
    if (!_shortPending) return false;
    _shortPending = false;
    return true;
}
