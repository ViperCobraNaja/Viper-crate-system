/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_h264_enc_param_hw.h"
#include "motion_detector.h"

static const char *TAG = "motion_detector";

// 运动检测器内部结构
typedef struct {
    motion_detect_config_t config;
    motion_state_t state;
    motion_heatmap_t current_heatmap;
    motion_heatmap_t *heatmap_history;
    uint32_t history_size;
    uint32_t history_index;

    // 触发判断状态
    uint32_t motion_frame_count;
    uint32_t no_motion_frame_count;
    uint32_t last_trigger_time;
    uint32_t cooldown_end_time;
    bool trigger_pending;         /* TRIGGERED 已置位但尚未被 check_trigger 消费 */

    // 前触发缓冲区
    uint8_t **pre_trigger_frames;
    uint32_t *frame_sizes;
    motion_heatmap_t *pre_trigger_heatmaps;
    uint32_t pre_trigger_index;
    uint32_t pre_trigger_count;

    // 同步锁
    SemaphoreHandle_t lock;
    size_t frame_buffer_size;
} motion_detector_t;

// 默认配置
static const motion_detect_config_t default_config = {
    .threshold = 30,
    .min_area = 5,
    .cooldown_ms = 5000,
    .trigger_frames = 3,
    .pre_trigger_frames = 10,
    .enable = true,
    .grid_cols = MOTION_GRID_COLS,
    .grid_rows = MOTION_GRID_ROWS
};

motion_detector_handle_t motion_detector_init(const motion_detect_config_t *config)
{
    motion_detector_t *detector = calloc(1, sizeof(motion_detector_t));
    if (!detector) {
        ESP_LOGE(TAG, "Failed to allocate motion detector");
        return NULL;
    }

    // 复制配置
    if (config) {
        memcpy(&detector->config, config, sizeof(motion_detect_config_t));
    } else {
        memcpy(&detector->config, &default_config, sizeof(motion_detect_config_t));
    }

    // 初始化状态
    detector->state = MOTION_STATE_IDLE;
    detector->motion_frame_count = 0;
    detector->no_motion_frame_count = 0;
    detector->last_trigger_time = 0;
    detector->cooldown_end_time = 0;
    detector->trigger_pending = false;

    // 分配热度图历史缓冲区
    detector->history_size = detector->config.pre_trigger_frames;
    detector->heatmap_history = calloc(detector->history_size, sizeof(motion_heatmap_t));
    if (!detector->heatmap_history) {
        ESP_LOGE(TAG, "Failed to allocate heatmap history");
        free(detector);
        return NULL;
    }

    // 分配前触发帧缓冲区
    detector->pre_trigger_frames = NULL;
    detector->frame_sizes = NULL;
    detector->pre_trigger_heatmaps = NULL;
    detector->pre_trigger_index = 0;
    detector->pre_trigger_count = 0;

    // 创建互斥锁
    detector->lock = xSemaphoreCreateMutex();
    if (!detector->lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(detector->heatmap_history);
        free(detector);
        return NULL;
    }

    ESP_LOGI(TAG, "Motion detector initialized with threshold=%d, pre_trigger=%d frames",
             detector->config.threshold, detector->config.pre_trigger_frames);

    return (motion_detector_handle_t)detector;
}

// 计算网格索引
static inline uint8_t calculate_grid_index(uint32_t macroblock_x, uint32_t macroblock_y,
                                          uint32_t width_mb, uint32_t height_mb,
                                          uint8_t grid_cols, uint8_t grid_rows)
{
    // 将宏块坐标映射到网格
    uint8_t grid_x = (macroblock_x * grid_cols) / width_mb;
    uint8_t grid_y = (macroblock_y * grid_rows) / height_mb;

    // 确保在范围内
    if (grid_x >= grid_cols) grid_x = grid_cols - 1;
    if (grid_y >= grid_rows) grid_y = grid_rows - 1;

    return grid_y * grid_cols + grid_x;
}

