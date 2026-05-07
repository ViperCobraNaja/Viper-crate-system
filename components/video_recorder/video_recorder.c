/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "video_recorder.h"
#include "storage_manager.h"

static const char *TAG = "video_recorder";

// ============================================================================
// 内部类型定义
// ============================================================================

/**
 * @brief 前触发环形缓冲区
 *
 * 在运动触发录像前持续缓存帧，触发后将这些帧写入文件开头
 */
typedef struct {
    uint8_t **frames;           ///< 帧数据指针数组
    uint32_t *frame_sizes;      ///< 各帧数据大小
    uint32_t *timestamps;       ///< 各帧时间戳
    motion_heatmap_t *heatmaps; ///< 各帧运动热度图
    uint32_t capacity;          ///< 缓冲区容量
    uint32_t head;              ///< 缓冲区头部索引（下一个写入位置）
    uint32_t count;             ///< 当前有效帧数
} pre_trigger_buffer_t;

/**
 * @brief 编码器状态
 */
typedef struct {
    bool high_resolution;       ///< true: 录像模式，false: 监控模式
    uint32_t width;             ///< 当前编码宽度
    uint32_t height;            ///< 当前编码高度
    uint32_t fps;               ///< 当前编码帧率
    uint32_t bitrate;           ///< 当前编码码率
    void *encoder_handle;       ///< H.264编码器句柄
} encoder_state_t;

/**
 * @brief 录像器内部结构
 */
typedef struct {
    video_record_config_t config;   ///< 录像配置
    video_record_state_t state;     ///< 当前状态
    char current_reason[32];        ///< 录像触发原因

    motion_detector_handle_t motion_detector;

    uint32_t start_time_ms;         ///< 录像开始时间
    uint32_t current_duration_ms;   ///< 当前录制时长
    uint32_t frame_count;           ///< 已录制帧数
    uint32_t motion_trigger_time;   ///< 运动触发时间
    uint32_t no_motion_start_time;  ///< 无运动开始时间

    char current_filename[64];      ///< 当前录像文件名
    uint32_t current_file_size;     ///< 当前文件大小
    uint32_t sequence_number;       ///< 当日录像序号

    pre_trigger_buffer_t pre_trigger;   ///< 前触发缓冲区
    encoder_state_t encoder;            ///< 编码器状态

    SemaphoreHandle_t lock;         ///< 互斥锁
} video_recorder_t;

// ============================================================================
// 默认配置
// ============================================================================

static const video_record_config_t default_config = {
    .monitor = {
        .width = 320,
        .height = 240,
        .fps = 10,
        .bitrate = 512000,
        .format = 0  // RGB565
    },
    .recording = {
        .width = 1280,
        .height = 720,
        .fps = 15,
        .bitrate = 2000000,
        .max_duration_ms = 5 * 60 * 1000,  // 5分钟
        .post_trigger_ms = 3000            // 3秒
    },
    .storage = {
        .base_path = "/sdcard/videos",
        .max_files = 100,
        .max_storage_mb = 4096,  // 4GB
        .auto_delete = true
    },
    .motion_config = {
        .threshold = 30,
        .min_area = 5,
        .cooldown_ms = 5000,
        .trigger_frames = 3,
        .pre_trigger_frames = 10,
        .enable = true,
        .grid_cols = MOTION_GRID_COLS,
        .grid_rows = MOTION_GRID_ROWS
    }
};

// ============================================================================
// 内部辅助函数
// ============================================================================

/**
 * @brief 生成ISO8601格式时间戳字符串
 */
static void get_timestamp_string(char *buffer, size_t size)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(buffer, size, "%Y%m%d_%H%M%S", &timeinfo);
}

/**
 * @brief 生成唯一文件名
 *
 * 格式: YYYYMMDD_HHMMSS_<reason>_<seq>.mp4
 * 序列号使用实例级计数器，避免静态变量线程安全问题
 */
static void generate_filename(video_recorder_t *recorder, const char *reason, char *filename, size_t size)
{
    char timestamp[32];
    get_timestamp_string(timestamp, sizeof(timestamp));

    recorder->sequence_number++;
    snprintf(filename, size, "%s_%s_%03lu.mp4", timestamp, reason, (unsigned long)recorder->sequence_number);
}

/**
 * @brief 初始化前触发缓冲区
 */
