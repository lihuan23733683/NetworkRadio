// ============================================================================
//  Player.cpp
// ============================================================================
#include "Player.h"

#include <Preferences.h>

#include "../core/EventLog.h"

// ---------------------------------------------------------------------------
//  构造 / 初始化
// ---------------------------------------------------------------------------
bool Player::begin() {
    // 读取上次音量
    Preferences p;
    p.begin(NVS_NS_APP, true);
    _volume = p.getUChar(NVS_KEY_VOLUME, DEFAULT_VOLUME);
    // 重启风暴检查：上一轮开机周期里自发重启已达上限 -> 本次禁用重启，只跳台
    // （凌晨实测：解码死锁时连着重启 3 次才恢复，期间约 10 分钟静音；
    //   与其反复重启，不如换个台——死锁往往只跟当前这条流有关）
    uint8_t storm = p.getUChar(NVS_KEY_STORM, 0);
    p.end();
    if (storm >= STORM_REBOOT_LIMIT) {
        _stormMode = true;
        Serial.printf("[音频] 检测到重启风暴（上轮自发重启 %u 次），本次禁用自发重启\n", storm);
        EvLog::add("重启风暴保护：禁用自发重启，故障时改跳台");
    }
    if (_volume > MAX_VOLUME) _volume = DEFAULT_VOLUME;

    Serial.printf("[音频] I2S 引脚  BCLK=GPIO%d  LRC=GPIO%d  DIN=GPIO%d\n", PIN_I2S_BCLK,
                  PIN_I2S_LRC, PIN_I2S_DOUT);

    if (!_audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT)) {
        Serial.println("[音频] setPinout 失败，I2S 初始化异常");
        return false;
    }

    _audio.setConnectionTimeout(HTTP_TIMEOUT_MS, HTTP_TIMEOUT_SSL);
    _audio.setVolume(_volume);

    // 库 3.4.x 统一事件回调（在库的音频任务里触发，注意只做轻量打印/置标志）
    Audio::audio_info_callback = [this](Audio::msg_t m) { this->onEvent(m); };

    Serial.printf("[音频] 库版本 %s，音量 %u/%u，就绪\n", _audio.getVersion(), _volume,
                  MAX_VOLUME);
    _state = ST_IDLE;
    return true;
}

// ---------------------------------------------------------------------------
//  事件回调
// ---------------------------------------------------------------------------
void Player::onEvent(const Audio::msg_t& m) {
    const char* txt = m.msg ? m.msg : "";

    switch (m.e) {
        case Audio::evt_info:
            // 库的连接/解码细节，调试期很有用
            if (_verbose) Serial.printf("[音频] %s\n", txt);
            break;

        case Audio::evt_name:
            Serial.printf("[电台] 名称: %s\n", txt);
            break;

        case Audio::evt_streamtitle:
            Serial.printf("[曲目] %s\n", txt);
            _retries = 0;
            if (_state == ST_CONNECTING) _state = ST_PLAYING;
            break;

        case Audio::evt_bitrate:
            Serial.printf("[音频] 码率: %s b/s\n", txt);
            break;

        case Audio::evt_icydescription:
            Serial.printf("[电台] 简介: %s\n", txt);
            break;

        case Audio::evt_id3data:
            if (_verbose) Serial.printf("[ID3 ] %s\n", txt);
            break;

        case Audio::evt_icyurl:
            if (_verbose) Serial.printf("[电台] 主页: %s\n", txt);
            break;

        case Audio::evt_eof:
            handleEof(txt);
            break;

        case Audio::evt_log:
            // 库内部 LOGx，级别低时才有内容
            if (_verbose && txt[0]) Serial.printf("[库] %s\n", txt);
            break;

        default:
            break;
    }
}

void Player::handleEof(const char* who) {
    // 刚发起连接/停止时库也会报 eof，属于正常切换，不要触发重连
    if (millis() - _lastConnectAt < 1500) return;
    if (_state == ST_IDLE || _state == ST_STOPPED || _state == ST_PAUSED) return;
    if (_state == ST_LOST) return;

    Serial.printf("\n[播放] 流断开（%s）\n", (who && who[0]) ? who : "no detail");
    _state  = ST_LOST;
    _lostAt = millis();
    _retryAt = millis() + retryDelayMs();
    _connDeadline = 0;
}