// 更新热度图
static void update_heatmap(motion_detector_t *detector, motion_vector_t *vectors,
                          uint32_t vector_count, uint32_t width_mb, uint32_t height_mb,
                          uint32_t timestamp)
{
    motion_heatmap_t *heatmap = &detector->current_heatmap;

    // 重置热度图
    memset(heatmap->grid_motion, 0, sizeof(heatmap->grid_motion));
    memset(heatmap->grid_count, 0, sizeof(heatmap->grid_count));
    heatmap->total_motion = 0;
    heatmap->total_valid = 0;

    // 处理每个运动矢量
    for (uint32_t i = 0; i < vector_count; i++) {
        if (vectors[i].valid) {
            // 计算运动强度
            uint32_t motion = abs(vectors[i].dx) + abs(vectors[i].dy);

            // 计算宏块坐标
            uint32_t mb_x = i % width_mb;
            uint32_t mb_y = i / width_mb;

            // 更新网格
            uint8_t grid_idx = calculate_grid_index(mb_x, mb_y, width_mb, height_mb,
                                                   detector->config.grid_cols, detector->config.grid_rows);

            heatmap->grid_motion[grid_idx] += motion;
            heatmap->grid_count[grid_idx]++;
            heatmap->total_motion += motion;
            heatmap->total_valid++;
        }
    }

    // 计算平均运动强度
    if (heatmap->total_valid > 0) {
        heatmap->avg_motion = (float)heatmap->total_motion / heatmap->total_valid;
    } else {
        heatmap->avg_motion = 0.0f;
    }

    heatmap->frame_timestamp = timestamp;

    // 找出最热的4个区域
    uint32_t max_motion[4] = {0};
    uint8_t max_indices[4] = {0};

    for (uint8_t i = 0; i < MOTION_GRID_SIZE; i++) {
        uint32_t grid_motion = heatmap->grid_motion[i];

        // 插入排序找到最大的4个
        for (uint8_t j = 0; j < 4; j++) {
            if (grid_motion > max_motion[j]) {
                // 向后移动
                for (uint8_t k = 3; k > j; k--) {
                    max_motion[k] = max_motion[k-1];
                    max_indices[k] = max_indices[k-1];
                }
                max_motion[j] = grid_motion;
                max_indices[j] = i;
                break;
            }
        }
    }

    memcpy(heatmap->hot_zones, max_indices, sizeof(heatmap->hot_zones));
}

esp_err_t motion_detector_process_vectors(
    motion_detector_handle_t handle,
    motion_vector_t *vectors,
    uint32_t vector_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp)
{
    if (!handle || !vectors) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);

    // 检查冷却时间
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (detector->state == MOTION_STATE_COOLDOWN && current_time < detector->cooldown_end_time) {
        xSemaphoreGive(detector->lock);
        return ESP_OK;
    }

    // 计算宏块尺寸
    uint32_t width_mb = width / 16;
    uint32_t height_mb = height / 16;

    // 更新热度图
    update_heatmap(detector, vectors, vector_count, width_mb, height_mb, timestamp);

    // 保存到历史记录
    uint32_t history_index = detector->history_index % detector->history_size;
    memcpy(&detector->heatmap_history[history_index], &detector->current_heatmap, sizeof(motion_heatmap_t));
    detector->history_index++;

    // 检查运动触发
    bool motion_detected = (detector->current_heatmap.avg_motion > detector->config.threshold);

    if (motion_detected) {
        detector->motion_frame_count++;
        detector->no_motion_frame_count = 0;

        if (detector->state == MOTION_STATE_IDLE &&
            detector->motion_frame_count >= detector->config.trigger_frames) {
            detector->state = MOTION_STATE_TRIGGERED;
            detector->last_trigger_time = current_time;
            detector->trigger_pending = true;
            ESP_LOGI(TAG, "Motion triggered! Avg motion: %.2f", detector->current_heatmap.avg_motion);
        } else if (detector->state == MOTION_STATE_IDLE) {
            detector->state = MOTION_STATE_DETECTED;
        }
    } else {
        detector->no_motion_frame_count++;
        detector->motion_frame_count = 0;

        if (detector->state == MOTION_STATE_TRIGGERED || detector->state == MOTION_STATE_DETECTED) {
            detector->state = MOTION_STATE_IDLE;
        }
    }

    // 如果已触发且仍在冷却期外，进入冷却
    if (detector->state == MOTION_STATE_TRIGGERED) {
        detector->cooldown_end_time = current_time + detector->config.cooldown_ms;
        detector->state = MOTION_STATE_COOLDOWN;
    }

    xSemaphoreGive(detector->lock);
    return ESP_OK;
}

