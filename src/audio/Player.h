// ============================================================================
//  Player.h  --  I2S 播放器封装（ESP32-audioI2S）
//  负责：连接网络流 / 播放 / 音量 / 掉流自动重连 / 事件日志
// ============================================================================
#pragma once
#include <Arduino.h>

#include <Audio.h>

#include "../core/AppConfig.h"

class Player {
public:
    enum State : uint8_t {
        ST_IDLE = 0,   // 空闲，没在放
        ST_CONNECTING, // 已发起连接
        ST_PLAYING,    // 正在播
        ST_PAUSED,     // 暂停
        ST_LOST,       // 流断了，等待重连
        ST_STOPPED,    // 手动停止 / 放弃重连
    };

    bool begin();
    void loop();

    bool playUrl(const String& url, const String& label);
    void stop();
    bool togglePause();

    void setVolume(uint8_t vol);
    void volumeDelta(int d);
    void setMute(bool m);
    bool muted() const { return _muted; }
    uint8_t volume() const { return _volume; }

    void printInfo();
    void setVerbose(bool v) { _verbose = v; }
    bool verbose() const { return _verbose; }

    State         state() const { return _state; }
    const String& url() const { return _url; }
    const String& label() const { return _label; }
    static const char* stateName(State s);

    // ---- 远程诊断（网页 /api/diag 使用，不打印串口，避免干扰解码）----
    // 注意：Audio 库的 getter 不是 const 方法，这里只能用非 const 成员
    uint32_t inFilled()     { return (uint32_t)_audio.inBufferFilled(); }
    uint32_t inSize()       { return (uint32_t)_audio.getInBufferSize(); }
    uint32_t bitRate()      { return (uint32_t)_audio.getBitRate(); }
    uint32_t sampleRate()   { return (uint32_t)_audio.getSampleRate(); }
    uint8_t  bitsPerSample(){ return _audio.getBitsPerSample(); }
    uint8_t  channels()     { return _audio.getChannels(); }
    uint16_t vuLevel()      { return _audio.getVUlevel(); }
    uint32_t audioClock()   { return _audio.getAudioCurrentTime(); }
    bool     libRunning()   { return _audio.isRunning(); }
    uint8_t  retries() const { return _retries; }
    uint32_t playingForMs() const { return _playSince ? (millis() - _playSince) : 0; }
    uint32_t stallCount() const { return _stallCount; }
    uint8_t  failStreak() const { return _failStreak; }
    const String& lastError() const { return _err; }

    // 连续多次连不上（地址失效）时的回调：让上层自动跳到下一台，
    // 避免整机守着一个死地址永久无声。由 .ino 设置，回调里应调用 playStation()。
    void setDeadHandler(void (*h)()) { _onDead = h; }

private:
    void onEvent(const Audio::msg_t& m);
    void handleEof(const char* who);
    uint32_t retryDelayMs() const;   // 重连退避：4/8/16/30/60 秒
    void fail(const char* reason);   // 统一失败处理：关连接 + 计连击 + 退避重试
    void rebootSelf(const char* why); // 库状态彻底僵死时的最后手段（重启清除）
    void tickConnecting();
    void tickPlaying();
    void tickLost();

    Audio            _audio;
    volatile State   _state = ST_IDLE;
    String           _url, _label;
    uint8_t          _volume  = DEFAULT_VOLUME;
    bool             _muted   = false;
    bool             _paused  = false;
    bool             _verbose = false;   // 详细日志默认关：日志在音频任务里打印，
                                         // 串口缓冲满会阻塞解码导致断音，串口 "log on" 临时开
    bool             _volDirty   = false;
    uint32_t         _volDirtyAt = 0;
    uint32_t         _emptySince = 0;   // 静默断流看门狗：缓冲区开始为空的时刻
    uint32_t         _connDeadline = 0; // 重连后等待首包数据的截止时刻
    uint32_t         _playSince = 0;    // 本次连续正常播放的起点（用于重置重连计数）
    uint32_t         _lastFill = 0xFFFFFFFF;  // 上次观察到的缓冲填充量（-1=未初始化）
    uint32_t         _lastClock = 0xFFFFFFFF; // 上次看到的解码时钟（秒），-1=未建立基线
    uint32_t         _clockAt   = 0;          // 解码时钟最后一次前进的时刻
    uint32_t         _stallCount = 0;         // 累计触发的"停滞"次数（诊断用）
    uint32_t         _lastStallAt = 0;        // 最近一次停滞时刻（判断是否连续发作）
    uint8_t          _stallBurst = 0;         // 短时间内的停滞连击次数（用于加硬核恢复）
    uint8_t          _retries = 0;
    uint32_t         _lastConnectAt = 0;
    uint32_t         _lostAt        = 0;
    uint32_t         _retryAt       = 0;   // 下次重连的时刻（显式时间点比 lostAt+delay 清楚）
    uint32_t         _lastAudioAt   = 0;   // 最近一次"确实解出声音"的时刻（判断是否临时断流）
    // ---- v0.5.2：死地址保护 ----
    uint8_t          _failStreak = 0;   // 连续"一次都没出声"的连接失败次数
    uint8_t          _blindCount = 0;   // 本次开机以来"连上却解不出音频"的累计次数
    bool             _stormMode  = false; // 重启风暴保护：本次开机禁用自发重启，只跳台
                                        // （跨电台累计：若换台也救不回来就是库僵死，需重启）
    String           _err;              // 给网页显示的一句话错误说明
    void           (*_onDead)()  = nullptr;
};
