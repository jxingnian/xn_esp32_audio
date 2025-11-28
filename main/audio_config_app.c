/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-27 21:48:21
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-28 20:47:00
 * @FilePath: \xn_esp32_audio\main\audio_config_app.c
 * @Description: 音频配置
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */
#include "audio_config_app.h"

void audio_config_app_build(audio_mgr_config_t *cfg,
                            audio_mgr_event_cb_t event_cb,
                            void *user_ctx)
{
    if (!cfg) {
        return;
    }

    *cfg = AUDIO_MANAGER_DEFAULT_CONFIG();

    cfg->hw_config.mic.port = 1;
    cfg->hw_config.mic.bclk_gpio = 15;
    cfg->hw_config.mic.lrck_gpio = 2;
    cfg->hw_config.mic.din_gpio = 39;
    cfg->hw_config.mic.sample_rate = 16000;
    cfg->hw_config.mic.bits = 32;

    cfg->hw_config.speaker.port = 0;
    cfg->hw_config.speaker.bclk_gpio = 48;
    cfg->hw_config.speaker.lrck_gpio = 38;
    cfg->hw_config.speaker.dout_gpio = 47;
    cfg->hw_config.speaker.sample_rate = 16000;
    cfg->hw_config.speaker.bits = 16;

    cfg->hw_config.button.gpio = 0;
    cfg->hw_config.button.active_low = true;

    cfg->wakeup_config.enabled = false;
    cfg->wakeup_config.wake_word_name = "小鸭小鸭";
    cfg->wakeup_config.model_partition = "model";
    cfg->wakeup_config.sensitivity = 2;
    cfg->wakeup_config.wakeup_timeout_ms = 8000;
    cfg->wakeup_config.wakeup_end_delay_ms = 1200;

    cfg->vad_config.enabled = true;
    cfg->vad_config.vad_mode = 2;
    cfg->vad_config.min_speech_ms = 200;
    cfg->vad_config.min_silence_ms = 400;

    cfg->afe_config.aec_enabled = true;
    cfg->afe_config.ns_enabled = true;
    cfg->afe_config.agc_enabled = true;
    cfg->afe_config.afe_mode = 1;

    cfg->event_callback = event_cb;
    cfg->user_ctx = user_ctx;
}


