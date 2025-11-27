/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-27 19:17:04
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-27 19:20:57
 * @FilePath: \xn_esp32_audio\components\audio_manager\src\audio_manager.c
 * @Description: 音频管理器实现 - 模块化架构
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */
#include "audio_manager.h"
#include "ring_buffer.h"
#include "i2s_hal.h"
#include "playback_controller.h"
#include "button_handler.h"
#include "afe_wrapper.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AUDIO_MGR";

// ============ 配置常量 ============

/**
 * @brief 播放帧大小（采样点数）
 * 每次从播放缓冲区读取的采样点数，影响播放延迟和 CPU 占用
 */
#define PLAYBACK_FRAME_SAMPLES      1024

/**
 * @brief 播放缓冲区大小（字节）
 * 用于缓存待播放的音频数据，512KB 可存储约 6 秒的音频（16kHz, 16bit）
 */
#define PLAYBACK_BUFFER_SIZE        (512 * 1024)  // 512KB

/**
 * @brief 回采缓冲区大小（字节）
 * 用于存储扬声器播放的音频数据，供 AEC 使用，16KB 可存储约 0.5 秒的音频
 */
#define REFERENCE_BUFFER_SIZE       (16 * 1024)   // 16KB

// ============ 音频管理器上下文 ============

/**
 * @brief 音频管理器上下文结构体
 * 
 * 存储音频管理器的所有状态和配置信息，包括：
 * - 各模块的句柄
 * - 共享缓冲区
 * - 运行状态
 * - 回调函数
 */
typedef struct {
    // 配置
    audio_mgr_config_t config;              ///< 音频管理器配置参数
    
    // 模块句柄
    i2s_hal_handle_t i2s_hal;              ///< I2S 硬件抽象层句柄
    playback_controller_handle_t playback_ctrl;  ///< 播放控制器句柄
    button_handler_handle_t button_handler; ///< 按键处理器句柄
    afe_wrapper_handle_t afe_wrapper;      ///< AFE 包装器句柄
    
    // 共享缓冲区
    ring_buffer_handle_t reference_rb;     ///< 回采缓冲区句柄（播放控制器和 AFE 共享）
    
    // 状态
    bool initialized;                       ///< 是否已初始化
    bool running;                           ///< 是否正在运行（监听音频）
    bool recording;                         ///< 是否正在录音
    uint8_t volume;                         ///< 音量（0-100）
    
    // 回调
    audio_record_callback_t record_callback; ///< 录音数据回调函数
    void *record_ctx;                        ///< 录音回调的用户上下文

} audio_manager_ctx_t;

/**
 * @brief 音频管理器全局上下文实例
 * 使用静态变量存储，确保全局唯一性
 */
static audio_manager_ctx_t s_ctx = {0};

// ============ 内部回调函数 ============

/**
 * @brief 按键事件回调函数
 * 
 * 当按键被按下或松开时，由按键处理器调用此函数。
 * 将按键事件转换为音频管理器事件并通知上层应用。
 * 
 * @param event 按键事件类型（按下/松开）
 * @param user_ctx 用户上下文（未使用）
 */
static void button_event_handler(button_event_type_t event, void *user_ctx)
{
    // 检查是否有事件回调函数
    if (!s_ctx.config.event_callback) return;
    
    // 构造音频管理器事件
    audio_mgr_event_t mgr_event = {0};
    
    if (event == BUTTON_EVENT_PRESS) {
        ESP_LOGI(TAG, "🔘 按键按下，触发对话");
        mgr_event.type = AUDIO_MGR_EVENT_BUTTON_TRIGGER;
    } else if (event == BUTTON_EVENT_RELEASE) {
        ESP_LOGI(TAG, "🔘 按键松开");
        mgr_event.type = AUDIO_MGR_EVENT_BUTTON_RELEASE;
    }
    
    // 通知上层应用
    s_ctx.config.event_callback(&mgr_event, s_ctx.config.user_ctx);
}

/**
 * @brief AFE 事件回调函数
 * 
 * 当 AFE 检测到唤醒词、VAD 开始/结束时，由 AFE 包装器调用此函数。
 * 将 AFE 事件转换为音频管理器事件并通知上层应用。
 * 
 * @param event AFE 事件指针
 * @param user_ctx 用户上下文（未使用）
 */