static esp_err_t init_pre_trigger_buffer(video_recorder_t *recorder)
{
    uint32_t capacity = recorder->config.motion_config.pre_trigger_frames;

    recorder->pre_trigger.frames = calloc(capacity, sizeof(uint8_t *));
    recorder->pre_trigger.frame_sizes = calloc(capacity, sizeof(uint32_t));
    recorder->pre_trigger.timestamps = calloc(capacity, sizeof(uint32_t));
    recorder->pre_trigger.heatmaps = calloc(capacity, sizeof(motion_heatmap_t));

    if (!recorder->pre_trigger.frames ||
        !recorder->pre_trigger.frame_sizes ||
        !recorder->pre_trigger.timestamps ||
        !recorder->pre_trigger.heatmaps) {
        ESP_LOGE(TAG, "Failed to allocate pre-trigger buffer");
        return ESP_ERR_NO_MEM;
    }

    recorder->pre_trigger.capacity = capacity;
    recorder->pre_trigger.head = 0;
    recorder->pre_trigger.count = 0;

    return ESP_OK;
}

/**
 * @brief 保存帧到前触发环形缓冲区
 *
 * 缓冲区满时覆盖最旧的帧（FIFO循环覆盖）
 */
static esp_err_t save_to_pre_trigger_buffer(
    video_recorder_t *recorder,
    uint8_t *frame_data,
    size_t frame_size,
    uint32_t timestamp,
    motion_heatmap_t *heatmap)
{
    pre_trigger_buffer_t *buf = &recorder->pre_trigger;

    if (buf->count >= buf->capacity) {
        // 缓冲区已满，覆盖head位置（最旧的帧）
        free(buf->frames[buf->head]);

        buf->frames[buf->head] = malloc(frame_size);
        if (!buf->frames[buf->head]) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(buf->frames[buf->head], frame_data, frame_size);
        buf->frame_sizes[buf->head] = frame_size;
        buf->timestamps[buf->head] = timestamp;

        if (heatmap) {
            memcpy(&buf->heatmaps[buf->head], heatmap, sizeof(motion_heatmap_t));
        }

        buf->head = (buf->head + 1) % buf->capacity;
    } else {
        // 缓冲区未满，追加到末尾
        uint32_t write_idx = (buf->head + buf->count) % buf->capacity;

        buf->frames[write_idx] = malloc(frame_size);
        if (!buf->frames[write_idx]) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(buf->frames[write_idx], frame_data, frame_size);
        buf->frame_sizes[write_idx] = frame_size;
        buf->timestamps[write_idx] = timestamp;

        if (heatmap) {
            memcpy(&buf->heatmaps[write_idx], heatmap, sizeof(motion_heatmap_t));
        }

        buf->count++;
    }

    return ESP_OK;
}

/**
 * @brief 清空前触发缓冲区，释放所有已分配帧内存
 */
static void clear_pre_trigger_buffer(video_recorder_t *recorder)
{
    pre_trigger_buffer_t *buf = &recorder->pre_trigger;

    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (buf->head + i) % buf->capacity;
        if (buf->frames[idx]) {
            free(buf->frames[idx]);
            buf->frames[idx] = NULL;
        }
    }

    buf->head = 0;
    buf->count = 0;
}

// ============================================================================
// 初始化与销毁函数
// ============================================================================