// 从硬件运动矢量更新热度图（稀疏数据，已含位置信息）
static void update_heatmap_from_hw_mv(motion_detector_t *detector,
                                       const uint32_t *mv_data,
                                       uint32_t data_count,
                                       uint32_t width_mb, uint32_t height_mb,
                                       uint32_t timestamp)
{
    motion_heatmap_t *heatmap = &detector->current_heatmap;

    memset(heatmap->grid_motion, 0, sizeof(heatmap->grid_motion));
    memset(heatmap->grid_count, 0, sizeof(heatmap->grid_count));
    heatmap->total_motion = 0;
    heatmap->total_valid = 0;

    for (uint32_t i = 0; i < data_count; i++) {
        esp_h264_enc_mv_data_t mv = { .data = mv_data[i] };

        if (mv.mb_x >= width_mb || mv.mb_y >= height_mb) continue;

        uint32_t motion = abs(mv.mv_x) + abs(mv.mv_y);
        if (motion == 0) continue;

        uint8_t grid_idx = calculate_grid_index(mv.mb_x, mv.mb_y, width_mb, height_mb,
                                                 detector->config.grid_cols, detector->config.grid_rows);

        heatmap->grid_motion[grid_idx] += motion;
        heatmap->grid_count[grid_idx]++;
        heatmap->total_motion += motion;
        heatmap->total_valid++;
    }

    if (heatmap->total_valid > 0) {
        heatmap->avg_motion = (float)heatmap->total_motion / heatmap->total_valid;
    } else {
        heatmap->avg_motion = 0.0f;
    }
    heatmap->frame_timestamp = timestamp;

    // 找出最热的4个区域
    uint32_t max_motion[4] = {0};
    uint8_t max_indices[4] = {0};
    for (uint8_t i = 0; i < MOTION_GRID_SIZE; i++) {
        uint32_t grid_motion = heatmap->grid_motion[i];
        for (uint8_t j = 0; j < 4; j++) {
            if (grid_motion > max_motion[j]) {
                for (uint8_t k = 3; k > j; k--) {
                    max_motion[k] = max_motion[k-1];
                    max_indices[k] = max_indices[k-1];
                }
                max_motion[j] = grid_motion;
                max_indices[j] = i;
                break;
            }
        }
    }
    memcpy(heatmap->hot_zones, max_indices, sizeof(heatmap->hot_zones));
}