static void afe_event_handler(const afe_event_t *event, void *user_ctx)
{
    // 检查是否有事件回调函数
    if (!s_ctx.config.event_callback) return;
    
    // 构造音频管理器事件
    audio_mgr_event_t mgr_event = {0};
    
    switch (event->type) {
        case AFE_EVENT_WAKEUP_DETECTED:
            // 唤醒词检测事件
            mgr_event.type = AUDIO_MGR_EVENT_WAKEUP_DETECTED;
            mgr_event.data.wakeup.wake_word_index = event->data.wakeup.wake_word_index;
            mgr_event.data.wakeup.volume_db = event->data.wakeup.volume_db;
            break;
            
        case AFE_EVENT_VAD_START:
            // VAD 开始检测到语音
            mgr_event.type = AUDIO_MGR_EVENT_VAD_START;
            break;
            
        case AFE_EVENT_VAD_END:
            // VAD 检测到语音结束
            mgr_event.type = AUDIO_MGR_EVENT_VAD_END;
            break;
    }
    
    // 通知上层应用
    s_ctx.config.event_callback(&mgr_event, s_ctx.config.user_ctx);
}

/**
 * @brief AFE 录音数据回调函数
 * 
 * 当 AFE 处理完音频数据后，调用此函数将处理后的音频数据传递给上层应用。
 * 
 * @param pcm_data PCM 音频数据指针
 * @param samples 采样点数
 * @param user_ctx 用户上下文（未使用）
 */
static void afe_record_handler(const int16_t *pcm_data, size_t samples, void *user_ctx)
{
    // 如果设置了录音回调，则调用它
    if (s_ctx.record_callback) {
        s_ctx.record_callback(pcm_data, samples, s_ctx.record_ctx);
    }
}

// ============ 公共 API 实现 ============