video_recorder_handle_t video_recorder_init(const video_record_config_t *config)
{
    video_recorder_t *recorder = calloc(1, sizeof(video_recorder_t));
    if (!recorder) {
        ESP_LOGE(TAG, "Failed to allocate video recorder");
        return NULL;
    }

    // 复制配置
    if (config) {
        memcpy(&recorder->config, config, sizeof(video_record_config_t));
    } else {
        memcpy(&recorder->config, &default_config, sizeof(video_record_config_t));
    }

    // 初始化状态
    recorder->state = RECORD_STATE_IDLE;
    recorder->start_time_ms = 0;
    recorder->current_duration_ms = 0;
    recorder->frame_count = 0;
    recorder->motion_trigger_time = 0;
    recorder->no_motion_start_time = 0;
    recorder->current_filename[0] = '\0';
    recorder->current_file_size = 0;

    // 初始化运动检测器
    recorder->motion_detector = motion_detector_init(&recorder->config.motion_config);
    if (!recorder->motion_detector) {
        ESP_LOGE(TAG, "Failed to initialize motion detector");
        free(recorder);
        return NULL;
    }

    // 初始化前触发缓冲区
    if (init_pre_trigger_buffer(recorder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize pre-trigger buffer");
        motion_detector_deinit(recorder->motion_detector);
        free(recorder);
        return NULL;
    }

    // 初始化编码器状态（默认为监控模式）
    recorder->encoder.high_resolution = false;
    recorder->encoder.width = recorder->config.monitor.width;
    recorder->encoder.height = recorder->config.monitor.height;
    recorder->encoder.fps = recorder->config.monitor.fps;
    recorder->encoder.bitrate = recorder->config.monitor.bitrate;
    recorder->encoder.encoder_handle = NULL;

    // 创建互斥锁
    recorder->lock = xSemaphoreCreateMutex();
    if (!recorder->lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        clear_pre_trigger_buffer(recorder);
        motion_detector_deinit(recorder->motion_detector);
        free(recorder);
        return NULL;
    }

    ESP_LOGI(TAG, "Video recorder initialized in monitor mode: %dx%d@%dfps",
             recorder->encoder.width, recorder->encoder.height, recorder->encoder.fps);

    return (video_recorder_handle_t)recorder;
}

esp_err_t video_recorder_deinit(video_recorder_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    // 如果正在录像，先停止
    if (recorder->state == RECORD_STATE_RECORDING ||
        recorder->state == RECORD_STATE_POST_RECORDING) {
        video_recorder_stop(handle);
    }

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    // 释放运动检测器
    if (recorder->motion_detector) {
        motion_detector_deinit(recorder->motion_detector);
    }

    // 清空前触发缓冲区
    clear_pre_trigger_buffer(recorder);
    free(recorder->pre_trigger.frames);
    free(recorder->pre_trigger.frame_sizes);
    free(recorder->pre_trigger.timestamps);
    free(recorder->pre_trigger.heatmaps);

    // 删除互斥锁
    if (recorder->lock) {
        vSemaphoreDelete(recorder->lock);
    }

    free(recorder);
    return ESP_OK;
}

// ============================================================================
// 录像控制函数
// ============================================================================

esp_err_t video_recorder_start(video_recorder_handle_t handle, const char *reason)
{
    if (!handle || !reason) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    if (recorder->state != RECORD_STATE_IDLE) {
        ESP_LOGW(TAG, "Recorder not idle, current state: %d", recorder->state);
        xSemaphoreGive(recorder->lock);
        return ESP_ERR_INVALID_STATE;
    }

    // 切换到录像模式
    recorder->encoder.high_resolution = true;
    recorder->encoder.width = recorder->config.recording.width;
    recorder->encoder.height = recorder->config.recording.height;
    recorder->encoder.fps = recorder->config.recording.fps;
    recorder->encoder.bitrate = recorder->config.recording.bitrate;

    // 生成文件名
    generate_filename(recorder, reason, recorder->current_filename, sizeof(recorder->current_filename));
    strncpy(recorder->current_reason, reason, sizeof(recorder->current_reason) - 1);

    // 创建录像文件
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s",
             recorder->config.storage.base_path, recorder->current_filename);

    // TODO: 调用存储管理器创建文件
    ESP_LOGI(TAG, "Creating video file: %s", filepath);

    // 更新状态
    recorder->state = RECORD_STATE_PRE_RECORDING;
    recorder->start_time_ms = esp_timer_get_time() / 1000;
    recorder->current_duration_ms = 0;
    recorder->frame_count = 0;
    recorder->current_file_size = 0;

    // 写入前触发缓冲区数据
    if (recorder->pre_trigger.count > 0) {
        ESP_LOGI(TAG, "Writing %" PRIu32 " pre-trigger frames", recorder->pre_trigger.count);

        for (uint32_t i = 0; i < recorder->pre_trigger.count; i++) {
            uint32_t idx = (recorder->pre_trigger.head + i) % recorder->pre_trigger.capacity;
            // TODO: 将前触发帧写入文件
            ESP_LOGD(TAG, "Pre-trigger frame %" PRIu32 ": %" PRIu32 " bytes",
                     i, recorder->pre_trigger.frame_sizes[idx]);
        }

        clear_pre_trigger_buffer(recorder);
    }

    recorder->state = RECORD_STATE_RECORDING;
    recorder->no_motion_start_time = 0;

    ESP_LOGI(TAG, "Recording started: %s, mode: %" PRIu32 "x%" PRIu32 "@%" PRIu32 "fps",
             recorder->current_filename, recorder->encoder.width,
             recorder->encoder.height, recorder->encoder.fps);

    xSemaphoreGive(recorder->lock);
    return ESP_OK;
}