// ---------------------------------------------------------------------------
//  失败处理（v0.5.2 新增）
//  旧版每个判据各自置 LOST，且"库停止运行"这条路不在退避体系里，
//  遇到 403/404 这类死地址会每 4 秒重连一次、永不放手：
//  既猛敲服务器，又把这 24 条的环形事件日志刷满，等于把现场证据冲掉了。
//  现在统一从这里走：关连接 -> 计连击 -> 交给 tickLost() 指数退避。
// ---------------------------------------------------------------------------
void Player::fail(const char* reason) {
    _audio.stopSong();
    _connDeadline = 0;
    _emptySince   = 0;
    _lastClock    = 0xFFFFFFFF;
    _state        = ST_LOST;
    _lostAt       = millis();
    _retryAt      = millis() + retryDelayMs();

    // 「刚还在播」和「从来没播出来过」必须区别对待：
    //   前者是服务器掐流/网络抖动（比如蜻蜓、青蜓这类直播源 1~2 分钟断一次），
    //   重连就该好，绝不能因为连拒几次就把好好的台判死跳走；
    //   后者才是地址失效（403/404）。
    bool recentAudio = _lastAudioAt && (millis() - _lastAudioAt < 60000);

    if (recentAudio) {
        Serial.printf("[播放] 掉线（1 分钟内还在正常播放，按临时断流处理）：%s\n", reason);
        EvLog::add("临时断流：%s", reason);
        _failStreak = 0;
        return;
    }

    if (_failStreak < 255) _failStreak++;
    Serial.printf("[播放] 掉线（连续第 %u 次）：%s\n", _failStreak, reason);
    // 只在第 1 次和每 5 次记一条事件，避免刷屏把有用的历史挤掉
    if (_failStreak == 1 || _failStreak % 5 == 0)
        EvLog::add("掉线第%u次：%s", _failStreak, reason);

    // 连续多次连上去却一个音频字节都没解出来 -> 基本可以判定地址失效
    if (_failStreak >= AUDIO_DEAD_STREAK) {
        _err = String("该地址连续 ") + String(_failStreak) +
               " 次无音频返回，疑似失效（服务器拒绝或已下线）";
        Serial.printf("[播放] %s\n", _err.c_str());
        EvLog::add("疑似死地址：连续%u次无音频", _failStreak);
        _failStreak = 0;
        _retries    = 0;
        if (_onDead) _onDead();   // 让上层自动跳下一台
    }
}

// ---------------------------------------------------------------------------
//  播放控制
// ---------------------------------------------------------------------------
bool Player::playUrl(const String& url, const String& label) {
    if (url.length() == 0) {
        Serial.println("[播放] 地址为空");
        return false;
    }

    if (_state != ST_IDLE && _state != ST_STOPPED) _audio.stopSong();
    delay(60);

    _url   = url;
    _label = label;
    _paused = false;

    // 计数器全部归零：这是用户主动选台，不应继承上一台的失败记录
    _retries = 0;
    _failStreak = 0;
    _blindCount = 0;
    _lastAudioAt = 0;      // 新电台重新计"出过声没有"，别继承上一台的记录
    _retryAt = 0;
    _err = "";
    _connDeadline = millis() + AUDIO_CONNECT_GRACE_MS;
    _emptySince   = 0;
    _lastClock    = 0xFFFFFFFF;
    _clockAt      = millis();
    _playSince    = 0;
    // 注意：这里先置"连接中"，等真的解出音频才转"播放中"。
    // 旧版直接置播放中，导致 403 页面连上后立刻命中"库停止运行"判据，
    // 0.3 秒内就在 播放中<->掉流 之间弹跳。
    _state = ST_CONNECTING;

    Serial.printf("[播放] 连接中: %s\n", _url.c_str());
    uint32_t t0 = millis();
    _lastConnectAt = t0;

    if (!_audio.connecttohost(_url.c_str())) {
        Serial.println("[播放] 连接失败：地址失效 / DNS 失败 / 服务器拒绝 / 网络不通");
        _state = ST_LOST;
        _lostAt = millis();
        _connDeadline = 0;
        _failStreak = 1;
        _retryAt = millis() + retryDelayMs();
        return false;
    }

    Serial.printf("[播放] 已发起连接，握手 %lu ms，等待音频数据\n",
                  (unsigned long)(millis() - t0));
    return true;
}