/**
 * @brief 初始化音频管理器
 * 
 * 按照以下顺序初始化各个模块：
 * 1. 创建 I2S HAL（硬件抽象层）
 * 2. 创建回采缓冲区（用于 AEC）
 * 3. 创建播放控制器（管理音频播放）
 * 4. 创建 AFE 包装器（音频前端处理）
 * 5. 创建按键处理器（处理物理按键）
 * 
 * @param config 音频管理器配置参数
 * @return 
 *     - ESP_OK: 初始化成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t audio_manager_init(const audio_mgr_config_t *config)
{
    // 检查是否已经初始化
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "音频管理器已初始化");
        return ESP_OK;
    }

    // 参数检查
    if (!config || !config->event_callback) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "======== 初始化音频管理器（模块化架构）========");

    // 保存配置
    memcpy(&s_ctx.config, config, sizeof(audio_mgr_config_t));
    s_ctx.volume = 80;  // 默认音量 80%

    // ========== 1. 创建 I2S HAL ==========
    // 配置麦克风 I2S 参数
    i2s_mic_config_t mic_cfg = {
        .port = config->hw_config.mic.port,
        .bclk_gpio = config->hw_config.mic.bclk_gpio,
        .lrck_gpio = config->hw_config.mic.lrck_gpio,
        .din_gpio = config->hw_config.mic.din_gpio,
        .sample_rate = config->hw_config.mic.sample_rate,
        .bits = config->hw_config.mic.bits,
        .max_frame_samples = 512,  // 预分配 512 采样点的缓冲区
        .bit_shift = 14,           // 默认右移 14 位（可根据音量调整：12-16）
    };

    // 配置扬声器 I2S 参数
    i2s_speaker_config_t speaker_cfg = {
        .port = config->hw_config.speaker.port,
        .bclk_gpio = config->hw_config.speaker.bclk_gpio,
        .lrck_gpio = config->hw_config.speaker.lrck_gpio,
        .dout_gpio = config->hw_config.speaker.dout_gpio,
        .sample_rate = config->hw_config.speaker.sample_rate,
        .bits = config->hw_config.speaker.bits,
        .max_frame_samples = PLAYBACK_FRAME_SAMPLES,
    };

    // 创建 I2S HAL 实例
    s_ctx.i2s_hal = i2s_hal_create(&mic_cfg, &speaker_cfg);
    if (!s_ctx.i2s_hal) {
        ESP_LOGE(TAG, "I2S HAL 创建失败");
        return ESP_ERR_NO_MEM;
    }

    // ========== 2. 创建回采缓冲区 ==========
    // 回采缓冲区用于存储扬声器播放的音频，供 AEC 使用
    // 注意：这里创建的缓冲区会被播放控制器接管，后续会重新获取
    s_ctx.reference_rb = ring_buffer_create(REFERENCE_BUFFER_SIZE / sizeof(int16_t), false);
    if (!s_ctx.reference_rb) {
        ESP_LOGE(TAG, "回采缓冲区创建失败");
        i2s_hal_destroy(s_ctx.i2s_hal);
        return ESP_ERR_NO_MEM;
    }

    // ========== 3. 创建播放控制器 ==========
    playback_controller_config_t playback_cfg = {
        .i2s_hal = s_ctx.i2s_hal,
        .playback_buffer_samples = PLAYBACK_BUFFER_SIZE / sizeof(int16_t),
        .reference_buffer_samples = REFERENCE_BUFFER_SIZE / sizeof(int16_t),
        .frame_samples = PLAYBACK_FRAME_SAMPLES,
        .reference_callback = NULL,  // 使用缓冲区方式，不使用回调
        .reference_ctx = NULL,
        .volume_ptr = &s_ctx.volume,  // 共享音量指针
    };

    s_ctx.playback_ctrl = playback_controller_create(&playback_cfg);
    if (!s_ctx.playback_ctrl) {
        ESP_LOGE(TAG, "播放控制器创建失败");
        ring_buffer_destroy(s_ctx.reference_rb);
        i2s_hal_destroy(s_ctx.i2s_hal);
        return ESP_ERR_NO_MEM;
    }

    // 获取播放控制器的回采缓冲区（播放控制器会创建自己的缓冲区）
    s_ctx.reference_rb = playback_controller_get_reference_buffer(s_ctx.playback_ctrl);

    // ========== 4. 创建 AFE 包装器 ==========
    // 配置唤醒词检测参数
    afe_wakeup_config_t afe_wakeup = {
        .enabled = config->wakeup_config.enabled,
        .wake_word_name = config->wakeup_config.wake_word_name,
        .model_partition = config->wakeup_config.model_partition,
        .sensitivity = config->wakeup_config.sensitivity,
    };

    // 配置 VAD 参数
    afe_vad_config_t afe_vad = {
        .enabled = config->vad_config.enabled,
        .vad_mode = config->vad_config.vad_mode,
        .min_speech_ms = config->vad_config.min_speech_ms,
        .min_silence_ms = config->vad_config.min_silence_ms,
    };

    // 配置音频前端处理参数
    afe_feature_config_t afe_feature = {
        .aec_enabled = config->afe_config.aec_enabled,
        .ns_enabled = config->afe_config.ns_enabled,
        .agc_enabled = config->afe_config.agc_enabled,
        .afe_mode = config->afe_config.afe_mode,
    };

    // 配置 AFE 包装器
    afe_wrapper_config_t afe_cfg = {
        .i2s_hal = s_ctx.i2s_hal,
        .reference_rb = s_ctx.reference_rb,  // 共享回采缓冲区
        .wakeup_config = afe_wakeup,
        .vad_config = afe_vad,
        .feature_config = afe_feature,
        .event_callback = afe_event_handler,  // AFE 事件回调
        .event_ctx = NULL,
        .record_callback = afe_record_handler,  // 录音数据回调
        .record_ctx = NULL,
        .running_ptr = &s_ctx.running,  // 共享运行状态指针
        .recording_ptr = &s_ctx.recording,  // 共享录音状态指针
    };

    s_ctx.afe_wrapper = afe_wrapper_create(&afe_cfg);
    if (!s_ctx.afe_wrapper) {
        ESP_LOGE(TAG, "AFE 包装器创建失败");
        playback_controller_destroy(s_ctx.playback_ctrl);
        i2s_hal_destroy(s_ctx.i2s_hal);
        return ESP_ERR_NO_MEM;
    }

    // ========== 5. 创建按键处理器 ==========
    button_handler_config_t button_cfg = {
        .gpio = config->hw_config.button.gpio,
        .active_low = config->hw_config.button.active_low,
        .debounce_ms = 50,  // 50ms 防抖
        .callback = button_event_handler,  // 按键事件回调
        .user_ctx = NULL,
    };

    s_ctx.button_handler = button_handler_create(&button_cfg);
    if (!s_ctx.button_handler) {
        ESP_LOGE(TAG, "按键处理器创建失败");
        afe_wrapper_destroy(s_ctx.afe_wrapper);
        playback_controller_destroy(s_ctx.playback_ctrl);
        i2s_hal_destroy(s_ctx.i2s_hal);
        return ESP_ERR_NO_MEM;
    }

    // 标记为已初始化
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "✅ 音频管理器初始化完成（模块化架构）");
    ESP_LOGI(TAG, "   - I2S HAL: ✓");
    ESP_LOGI(TAG, "   - 播放控制器: ✓");
    ESP_LOGI(TAG, "   - AFE 包装器: ✓");
    ESP_LOGI(TAG, "   - 按键处理器: ✓");

    return ESP_OK;
}

/**
 * @brief 反初始化音频管理器
 * 
 * 按照与初始化相反的顺序销毁各个模块，释放资源。
 * 注意：reference_rb 由播放控制器管理，不需要单独销毁。
 */