esp_err_t video_recorder_stop(video_recorder_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    if (recorder->state != RECORD_STATE_RECORDING &&
        recorder->state != RECORD_STATE_POST_RECORDING) {
        ESP_LOGW(TAG, "Not recording, current state: %d", recorder->state);
        xSemaphoreGive(recorder->lock);
        return ESP_ERR_INVALID_STATE;
    }

    // 切换到监控模式
    recorder->encoder.high_resolution = false;
    recorder->encoder.width = recorder->config.monitor.width;
    recorder->encoder.height = recorder->config.monitor.height;
    recorder->encoder.fps = recorder->config.monitor.fps;
    recorder->encoder.bitrate = recorder->config.monitor.bitrate;

    // 完成文件写入
    recorder->state = RECORD_STATE_WRITING_FILE;

    // TODO: 关闭录像文件，写入元数据
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s",
             recorder->config.storage.base_path, recorder->current_filename);

    ESP_LOGI(TAG, "Finalizing video file: %s, duration: %" PRIu32 "ms, frames: %" PRIu32,
             filepath, recorder->current_duration_ms, recorder->frame_count);

    // 生成元数据
    video_metadata_t metadata = {
        .duration_ms = recorder->current_duration_ms,
        .width = recorder->config.recording.width,
        .height = recorder->config.recording.height,
        .fps = recorder->config.recording.fps,
        .bitrate = recorder->config.recording.bitrate,
        .frame_count = recorder->frame_count,
        .avg_motion_level = 0.0f,  // TODO: 计算平均运动强度
        .file_size = recorder->current_file_size
    };

    	snprintf(metadata.filename, sizeof(metadata.filename), "%s", recorder->current_filename);
    get_timestamp_string(metadata.create_time, sizeof(metadata.create_time));

    // TODO: 保存元数据到文件
    // storage_manager_save_metadata(filepath, &metadata);

    // 重置状态
    recorder->state = RECORD_STATE_IDLE;
    recorder->current_filename[0] = '\0';
    recorder->current_reason[0] = '\0';
    recorder->start_time_ms = 0;
    recorder->current_duration_ms = 0;
    recorder->frame_count = 0;
    recorder->current_file_size = 0;
    recorder->no_motion_start_time = 0;

    // 重置运动检测器
    motion_detector_reset(recorder->motion_detector);

    ESP_LOGI(TAG, "Recording stopped, returning to monitor mode: %" PRIu32 "x%" PRIu32 "@%" PRIu32 "fps",
             recorder->encoder.width, recorder->encoder.height, recorder->encoder.fps);

    xSemaphoreGive(recorder->lock);
    return ESP_OK;
}

esp_err_t video_recorder_manual_trigger(video_recorder_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    if (recorder->state != RECORD_STATE_IDLE) {
        ESP_LOGW(TAG, "Already recording");
        xSemaphoreGive(recorder->lock);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(recorder->lock);
    return video_recorder_start(handle, "manual");
}

// ============================================================================
// 帧处理函数
// ============================================================================

esp_err_t video_recorder_process_frame(
    video_recorder_handle_t handle,
    uint8_t *frame_data,
    size_t frame_size,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp)
{
    if (!handle || !frame_data) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    uint32_t current_time = esp_timer_get_time() / 1000;

    if (recorder->state == RECORD_STATE_RECORDING) {
        recorder->current_duration_ms = current_time - recorder->start_time_ms;
        recorder->frame_count++;

        if (recorder->current_duration_ms > recorder->config.recording.max_duration_ms) {
            ESP_LOGW(TAG, "Recording timeout reached, stopping");
            xSemaphoreGive(recorder->lock);
            return video_recorder_stop(handle);
        }

        // TODO: 编码并写入帧到文件
        recorder->current_file_size += frame_size;

        ESP_LOGD(TAG, "Recording frame %" PRIu32 ", duration: %" PRIu32 "ms, size: %u bytes",
                 recorder->frame_count, recorder->current_duration_ms, (unsigned)recorder->current_file_size);

    } else if (recorder->state == RECORD_STATE_IDLE) {
        motion_heatmap_t heatmap;
        bool motion_triggered = false;

        motion_detector_check_trigger(recorder->motion_detector, &motion_triggered, &heatmap);

        if (motion_triggered) {
            ESP_LOGI(TAG, "Motion triggered, starting recording");
            save_to_pre_trigger_buffer(recorder, frame_data, frame_size, timestamp, &heatmap);
            xSemaphoreGive(recorder->lock);
            return video_recorder_start(handle, "motion");
        } else {
            save_to_pre_trigger_buffer(recorder, frame_data, frame_size, timestamp, &heatmap);
        }
    }

    xSemaphoreGive(recorder->lock);
    return ESP_OK;
}

esp_err_t video_recorder_process_motion_vectors(
    video_recorder_handle_t handle,
    motion_vector_t *vectors,
    uint32_t vector_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp)
{
    if (!handle || !vectors) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    esp_err_t ret = motion_detector_process_vectors(
        recorder->motion_detector, vectors, vector_count, width, height, timestamp);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process motion vectors: %d", ret);
        return ret;
    }

    bool motion_triggered = false;
    motion_heatmap_t heatmap;

    ret = motion_detector_check_trigger(recorder->motion_detector, &motion_triggered, &heatmap);
    if (ret != ESP_OK) {
        return ret;
    }

    if (motion_triggered && recorder->state == RECORD_STATE_IDLE) {
        ESP_LOGI(TAG, "Motion triggered from hardware vectors, starting recording");
        return video_recorder_start(handle, "motion_hw");
    }

    return ESP_OK;
}