esp_err_t motion_detector_process_hw_mv_data(
    motion_detector_handle_t handle,
    const uint32_t *mv_data,
    uint32_t data_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp)
{
    if (!handle || !mv_data) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);

    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (detector->state == MOTION_STATE_COOLDOWN && current_time < detector->cooldown_end_time) {
        xSemaphoreGive(detector->lock);
        return ESP_OK;
    }

    uint32_t width_mb = width / 16;
    uint32_t height_mb = height / 16;

    update_heatmap_from_hw_mv(detector, mv_data, data_count, width_mb, height_mb, timestamp);

    uint32_t history_index = detector->history_index % detector->history_size;
    memcpy(&detector->heatmap_history[history_index], &detector->current_heatmap, sizeof(motion_heatmap_t));
    detector->history_index++;

    bool motion_detected = (detector->current_heatmap.avg_motion > detector->config.threshold);

    if (motion_detected) {
        detector->motion_frame_count++;
        detector->no_motion_frame_count = 0;

        if (detector->state == MOTION_STATE_IDLE &&
            detector->motion_frame_count >= detector->config.trigger_frames) {
            detector->state = MOTION_STATE_TRIGGERED;
            detector->last_trigger_time = current_time;
            detector->trigger_pending = true;
            ESP_LOGI(TAG, "Motion triggered! Avg motion: %.2f", detector->current_heatmap.avg_motion);
        } else if (detector->state == MOTION_STATE_IDLE) {
            detector->state = MOTION_STATE_DETECTED;
        }
    } else {
        detector->no_motion_frame_count++;
        detector->motion_frame_count = 0;

        if (detector->state == MOTION_STATE_TRIGGERED || detector->state == MOTION_STATE_DETECTED) {
            detector->state = MOTION_STATE_IDLE;
        }
    }

    if (detector->state == MOTION_STATE_TRIGGERED) {
        detector->cooldown_end_time = current_time + detector->config.cooldown_ms;
        detector->state = MOTION_STATE_COOLDOWN;
    }

    xSemaphoreGive(detector->lock);
    return ESP_OK;
}

esp_err_t motion_detector_check_trigger(
    motion_detector_handle_t handle,
    bool *triggered,
    motion_heatmap_t *heatmap)
{
    if (!handle || !triggered) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);

    *triggered = (detector->state == MOTION_STATE_TRIGGERED) || detector->trigger_pending;
    if (*triggered) {
        detector->trigger_pending = false;
    }

    if (heatmap) {
        memcpy(heatmap, &detector->current_heatmap, sizeof(motion_heatmap_t));
    }

    xSemaphoreGive(detector->lock);
    return ESP_OK;
}

esp_err_t motion_detector_get_status(
    motion_detector_handle_t handle,
    motion_state_t *state,
    uint8_t *motion_level)
{
    if (!handle || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);

    *state = detector->state;

    if (motion_level) {
        // 将平均运动强度转换为0-100级别
        float scaled_motion = detector->current_heatmap.avg_motion;
        if (scaled_motion > 100.0f) scaled_motion = 100.0f;
        *motion_level = (uint8_t)scaled_motion;
    }

    xSemaphoreGive(detector->lock);
    return ESP_OK;
}

esp_err_t motion_detector_reset(motion_detector_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);

    detector->state = MOTION_STATE_IDLE;
    detector->motion_frame_count = 0;
    detector->no_motion_frame_count = 0;
    detector->last_trigger_time = 0;
    detector->cooldown_end_time = 0;
    detector->trigger_pending = false;

    // 清空热度图
    memset(&detector->current_heatmap, 0, sizeof(motion_heatmap_t));

    xSemaphoreGive(detector->lock);
    return ESP_OK;
}

uint32_t motion_detector_get_pre_trigger_frames(motion_detector_handle_t handle)
{
    if (!handle) {
        return 0;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    xSemaphoreTake(detector->lock, portMAX_DELAY);
    uint32_t frames = detector->pre_trigger_count;
    xSemaphoreGive(detector->lock);

    return frames;
}

esp_err_t motion_detector_deinit(motion_detector_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    motion_detector_t *detector = (motion_detector_t *)handle;

    // 释放前触发帧缓冲区
    if (detector->pre_trigger_frames) {
        for (uint32_t i = 0; i < detector->pre_trigger_count; i++) {
            if (detector->pre_trigger_frames[i]) {
                free(detector->pre_trigger_frames[i]);
            }
        }
        free(detector->pre_trigger_frames);
        free(detector->frame_sizes);
        free(detector->pre_trigger_heatmaps);
    }

    // 释放热度图历史
    if (detector->heatmap_history) {
        free(detector->heatmap_history);
    }

    // 删除互斥锁
    if (detector->lock) {
        vSemaphoreDelete(detector->lock);
    }

    free(detector);
    return ESP_OK;
}