void audio_manager_deinit(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return;

    // 停止所有运行中的功能
    audio_manager_stop();
    audio_manager_stop_playback();

    // 销毁按键处理器
    if (s_ctx.button_handler) {
        button_handler_destroy(s_ctx.button_handler);
        s_ctx.button_handler = NULL;
    }

    // 销毁 AFE 包装器
    if (s_ctx.afe_wrapper) {
        afe_wrapper_destroy(s_ctx.afe_wrapper);
        s_ctx.afe_wrapper = NULL;
    }

    // 销毁播放控制器
    if (s_ctx.playback_ctrl) {
        playback_controller_destroy(s_ctx.playback_ctrl);
        s_ctx.playback_ctrl = NULL;
    }

    // 销毁 I2S HAL
    if (s_ctx.i2s_hal) {
        i2s_hal_destroy(s_ctx.i2s_hal);
        s_ctx.i2s_hal = NULL;
    }

    // reference_rb 由播放控制器管理，不需要单独销毁

    // 清空上下文
    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "音频管理器已销毁");
}

/**
 * @brief 启动音频监听
 * 
 * 启动音频监听功能，开始检测唤醒词和语音活动。
 * 
 * @return 
 *     - ESP_OK: 启动成功
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_start(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    
    // 如果已经在运行，直接返回
    if (s_ctx.running) return ESP_OK;

    ESP_LOGI(TAG, "🎧 启动音频监听...");
    s_ctx.running = true;

    ESP_LOGI(TAG, "✅ 音频监听已启动，等待唤醒词: %s",
             s_ctx.config.wakeup_config.wake_word_name);

    return ESP_OK;
}

/**
 * @brief 停止音频监听
 * 
 * 停止音频监听功能，不再检测唤醒词和语音活动。
 * 
 * @return ESP_OK: 停止成功
 */
esp_err_t audio_manager_stop(void)
{
    // 如果未运行，直接返回
    if (!s_ctx.running) return ESP_OK;

    ESP_LOGI(TAG, "🛑 停止音频监听");
    s_ctx.running = false;
    s_ctx.recording = false;

    return ESP_OK;
}

/**
 * @brief 触发对话
 * 
 * 手动触发对话，模拟按键按下事件。
 * 用于程序内部触发对话，而不需要物理按键。
 * 
 * @return 
 *     - ESP_OK: 触发成功
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_trigger_conversation(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    // 构造按键触发事件
    audio_mgr_event_t event = {
        .type = AUDIO_MGR_EVENT_BUTTON_TRIGGER,
    };

    // 通知上层应用
    if (s_ctx.config.event_callback) {
        s_ctx.config.event_callback(&event, s_ctx.config.user_ctx);
    }

    return ESP_OK;
}

/**
 * @brief 开始录音
 * 
 * 设置录音标志，AFE 会开始将处理后的音频数据通过回调传递给上层应用。
 * 
 * @return 
 *     - ESP_OK: 开始成功
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_start_recording(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "📼 开始录音");
    s_ctx.recording = true;

    return ESP_OK;
}

/**
 * @brief 停止录音
 * 
 * 清除录音标志，AFE 停止传递音频数据。
 * 
 * @return ESP_OK: 停止成功
 */
esp_err_t audio_manager_stop_recording(void)
{
    // 如果未在录音，直接返回
    if (!s_ctx.recording) return ESP_OK;

    ESP_LOGI(TAG, "⏹️ 停止录音");
    s_ctx.recording = false;

    return ESP_OK;
}