esp_err_t video_recorder_process_hw_motion_vectors(
    video_recorder_handle_t handle,
    const uint32_t *mv_data,
    uint32_t data_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp)
{
    if (!handle || !mv_data) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    esp_err_t ret = motion_detector_process_hw_mv_data(
        recorder->motion_detector, mv_data, data_count, width, height, timestamp);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process HW motion vectors: %d", ret);
        return ret;
    }

    bool motion_triggered = false;
    motion_heatmap_t heatmap;

    ret = motion_detector_check_trigger(recorder->motion_detector, &motion_triggered, &heatmap);
    if (ret != ESP_OK) {
        return ret;
    }

    if (motion_triggered && recorder->state == RECORD_STATE_IDLE) {
        ESP_LOGI(TAG, "Motion triggered from HW encoder MVs, starting recording");
        return video_recorder_start(handle, "motion_hw");
    }

    return ESP_OK;
}

// ============================================================================
// 状态查询函数
// ============================================================================

esp_err_t video_recorder_get_status(
    video_recorder_handle_t handle,
    video_record_state_t *state,
    uint32_t *current_duration_ms,
    char *reason)
{
    if (!handle || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    *state = recorder->state;

    if (current_duration_ms) {
        *current_duration_ms = recorder->current_duration_ms;
    }

    if (reason) {
        strncpy(reason, recorder->current_reason, 31);
        reason[31] = '\0';
    }

    xSemaphoreGive(recorder->lock);
    return ESP_OK;
}

// ============================================================================
// 模式控制函数
// ============================================================================

esp_err_t video_recorder_switch_mode(
    video_recorder_handle_t handle,
    bool high_resolution)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    video_recorder_t *recorder = (video_recorder_t *)handle;

    xSemaphoreTake(recorder->lock, portMAX_DELAY);

    if (recorder->state != RECORD_STATE_IDLE) {
        ESP_LOGW(TAG, "Cannot switch mode while recording");
        xSemaphoreGive(recorder->lock);
        return ESP_ERR_INVALID_STATE;
    }

    recorder->encoder.high_resolution = high_resolution;

    if (high_resolution) {
        recorder->encoder.width = recorder->config.recording.width;
        recorder->encoder.height = recorder->config.recording.height;
        recorder->encoder.fps = recorder->config.recording.fps;
        recorder->encoder.bitrate = recorder->config.recording.bitrate;
        ESP_LOGI(TAG, "Switched to recording mode: %" PRIu32 "x%" PRIu32 "@%" PRIu32 "fps",
                 recorder->encoder.width, recorder->encoder.height, recorder->encoder.fps);
    } else {
        recorder->encoder.width = recorder->config.monitor.width;
        recorder->encoder.height = recorder->config.monitor.height;
        recorder->encoder.fps = recorder->config.monitor.fps;
        recorder->encoder.bitrate = recorder->config.monitor.bitrate;
        ESP_LOGI(TAG, "Switched to monitor mode: %" PRIu32 "x%" PRIu32 "@%" PRIu32 "fps",
                 recorder->encoder.width, recorder->encoder.height, recorder->encoder.fps);
    }

    xSemaphoreGive(recorder->lock);
    return ESP_OK;
}

// ============================================================================
// 文件管理函数
// ============================================================================

esp_err_t video_recorder_list_files(
    video_recorder_handle_t handle,
    video_metadata_t *file_list,
    int max_files,
    int *file_count)
{
    // TODO: 实现文件列表获取
    if (file_count) {
        *file_count = 0;
    }
    return ESP_OK;
}

esp_err_t video_recorder_get_metadata(
    video_recorder_handle_t handle,
    const char *filename,
    video_metadata_t *metadata)
{
    // TODO: 实现元数据获取
    return ESP_OK;
}

esp_err_t video_recorder_delete_file(
    video_recorder_handle_t handle,
    const char *filename)
{
    // TODO: 实现文件删除
    return ESP_OK;
}