void Player::stop() {
    _audio.stopSong();
    _state   = ST_STOPPED;
    _retries = 0;
    _paused  = false;
    _connDeadline = 0;
    _emptySince   = 0;
    _url     = "";
    _label   = "";
    Serial.println("[播放] 已停止");
}

bool Player::togglePause() {
    // 直播流没有"从暂停处继续"的概念，pauseResume() 从半截解码数据继续
    // 会产生刺耳杂音，所以这里：暂停 = 停止取流（静音），恢复 = 重连直播
    if (_state == ST_PLAYING || _state == ST_CONNECTING) {
        _state  = ST_PAUSED;
        _paused = true;
        _audio.stopSong();
        Serial.println("[播放] 已暂停（恢复时重新连接直播流，约 1~3 秒）");
        return true;
    }
    if (_state == ST_PAUSED) {
        if (_url.length() == 0) {
            Serial.println("[播放] 没有可恢复的节目");
            return false;
        }
        _paused        = false;
        _state         = ST_CONNECTING;
        _retries       = 0;
        _failStreak    = 0;
        _lastClock     = 0xFFFFFFFF;
        _clockAt       = millis();
        _connDeadline  = millis() + AUDIO_CONNECT_GRACE_MS;
        _lastConnectAt = millis();
        Serial.println("[播放] 恢复播放，重新连接 ...");
        if (!_audio.connecttohost(_url.c_str())) {
            Serial.println("[播放] 重连失败，稍后自动重试");
            _state  = ST_LOST;
            _lostAt = millis();
            _connDeadline = 0;
            _failStreak = 1;
            return false;
        }
        // 仍然停在"连接中"：等真的解出音频再转"播放中"（由 tickConnecting 判定）
        return true;
    }
    Serial.println("[播放] 当前没有在播放，无法暂停");
    return false;
}

void Player::setVolume(uint8_t vol) {
    if (vol > MAX_VOLUME) vol = MAX_VOLUME;
    _volume = vol;
    _audio.setVolume(_volume);
    // NVS 是闪存写入（几十 ms 且阻塞），连续点音量键时不能每次都写；
    // 音量停止变化 VOLUME_SAVE_DELAY_MS 后由 loop() 落盘
    _volDirty   = true;
    _volDirtyAt = millis();
}

void Player::volumeDelta(int d) {
    int v = (int)_volume + d;
    if (v < 0) v = 0;
    if (v > MAX_VOLUME) v = MAX_VOLUME;
    setVolume((uint8_t)v);
}

void Player::setMute(bool m) {
    _muted = m;
    _audio.setMute(m);
    Serial.printf("[音量] %s\n", m ? "已静音" : "取消静音");
}