/**
 * @brief 播放音频数据
 * 
 * 将 PCM 音频数据写入播放缓冲区，等待播放。
 * 
 * @param pcm_data PCM 音频数据指针
 * @param sample_count 采样点数
 * @return 
 *     - ESP_OK: 写入成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_play_audio(const int16_t *pcm_data, size_t sample_count)
{
    // 参数检查
    if (!s_ctx.initialized || !pcm_data || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 写入播放缓冲区
    return playback_controller_write(s_ctx.playback_ctrl, pcm_data, sample_count);
}

size_t audio_manager_get_playback_free_space(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized || !s_ctx.playback_ctrl) {
        return 0;
    }
    
    return playback_controller_get_free_space(s_ctx.playback_ctrl);
}

/**
 * @brief 启动播放
 * 
 * 启动播放控制器，开始播放缓冲区中的音频数据。
 * 
 * @return 
 *     - ESP_OK: 启动成功
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_start_playback(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    return playback_controller_start(s_ctx.playback_ctrl);
}

/**
 * @brief 停止播放
 * 
 * 停止播放控制器，不再播放音频。
 * 
 * @return ESP_OK: 停止成功
 */
esp_err_t audio_manager_stop_playback(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_OK;

    return playback_controller_stop(s_ctx.playback_ctrl);
}

/**
 * @brief 清空播放缓冲区
 * 
 * 清空播放缓冲区中的所有待播放数据。
 * 
 * @return 
 *     - ESP_OK: 清空成功
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_clear_playback_buffer(void)
{
    // 检查是否已初始化
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    return playback_controller_clear(s_ctx.playback_ctrl);
}

/**
 * @brief 设置音量
 * 
 * 设置播放音量，范围 0-100。
 * 
 * @param volume 音量值（0-100）
 */
void audio_manager_set_volume(uint8_t volume)
{
    // 限制音量范围
    if (volume > 100) volume = 100;
    s_ctx.volume = volume;
    ESP_LOGI(TAG, "🔊 音量: %d%%", volume);
}

/**
 * @brief 获取音量
 * 
 * 获取当前播放音量。
 * 
 * @return 音量值（0-100）
 */
uint8_t audio_manager_get_volume(void)
{
    return s_ctx.volume;
}

/**
 * @brief 更新唤醒词配置
 * 
 * 动态更新唤醒词检测的配置参数。
 * 
 * @param config 唤醒词配置参数
 * @return 
 *     - ESP_OK: 更新成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_update_wakeup_config(const audio_mgr_wakeup_config_t *config)
{
    // 参数检查
    if (!s_ctx.initialized || !config) return ESP_ERR_INVALID_ARG;

    // 更新配置
    memcpy(&s_ctx.config.wakeup_config, config, sizeof(audio_mgr_wakeup_config_t));
    
    // 构造 AFE 唤醒词配置
    afe_wakeup_config_t afe_wakeup = {
        .enabled = config->enabled,
        .wake_word_name = config->wake_word_name,
        .model_partition = config->model_partition,
        .sensitivity = config->sensitivity,
    };
    
    // 更新 AFE 配置
    return afe_wrapper_update_wakeup_config(s_ctx.afe_wrapper, &afe_wakeup);
}

/**
 * @brief 获取唤醒词配置
 * 
 * 获取当前唤醒词检测的配置参数。
 * 
 * @param config 输出参数，用于存储配置
 * @return 
 *     - ESP_OK: 获取成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t audio_manager_get_wakeup_config(audio_mgr_wakeup_config_t *config)
{
    // 参数检查
    if (!s_ctx.initialized || !config) return ESP_ERR_INVALID_ARG;

    // 复制配置
    memcpy(config, &s_ctx.config.wakeup_config, sizeof(audio_mgr_wakeup_config_t));

    return ESP_OK;
}

/**
 * @brief 检查是否正在运行
 * 
 * 检查音频监听是否正在运行。
 * 
 * @return true: 正在运行，false: 未运行
 */
bool audio_manager_is_running(void)
{
    return s_ctx.running;
}

/**
 * @brief 检查是否正在录音
 * 
 * 检查是否正在录音。
 * 
 * @return true: 正在录音，false: 未录音
 */
bool audio_manager_is_recording(void)
{
    return s_ctx.recording;
}

/**
 * @brief 检查是否正在播放
 * 
 * 检查是否正在播放音频。
 * 
 * @return true: 正在播放，false: 未播放
 */
bool audio_manager_is_playing(void)
{
    return playback_controller_is_running(s_ctx.playback_ctrl);
}

/**
 * @brief 设置录音回调函数
 * 
 * 设置录音数据回调函数，当有录音数据时，会调用此回调函数。
 * 
 * @param callback 回调函数指针
 * @param user_ctx 用户上下文指针
 */
void audio_manager_set_record_callback(audio_record_callback_t callback, void *user_ctx)
{
    s_ctx.record_callback = callback;
    s_ctx.record_ctx = user_ctx;
}
