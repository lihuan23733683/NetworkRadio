// ============================================================================
//  EventLog.h  --  轻量事件环形缓冲（诊断用，header-only）
//
//  为什么需要它：断音类故障是间歇性的，串口又要占着 COM 口、还可能因 DTR
//  复位芯片破坏现场。把关键事件（断流/停滞/重连/换台/复位原因）记在 RAM 里，
//  网页 GET /api/events 就能事后取证，完全不依赖串口。
//
//  为什么是 header-only：ESP32 的 arduino-cli 在 libsdetect（g++ -E）阶段
//  遇到新增 .cpp 会随机卡死几十分钟，为此新增一个翻译单元不值得。
//  单例用"内联函数里的函数局部 static"实现，多个 .cpp 包含也是同一份数据。
//
//  用法：EvLog::add("解码停滞 缓冲 %lu B", fill);
//        串口 /api/events 返回 ["12.3 | 开机，复位原因: 上电", ...]
// ============================================================================
#pragma once
#include <Arduino.h>
#include <esp_system.h>
#include <stdarg.h>

namespace EvLog {

constexpr uint8_t SLOTS = 24;   // 最多保留的事件条数（够覆盖一次故障前后）
constexpr uint8_t LEN   = 96;   // 单条最长字符数

struct Store {
    char         ring[SLOTS][LEN];
    uint8_t      head  = 0;     // 下一条写入位置
    uint8_t      count = 0;     // 已存条数
    portMUX_TYPE mux   = portMUX_INITIALIZER_UNLOCKED;
};

// 函数局部 static：C++11 保证只初始化一次且全程序共享
inline Store& store() {
    static Store s;
    return s;
}

// 追加一条事件（自动加 "开机秒数.x | " 前缀）
inline void add(const char* fmt, ...) {
    if (fmt == nullptr) return;
    Store& st = store();

    // 先在栈上格式化：临界区里不能做 String / 堆分配
    char     line[LEN];
    uint32_t ms = millis();
    int      n  = snprintf(line, sizeof(line), "%lu.%01lu | ", (unsigned long)(ms / 1000),
                           (unsigned long)((ms % 1000) / 100));
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;   // 超长就截断，别丢事件

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&st.mux);
    memcpy(st.ring[st.head], line, LEN);
    st.ring[st.head][LEN - 1] = '\0';
    st.head = (uint8_t)((st.head + 1) % SLOTS);
    if (st.count < SLOTS) st.count++;
    portEXIT_CRITICAL(&st.mux);
}

// 最近事件，JSON 数组字符串，按发生时间从早到晚
inline String toJson() {
    Store&  st = store();
    char    snap[SLOTS][LEN];
    uint8_t count, head;

    portENTER_CRITICAL(&st.mux);
    memcpy(snap, st.ring, sizeof(snap));
    count = st.count;
    head  = st.head;
    portEXIT_CRITICAL(&st.mux);

    String s = "[";
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t idx = (uint8_t)((head + SLOTS - count + i) % SLOTS);   // 最旧的一条开始
        if (i) s += ',';
        s += '"';
        for (const char* p = snap[idx]; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                s += '\\';
                s += (char)c;
            } else if (c < 0x20) {
                s += ' ';
            } else {
                s += (char)c;   // UTF-8 中文原样输出，JSON 允许
            }
        }
        s += '"';
    }
    s += ']';
    return s;
}

inline void clear() {
    Store& st = store();
    portENTER_CRITICAL(&st.mux);
    st.head  = 0;
    st.count = 0;
    portEXIT_CRITICAL(&st.mux);
}

// 本次开机的复位原因（上电/崩溃/看门狗/供电不足…），用于判断"是不是它自己重启了"
inline const char* resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "上电";
        case ESP_RST_EXT:       return "外部复位脚";
        case ESP_RST_SW:        return "软件重启";
        case ESP_RST_PANIC:     return "崩溃(PANIC)";
        case ESP_RST_INT_WDT:   return "中断看门狗(INT_WDT)";
        case ESP_RST_TASK_WDT:  return "任务看门狗(TASK_WDT)";
        case ESP_RST_WDT:       return "其它看门狗";
        case ESP_RST_BROWNOUT:  return "供电不足(BROWNOUT)";
        case ESP_RST_DEEPSLEEP: return "深睡唤醒";
        default:                return "未知";
    }
}

}   // namespace EvLog