// ---------------------------------------------------------------------------
//  主循环
// ---------------------------------------------------------------------------
void Player::loop() {
    _audio.loop();

    // 音量停止变化一段时间后落盘 NVS
    if (_volDirty && millis() - _volDirtyAt >= VOLUME_SAVE_DELAY_MS) {
        _volDirty = false;
        Preferences p;
        if (p.begin(NVS_NS_APP, false)) {
            p.putUChar(NVS_KEY_VOLUME, _volume);
            p.end();
        }
    }

    switch (_state) {
        case ST_CONNECTING: tickConnecting(); break;
        case ST_PLAYING:    tickPlaying();    break;
        case ST_LOST:       tickLost();       break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
//  连接中：等"真的解出音频"才算成功
//  判据用解码时钟 > 0 或缓冲里已有数据。连上 TCP 甚至收完响应头都不算成功，
//  403/404 页面同样能连上，这正是上一版每 4 秒重连一次的原因。
// ---------------------------------------------------------------------------
void Player::tickConnecting() {
    uint32_t clk = (uint32_t)_audio.getAudioCurrentTime();

    // 判据①：解码时钟开始走 = 真的解出声音了（最可靠）
    // 判据②：缓冲里有数据 + 库还在跑 + 已过 2 秒（照顾个别时钟不前进的流）
    // 只用"缓冲有数据"会被 403/404 的 HTML 响应体骗过，所以加时间与 isRunning 门槛
    bool decoded = (clk > 0);
    bool hasData = (!decoded) && _audio.inBufferFilled() > 0 && _audio.isRunning() &&
                   (millis() - _lastConnectAt > 2000);

    if (decoded || hasData) {
        _state        = ST_PLAYING;
        _playSince    = millis();
        _lastClock    = clk;
        _clockAt      = millis();
        _connDeadline = 0;
        _emptySince   = 0;
        _lastAudioAt  = millis();
        Serial.printf("[播放] 已开始出声（解码时钟 %lu s）\n", (unsigned long)clk);
        if (_failStreak) {
            EvLog::add("重连成功（此前连续失败 %u 次）", _failStreak);
            _failStreak = 0;
        }
        _err = "";
        return;
    }

    if (!_audio.isRunning()) {
        fail("库放弃连接（服务器拒绝 / 403 / 无可用音频）");
        return;
    }
    if (_connDeadline && (int32_t)(millis() - _connDeadline) > 0) {
        fail("连上后迟迟没有音频数据");
    }
}

// ---------------------------------------------------------------------------
//  播放中：三个看门狗
//   1) 库自己停止运行 -> 立刻重连（0 成本判据）
//   2) 缓冲区持续为空 -> 服务器断流（蜻蜓FM 这类十几分钟掐一次连接）
//   3) 解码时钟不前进 -> 解码器僵死（HE-AAC 流的典型症状，状态写着播放中却没声）
// ---------------------------------------------------------------------------
void Player::tickPlaying() {
    uint32_t clk = (uint32_t)_audio.getAudioCurrentTime();

    // 1) 库自己举手投降
    if (!_audio.isRunning()) {
        if (clk > 0) {
            _retries = 0;      // 曾经出声，是正常的流断开，快速重连
            _failStreak = 0;
            fail("音频库自行停止（流被服务器关闭）");
        } else {
            fail("音频库未能启动（地址无效 / 被拒绝）");
        }
        return;
    }

    // 稳定出声 10 秒 -> 连接健康，退避与失败计数清零
    if (clk > 0 && _playSince && millis() - _playSince > 10000) {
        _retries = 0;
        _failStreak = 0;
        _blindCount = 0;
        if (_err.length()) _err = "";
    }

    // 稳定出声 5 分钟 -> 重启风暴计数清零（本次开机周期已证明恢复正常）
    if (_stormMode && clk > 0 && _playSince && millis() - _playSince > 300000) {
        _stormMode = false;
        Preferences p;
        if (p.begin(NVS_NS_APP, false)) {
            p.putUChar(NVS_KEY_STORM, 0);
            p.end();
        }
        EvLog::add("稳定播放5分钟，解除风暴保护");
        Serial.println("[播放] 稳定播放 5 分钟，重启风暴计数已清零");
    }

    // 2) 缓冲持续为空
    if (_audio.inBufferFilled() == 0) {
        if (_emptySince == 0) {
            _emptySince = millis();
        } else if (millis() - _emptySince >= AUDIO_STALL_TIMEOUT_MS) {
            _playSince = 0;
            fail("缓冲持续为空（服务器不再推送）");
            return;
        }
    } else {
        _emptySince = 0;
        if (_playSince == 0) _playSince = millis();
    }

    // 3) 解码心跳看门狗（v0.5.1 重做，v0.5.2 与失败计数打通）
    // 依据：库的 getAudioCurrentTime() 只有"真的解出 PCM 帧"才前进，
    // 不关心缓冲区是满、是空、还是网络在抖 —— 这正是"有没有声音"的本质。
    if (_lastClock == 0xFFFFFFFF || clk != _lastClock) {
        _lastClock = clk;
        _clockAt   = millis();
        if (clk > 0) _lastAudioAt = millis();   // "刚还在播"的凭据
        return;
    }
    if (millis() - _clockAt < AUDIO_CLOCK_STALL_MS) return;

    _stallCount++;
    uint32_t now = millis();
    if (_lastStallAt && now - _lastStallAt < 120000) {
        if (_stallBurst < 255) _stallBurst++;
    } else {
        _stallBurst = 1;
    }
    _lastStallAt = now;
    _playSince   = 0;

    uint32_t fill = (uint32_t)_audio.inBufferFilled();
    uint32_t br   = (uint32_t)_audio.getBitRate();
    Serial.printf("[播放] 解码停滞（时钟停在 %lu s，缓冲 %lu/%lu B，码率 %lu，连击 %u）\n",
                  (unsigned long)clk, (unsigned long)fill,
                  (unsigned long)_audio.getInBufferSize(), (unsigned long)br, _stallBurst);
    // 连击时不刷事件，只在第 1 次和每 5 次记一条
    if (_stallBurst <= 3 || _stallBurst % 5 == 0)
        EvLog::add("解码停滞 时钟%lus 缓冲%luB 连击%u", (unsigned long)clk,
                   (unsigned long)fill, _stallBurst);

    _audio.stopSong();

    // ---- 从来没有解出过声音（clk==0）----
    // 典型形态：缓冲里有几十上百 KB 数据、码率 0、时钟 0 —— 数据进来了却没人解码。
    // 这在库里的根因是 m_f_I2S_init 被置 false（audioTask 只在它为 true 时才解码），
    // 而它只有在 setPinout() 里被赋值：一旦某次 setPinout 失败，解码就永久停摆。
    // 所以这类失败按"这台播不了"计进连击，4 次后自动换台；全局都播不了则重启。
    if (clk == 0) {
        if (_blindCount < 255) _blindCount++;
        Serial.printf("[播放] 连上却解不出音频（已连续 %u 次），按连接失败处理\n", _blindCount);
        fail("连上后无法解码（数据进入但解码器未启动）");
        // 换台也救不回来 = 库整体僵死，重启是唯一可用的清除手段
        if (_blindCount >= 6 && millis() > REBOOT_MIN_UPTIME_MS) rebootSelf("解码器僵死，换台无效");
        return;
    }

    // ---- 播过一段才卡死 ----
    _blindCount = 0;
    if (_stallBurst >= 3) {
        // 连续 3 次（2 分钟内）僵死：解码器基本废了，重新初始化 I2S 再试。
        // 关键：setPinout() 会写 m_f_I2S_init，一旦它返回 false，库的 audioTask
        // 会永远跳过解码（数据进得来、就是没人解），表现就是"换台也没声音"。
        // 所以必须检查返回值 —— 这是上一版把机器弄哑的真凶。
        Serial.println("[播放] 连续僵死，重新初始化 I2S（检查返回值）");
        delay(80);
        bool ok = _audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
        if (!ok) {
            // 重初始化失败 = 解码器已被库自己锁死，外部没有任何 API 能救，只能重启
            if (millis() > REBOOT_MIN_UPTIME_MS) {
                rebootSelf("I2S 重初始化失败，解码器已锁死");
            }
            // 刚开机就失败：先别重启（防止重启风暴），按"这台播不了"换台
            fail("I2S 重初始化失败");
            return;
        }
        _audio.setVolume(_volume);
        EvLog::add("连击恢复：重新初始化 I2S 成功");
        _stallBurst = 0;
    }

    _state     = ST_LOST;
    _lastClock = 0xFFFFFFFF;      // 下一段播放重新建立基线
    _clockAt   = now;
    // 停滞属于解码器抽风/服务器掐流，不是地址失效：退避清零，等 8 秒再连
    // （立刻重连常被服务器拒绝，反而白试几次；8 秒既够它释放旧会话，用户也几乎无感）
    _retries = 0;
    _lostAt  = now;
    _retryAt = now + AUDIO_STALL_RECONNECT_MS;
}

// 自我重启：库的解码任务一旦被 m_f_I2S_init=false 卡死，外部没有任何 API 能救回
// （setPinout 是真凶：失败时会把标志置 false，反而把解码器彻底停掉）。
// 重启 3 秒 > 永久无声，这是最后一道自愈手段。
void Player::rebootSelf(const char* why) {
    Serial.printf("[播放] 自发重启：%s\n", why);
    EvLog::add("自发重启：%s", why);

    // 风暴保护：上轮开机周期已经自发重启过 STORM_REBOOT_LIMIT 次还没好，
    // 说明重启治不了这个病（换台也大概率一样），这次不重启，交给判死地址跳台。
    if (_stormMode) {
        Serial.println("[播放] 风暴保护生效：不再重启，改为跳台");
        EvLog::add("风暴保护：跳过重启，改跳台");
        return;
    }

    // 记录一次自发重启（重启后 begin() 里读出，达到上限则进入风暴保护）
    {
        Preferences p;
        if (p.begin(NVS_NS_APP, false)) {
            uint8_t n = p.getUChar(NVS_KEY_STORM, 0);
            if (n < 250) p.putUChar(NVS_KEY_STORM, n + 1);
            p.end();
        }
    }

    _audio.stopSong();
    delay(120);
    ESP.restart();
}

// ---------------------------------------------------------------------------
//  掉流等待：指数退避 4/8/16/30/60 秒，永不放弃
// ---------------------------------------------------------------------------
void Player::tickLost() {
    if (_url.length() == 0) return;
    if (_retryAt && (int32_t)(millis() - _retryAt) < 0) return;

    if (_retries < 60) _retries++;
    _lastConnectAt = millis();
    _state        = ST_CONNECTING;
    _connDeadline = millis() + AUDIO_CONNECT_GRACE_MS;

    Serial.printf("[播放] 第 %u 次重连（已连续失败 %u 次，上次等待 %lu ms）\n",
                  _retries, _failStreak, (unsigned long)retryDelayMs());

    if (!_audio.connecttohost(_url.c_str())) {
        _state        = ST_LOST;
        _lostAt       = millis();
        _connDeadline = 0;
        if (_failStreak < 255) _failStreak++;
        _retryAt = millis() + retryDelayMs();
        // 连不上也算一次失败：连击到阈值同样判死地址、跳下一台
        if (_failStreak >= AUDIO_DEAD_STREAK) {
            _err = String("该地址连续 ") + String(_failStreak) + " 次连不上，疑似失效";
            EvLog::add("疑似死地址：连续%u次连不上", _failStreak);
            Serial.printf("[播放] %s\n", _err.c_str());
            _failStreak = 0;
            _retries    = 0;
            if (_onDead) _onDead();
        }
    }
}

uint32_t Player::retryDelayMs() const {
    // 4 -> 8 -> 16 -> 30 -> 60 秒，之后固定 60 秒（不再猛敲服务器）
    static const uint16_t tbl[] = {4, 8, 16, 30, 60, 60};
    uint8_t i = _retries;
    if (i > 5) i = 5;
    return (uint32_t)tbl[i] * 1000UL;
}


// ---------------------------------------------------------------------------
//  状态打印
// ---------------------------------------------------------------------------
void Player::printInfo() {
    Serial.println("---- 音频 ----");
    Serial.printf("  播放状态 : %s\n", stateName(_state));
    Serial.printf("  节目     : %s\n", _label.length() ? _label.c_str() : "-");
    Serial.printf("  地址     : %s\n", _url.length() ? _url.c_str() : "-");
    Serial.printf("  音量     : %u/%u %s\n", _volume, MAX_VOLUME, _muted ? "(静音)" : "");
    if (_state == ST_PLAYING || _state == ST_PAUSED) {
        Serial.printf("  码率     : %lu b/s\n", (unsigned long)_audio.getBitRate());
        Serial.printf("  采样率   : %lu Hz\n", (unsigned long)_audio.getSampleRate());
        Serial.printf("  位深/声道: %u bit / %u ch\n", _audio.getBitsPerSample(),
                      _audio.getChannels());
        Serial.printf("  VU 电平  : %u\n", _audio.getVUlevel());
    }
    Serial.printf("  缓冲余量 : %lu\n", (unsigned long)_audio.getHighWatermark());
    Serial.printf("  缓冲填充 : %lu / %lu B\n", (unsigned long)_audio.inBufferFilled(),
                  (unsigned long)_audio.getInBufferSize());
}

const char* Player::stateName(State s) {
    switch (s) {
        case ST_IDLE:       return "空闲";
        case ST_CONNECTING: return "连接中";
        case ST_PLAYING:    return "播放中";
        case ST_PAUSED:     return "暂停";
        case ST_LOST:       return "掉流/等待重连";
        case ST_STOPPED:    return "已停止";
        default:            return "未知";
    }
}
