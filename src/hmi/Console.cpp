// ============================================================================
//  Console.cpp
// ============================================================================
#include "Console.h"

#include "../core/AppConfig.h"

void Console::begin() {
    Serial.begin(SERIAL_BAUD);
    Serial.setTimeout(50);
}

void Console::loop() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (_buf.length() > 0) {
                _line  = _buf;
                _line.trim();
                _buf   = "";
                if (_line.length() > 0) _ready = true;
                Serial.println();
            }
            continue;
        }

        if (c == 8 || c == 127) {  // 退格
            if (_buf.length() > 0) {
                _buf.remove(_buf.length() - 1);
                Serial.print("\b \b");
            }
            continue;
        }

        if (_buf.length() < CONSOLE_MAX_LINE) {
            _buf += (char)c;
            Serial.print((char)c);  // 回显
        }
    }
}

bool Console::available(String& line) {
    if (!_ready) return false;
    line   = _line;
    _ready = false;
    return true;
}

void Console::prompt() {
    Serial.print("\nradio> ");
    Serial.flush();
}

void Console::banner() {
    Serial.println();
    Serial.println("==============================================================");
    Serial.printf("  %s  v%s   ESP32-S3 + MAX98357A  网络收音机（控制台调试版）\n", FW_NAME, FW_VERSION);
    Serial.println("==============================================================");
    Serial.printf("  I2S : BCLK=GPIO%d  LRC=GPIO%d  DIN=GPIO%d\n", PIN_I2S_BCLK, PIN_I2S_LRC,
                  PIN_I2S_DOUT);
    Serial.printf("  串口: %d 8N1    输入 help 查看命令\n", SERIAL_BAUD);
    Serial.println("==============================================================");
}
