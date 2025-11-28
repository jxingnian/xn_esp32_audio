/*
 * @Author: 星年 jixingnian@gmail.com
 * @Date: 2025-11-22 13:43:50
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-28 20:41:28
 * @FilePath: \xn_esp32_audio\main\main.c
 * @Description:
 *  - 初始化 WiFi 管理模块，确保联网能力正常
 *  - 初始化音频管理器，注册状态机回调
 *  - 当唤醒 + 讲话结束后，将录音回环到扬声器
 *
 * 运行步骤：
 *  1. 供电后等待 “loopback test ready” 日志
 *  2. 说出唤醒词（默认“小鸭小鸭”）或按键唤醒
 *  3. 在 VAD 窗口讲话，结束后会立即播放刚才的录音
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "xn_wifi_manage.h"
#include "audio_manager.h"
#include "audio_config_app.h"

static const char *TAG = "app";

#define LOOPBACK_SECONDS          6
#define LOOPBACK_SAMPLE_RATE      16000
#define LOOPBACK_MAX_SAMPLES      (LOOPBACK_SECONDS * LOOPBACK_SAMPLE_RATE)

typedef struct {
    int16_t *buffer;
    size_t   max_samples;
    size_t   used_samples;
    bool     capturing;
    TickType_t ignore_until;
} loopback_ctx_t;

static int16_t       s_loop_buffer[LOOPBACK_MAX_SAMPLES];
static loopback_ctx_t s_loop_ctx = {
    .buffer = s_loop_buffer,
    .max_samples = LOOPBACK_MAX_SAMPLES,
};

/* 清空捕获缓冲区状态，确保下一轮录音从头开始。 */
static void loopback_reset(loopback_ctx_t *ctx)
{
    ctx->used_samples = 0;
    ctx->capturing = false;
}

/* 将捕获的 PCM 数据依次写入播放缓冲区，触发播放任务输出。 */
static void loopback_playback(loopback_ctx_t *ctx)
{
    if (ctx->used_samples == 0) {
        ESP_LOGW(TAG, "no samples to playback");
        return;
    }

    ESP_LOGI(TAG, "playback start, %u samples", (unsigned)ctx->used_samples);
    audio_manager_clear_playback_buffer();

    size_t offset = 0;
    while (offset < ctx->used_samples) {
        size_t chunk = ctx->used_samples - offset;
        if (chunk > AUDIO_MANAGER_PLAYBACK_FRAME_SAMPLES) {
            chunk = AUDIO_MANAGER_PLAYBACK_FRAME_SAMPLES;
        }
        esp_err_t ret = audio_manager_play_audio(ctx->buffer + offset, chunk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "play audio failed: %s", esp_err_to_name(ret));
            break;
        }
        offset += chunk;
    }

    uint32_t ms = (uint32_t)(ctx->used_samples * 1000 / LOOPBACK_SAMPLE_RATE);
    ms += 300;
    ctx->ignore_until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

static void loopback_record_cb(const int16_t *pcm_data,
                               size_t sample_count,
                               void *user_ctx)
{
    loopback_ctx_t *ctx = (loopback_ctx_t *)user_ctx;
    if (!ctx || !ctx->capturing || !pcm_data || sample_count == 0) {
        return;
    }

    size_t remain = ctx->max_samples - ctx->used_samples;
    if (remain == 0) {
        return;
    }

    size_t to_copy = sample_count > remain ? remain : sample_count;
    memcpy(ctx->buffer + ctx->used_samples, pcm_data, to_copy * sizeof(int16_t));
    ctx->used_samples += to_copy;
}

/* 音频管理事件：驱动唤醒→录音→VAD→回放的状态流。 */
static void audio_event_cb(const audio_mgr_event_t *event, void *user_ctx)
{
    loopback_ctx_t *ctx = (loopback_ctx_t *)user_ctx;
    if (!ctx || !event) {
        return;
    }

    switch (event->type) {
    case AUDIO_MGR_EVENT_VAD_START:
        ESP_LOGI(TAG, "VAD start, begin capture");
        {
            TickType_t now = xTaskGetTickCount();
            if (ctx->ignore_until && (int32_t)(now - ctx->ignore_until) < 0) {
                ESP_LOGI(TAG, "VAD start ignored (echo window)");
                break;
            }
        }
        loopback_reset(ctx);
        ctx->capturing = true;
        break;

    case AUDIO_MGR_EVENT_VAD_END:
        if (ctx->capturing) {
            ctx->capturing = false;
            ESP_LOGI(TAG, "speech ended, total samples=%u", (unsigned)ctx->used_samples);
            loopback_playback(ctx);
        }
        break;

    case AUDIO_MGR_EVENT_WAKEUP_TIMEOUT:
        ESP_LOGW(TAG, "wake window timeout, discard recording");
        loopback_reset(ctx);
        break;

    case AUDIO_MGR_EVENT_BUTTON_TRIGGER:
        ESP_LOGI(TAG, "button trigger, force capture");
        loopback_reset(ctx);
        ctx->capturing = true;
        break;

    default:
        break;
    }
}

static void cpu_usage_monitor_task(void *arg)
{
    char stats[512];

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        memset(stats, 0, sizeof(stats));
        vTaskGetRunTimeStats(stats);
        ESP_LOGI(TAG, "CPU usage stats:\n%s", stats);
    }
}

/* 应用入口：WiFi + 音频管理初始化，把录音/事件回调接入状态机。 */
void app_main(void)
{
    // ESP_LOGI(TAG, "init WiFi manager");
    // ESP_ERROR_CHECK(wifi_manage_init(NULL));

    // xTaskCreatePinnedToCore(cpu_usage_monitor_task,
    //                         "cpu_mon",
    //                         4096,
    //                         NULL,
    //                         1,
    //                         NULL,
    //                         0);
    audio_mgr_config_t audio_cfg;
    audio_config_app_build(&audio_cfg, audio_event_cb, &s_loop_ctx);

    ESP_LOGI(TAG, "init audio manager");
    ESP_ERROR_CHECK(audio_manager_init(&audio_cfg));
    audio_manager_set_volume(100);
    audio_manager_set_record_callback(loopback_record_cb, &s_loop_ctx);
    ESP_ERROR_CHECK(audio_manager_start());
    ESP_ERROR_CHECK(audio_manager_start_playback()); // keep playback task alive

    ESP_LOGI(TAG, "loopback test ready: say wake word -> speak -> hear echo");

}
