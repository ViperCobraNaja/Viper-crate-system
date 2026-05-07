/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 运动矢量结构（16×16宏块）
typedef struct {
    int16_t dx;      // 水平位移（-128到127）
    int16_t dy;      // 垂直位移（-128到127）
    uint8_t valid;   // 矢量是否有效（1有效，0无效）
} motion_vector_t;

// 运动热度图网格（8×6网格）
#define MOTION_GRID_COLS 8
#define MOTION_GRID_ROWS 6
#define MOTION_GRID_SIZE (MOTION_GRID_COLS * MOTION_GRID_ROWS)

typedef struct {
    uint32_t grid_motion[MOTION_GRID_SIZE];  // 每个网格的运动强度
    uint32_t grid_count[MOTION_GRID_SIZE];   // 每个网格的有效矢量数
    uint32_t total_motion;                   // 总运动强度
    uint32_t total_valid;                    // 总有效矢量数
    float avg_motion;                        // 平均运动强度
    uint8_t hot_zones[4];                    // 最热的4个区域索引
    uint32_t frame_timestamp;                // 时间戳（毫秒）
} motion_heatmap_t;

// 运动检测配置
typedef struct {
    uint8_t threshold;          // 运动检测阈值（0-100）
    uint32_t min_area;          // 最小运动区域面积（宏块数）
    uint32_t cooldown_ms;       // 冷却时间（毫秒）
    uint32_t trigger_frames;    // 触发所需连续帧数（默认3）
    uint32_t pre_trigger_frames;// 前触发缓冲帧数
    bool enable;                // 启用运动检测
    uint8_t grid_cols;          // 网格列数
    uint8_t grid_rows;          // 网格行数
} motion_detect_config_t;

// 运动检测状态
typedef enum {
    MOTION_STATE_IDLE = 0,      // 空闲
    MOTION_STATE_DETECTED,      // 检测到运动
    MOTION_STATE_TRIGGERED,     // 已触发
    MOTION_STATE_COOLDOWN       // 冷却中
} motion_state_t;

// 运动检测句柄
typedef void* motion_detector_handle_t;

/**
 * @brief 初始化运动检测器
 *
 * @param config 检测配置
 * @return motion_detector_handle_t 检测器句柄
 */
motion_detector_handle_t motion_detector_init(const motion_detect_config_t *config);

/**
 * @brief 处理运动矢量数据
 *
 * @param handle 检测器句柄
 * @param vectors 运动矢量数组
 * @param vector_count 矢量数量
 * @param width 图像宽度（宏块数）
 * @param height 图像高度（宏块数）
 * @param timestamp 时间戳（毫秒）
 * @return esp_err_t
 */
esp_err_t motion_detector_process_vectors(
    motion_detector_handle_t handle,
    motion_vector_t *vectors,
    uint32_t vector_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp
);

/**
 * @brief 检查是否触发录像
 *
 * @param handle 检测器句柄
 * @param triggered 是否触发
 * @param heatmap 运动热度图（可选）
 * @return esp_err_t
 */
esp_err_t motion_detector_check_trigger(
    motion_detector_handle_t handle,
    bool *triggered,
    motion_heatmap_t *heatmap
);

/**
 * @brief 获取当前运动状态
 *
 * @param handle 检测器句柄
 * @param state 运动状态
 * @param motion_level 运动强度（0-100）
 * @return esp_err_t
 */
esp_err_t motion_detector_get_status(
    motion_detector_handle_t handle,
    motion_state_t *state,
    uint8_t *motion_level
);

/**
 * @brief 重置运动检测器
 *
 * @param handle 检测器句柄
 * @return esp_err_t
 */
esp_err_t motion_detector_reset(motion_detector_handle_t handle);

/**
 * @brief 处理硬件 H.264 编码器输出的运动矢量数据
 *
 * 直接接收 esp_h264 硬件编码器输出的 esp_h264_enc_mv_data_t 数据（packed uint32_t 格式），
 * 每个条目包含 mb_x, mb_y, mv_x, mv_y 位域。仅包含非零运动矢量（稀疏数据）。
 * 调用本函数前无需调用 motion_detector_process_vectors()。
 *
 * @param handle 检测器句柄
 * @param mv_data 硬件运动矢量数据数组指针（每个元素为 packed uint32_t）
 * @param data_count 有效运动矢量条目数（从 esp_h264_enc_hw_get_mv_data_len 获取）
 * @param width 图像宽度（像素）
 * @param height 图像高度（像素）
 * @param timestamp 时间戳（毫秒）
 * @return esp_err_t
 */
esp_err_t motion_detector_process_hw_mv_data(
    motion_detector_handle_t handle,
    const uint32_t *mv_data,
    uint32_t data_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp
);

/**
 * @brief 获取前触发缓冲区帧数
 *
 * @param handle 检测器句柄
 * @return uint32_t 帧数
 */
uint32_t motion_detector_get_pre_trigger_frames(motion_detector_handle_t handle);

/**
 * @brief 释放运动检测器资源
 *
 * @param handle 检测器句柄
 * @return esp_err_t
 */
esp_err_t motion_detector_deinit(motion_detector_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // MOTION_DETECTOR_H