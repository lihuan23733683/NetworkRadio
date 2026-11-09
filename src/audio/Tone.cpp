// ============================================================================
//  Tone.cpp  --  I2S 测试音输出
//  使用 ESP-IDF 新版 i2s_std 驱动，端口用 I2S_NUM_1，
//  不碰音频库占用的 I2S_NUM_0（避免把库的通道拆掉）。
// ============================================================================
#include "Tone.h"

#include <driver/i2s_std.h>

#include "../core/AppConfig.h"

#define TONE_SAMPLE_RATE   48000
#define TONE_CHUNK_FRAMES  256

bool tonePlay(uint32_t freq, uint32_t ms, uint8_t ampPct) {
    if (ampPct == 0)   ampPct = 1;
    if (ampPct > 100)  ampPct = 100;

    i2s_chan_handle_t tx = nullptr;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = TONE_CHUNK_FRAMES;

    if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) {
        Serial.println("[测试] i2s_new_channel 失败（I2S_NUM_1 被占用？）");
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_LRC;
    std_cfg.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DOUT;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) {
        Serial.println("[测试] i2s_channel_init_std_mode 失败（引脚冲突？）");
        i2s_del_channel(tx);
        return false;
    }
    if (i2s_channel_enable(tx) != ESP_OK) {
        Serial.println("[测试] i2s_channel_enable 失败");
        i2s_del_channel(tx);
        return false;
    }

    Serial.printf("[测试] I2S%d  BCLK=%d LRC=%d DIN=%d  %lu Hz  %u%% 振幅\n", I2S_NUM_1,
                  PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, (unsigned long)freq, ampPct);

    int16_t  buf[TONE_CHUNK_FRAMES * 2];
    int32_t  amp   = (int32_t)(32767.0f * ampPct / 100.0f);
    double   phase = 0.0;
    double   step  = 2.0 * M_PI * (double)freq / (double)TONE_SAMPLE_RATE;

    uint32_t totalFrames = (uint32_t)((uint64_t)ms * TONE_SAMPLE_RATE / 1000ULL);
    uint32_t done        = 0;
    uint32_t fadeFrames  = TONE_SAMPLE_RATE / 40;   // 25ms 淡入淡出，避免"啪"声
    if (totalFrames < fadeFrames * 2) fadeFrames = totalFrames / 4 + 1;

    while (done < totalFrames) {
        uint32_t n = totalFrames - done;
        if (n > TONE_CHUNK_FRAMES) n = TONE_CHUNK_FRAMES;

        for (uint32_t i = 0; i < n; i++) {
            // 淡入 / 淡出包络
            float env = 1.0f;
            uint32_t pos = done + i;
            if (pos < fadeFrames)                       env = (float)pos / fadeFrames;
            else if (pos > totalFrames - fadeFrames)    env = (float)(totalFrames - pos) / fadeFrames;
            if (env < 0) env = 0;

            int16_t s = (int16_t)(sin(phase) * amp * env);
            buf[i * 2]     = s;   // 左
            buf[i * 2 + 1] = s;   // 右
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        size_t written = 0;
        i2s_channel_write(tx, buf, n * 2 * sizeof(int16_t), &written, 1000);
        done += n;
    }

    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    return true;
}
