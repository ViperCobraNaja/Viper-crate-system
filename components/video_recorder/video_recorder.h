/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

#include "esp_err.h"
#include "motion_detector.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 录像状态枚举
// ============================================================================

/**
 * @brief 录像器状态枚举
 *
 * 录像器在不同阶段的运行状态
 */
typedef enum {
    RECORD_STATE_IDLE = 0,      ///< 空闲状态，未进行任何录制
    RECORD_STATE_PRE_RECORDING, ///< 预录制状态，前触发缓冲
    RECORD_STATE_RECORDING,     ///< 正在录制状态
    RECORD_STATE_POST_RECORDING,///< 后录制缓冲状态
    RECORD_STATE_WRITING_FILE,  ///< 文件写入状态
    RECORD_STATE_ERROR          ///< 错误状态
} video_record_state_t;

// ============================================================================
// 配置结构体
// ============================================================================

/**
 * @brief 监控模式配置
 */
typedef struct {
    uint32_t width;         ///< 图像宽度（默认320像素）
    uint32_t height;        ///< 图像高度（默认240像素）
    uint32_t fps;           ///< 帧率（默认10FPS）
    uint32_t bitrate;       ///< 码率（默认512kbps）
    uint8_t format;         ///< 像素格式（0: RGB565, 1: YUV420）
} video_monitor_config_t;

/**
 * @brief 录像模式配置
 */
typedef struct {
    uint32_t width;         ///< 图像宽度（默认1280像素）
    uint32_t height;        ///< 图像高度（默认720像素）
    uint32_t fps;           ///< 帧率（默认15FPS）
    uint32_t bitrate;       ///< 码率（默认2Mbps）
    uint32_t max_duration_ms; ///< 最大录制时长（默认300000ms，5分钟）
    uint32_t post_trigger_ms; ///< 后触发时长（默认3000ms，3秒）
} video_recording_config_t;

/**
 * @brief 存储配置
 */
typedef struct {
    char base_path[64];     ///< 文件存储基础路径
    uint32_t max_files;     ///< 最大文件数量限制
    uint32_t max_storage_mb;///< 最大存储空间（MB）
    bool auto_delete;       ///< 是否自动删除旧文件
} video_storage_config_t;

/**
 * @brief 录像器完整配置
 */
typedef struct {
    video_monitor_config_t monitor;    ///< 监控模式配置
    video_recording_config_t recording;///< 录像模式配置
    video_storage_config_t storage;    ///< 存储配置
    motion_detect_config_t motion_config; ///< 运动检测配置
} video_record_config_t;

// ============================================================================
// 元数据结构体
// ============================================================================

/**
 * @brief 录像文件元数据
 */
typedef struct {
    char filename[64];          ///< 文件名
    char create_time[20];       ///< ISO8601格式的创建时间
    uint32_t duration_ms;       ///< 录像时长（毫秒）
    uint32_t width;             ///< 视频宽度（像素）
    uint32_t height;            ///< 视频高度（像素）
    uint32_t fps;               ///< 视频帧率（FPS）
    uint32_t bitrate;           ///< 视频码率（bps）
    uint32_t frame_count;       ///< 总帧数
    float avg_motion_level;     ///< 平均运动强度
    motion_heatmap_t heatmap;   ///< 运动热度图数据
    uint32_t file_size;         ///< 文件大小（字节）
} video_metadata_t;

// ============================================================================
// 录像句柄类型
// ============================================================================

/**
 * @brief 录像器句柄（不透明指针）
 */
typedef void* video_recorder_handle_t;

// ============================================================================
// 初始化与销毁函数
// ============================================================================

/**
 * @brief 初始化视频录像器
 *
 * @param config 录像配置参数
 * @return video_recorder_handle_t 录像器句柄，失败返回NULL
 */
video_recorder_handle_t video_recorder_init(const video_record_config_t *config);

/**
 * @brief 释放录像器资源
 *
 * @param handle 录像器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_deinit(video_recorder_handle_t handle);

// ============================================================================
// 录像控制函数
// ============================================================================

/**
 * @brief 启动录像
 *
 * @param handle 录像器句柄
 * @param reason 录像原因（"motion" - 运动检测，"manual" - 手动触发，"schedule" - 定时等）
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_start(video_recorder_handle_t handle, const char *reason);

/**
 * @brief 停止录像
 *
 * @param handle 录像器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_stop(video_recorder_handle_t handle);

/**
 * @brief 手动触发录像（用于测试）
 *
 * @param handle 录像器句柄
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_manual_trigger(video_recorder_handle_t handle);

// ============================================================================
// 帧处理函数
// ============================================================================

/**
 * @brief 处理视频帧（运动检测和录制）
 *
 * @param handle 录像器句柄
 * @param frame_data 帧数据指针
 * @param frame_size 帧数据大小
 * @param width 帧宽度（像素）
 * @param height 帧高度（像素）
 * @param timestamp 时间戳（毫秒）
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_process_frame(
    video_recorder_handle_t handle,
    uint8_t *frame_data,
    size_t frame_size,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp
);

/**
 * @brief 处理运动矢量（硬件加速检测）
 *
 * @param handle 录像器句柄
 * @param vectors 运动矢量数组
 * @param vector_count 矢量数量
 * @param width 图像宽度（像素）
 * @param height 图像高度（像素）
 * @param timestamp 时间戳（毫秒）
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_process_motion_vectors(
    video_recorder_handle_t handle,
    motion_vector_t *vectors,
    uint32_t vector_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp
);

/**
 * @brief 处理硬件 H.264 编码器输出的运动矢量数据
 *
 * 直接接收 esp_h264 硬件编码器输出的 packed uint32_t 格式运动矢量数据，
 * 内部调用 motion_detector 进行运动判断和热度图生成。
 *
 * @param handle 录像器句柄
 * @param mv_data 硬件运动矢量数据（packed uint32_t 数组）
 * @param data_count 有效运动矢量条目数
 * @param width 图像宽度（像素）
 * @param height 图像高度（像素）
 * @param timestamp 时间戳（毫秒）
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_process_hw_motion_vectors(
    video_recorder_handle_t handle,
    const uint32_t *mv_data,
    uint32_t data_count,
    uint32_t width,
    uint32_t height,
    uint32_t timestamp
);

// ============================================================================
// 状态查询函数
// ============================================================================

/**
 * @brief 获取录像状态
 *
 * @param handle 录像器句柄
 * @param state 录像状态输出
 * @param current_duration_ms 当前录制时长输出（毫秒）
 * @param reason 录像原因输出缓冲区（至少32字节）
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_get_status(
    video_recorder_handle_t handle,
    video_record_state_t *state,
    uint32_t *current_duration_ms,
    char *reason
);

// ============================================================================
// 模式控制函数
// ============================================================================

/**
 * @brief 切换相机模式（监控/录像）
 *
 * @param handle 录像器句柄
 * @param high_resolution true: 高分辨率录像模式，false: 低分辨率监控模式
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_switch_mode(
    video_recorder_handle_t handle,
    bool high_resolution
);

// ============================================================================
// 文件管理函数
// ============================================================================

/**
 * @brief 获取录像文件列表
 *
 * @param handle 录像器句柄
 * @param file_list 文件列表输出数组
 * @param max_files 数组最大容量
 * @param file_count 实际文件数量输出
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_list_files(
    video_recorder_handle_t handle,
    video_metadata_t *file_list,
    int max_files,
    int *file_count
);

/**
 * @brief 获取录像文件元数据
 *
 * @param handle 录像器句柄
 * @param filename 文件名
 * @param metadata 元数据输出
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_get_metadata(
    video_recorder_handle_t handle,
    const char *filename,
    video_metadata_t *metadata
);

/**
 * @brief 删除录像文件
 *
 * @param handle 录像器句柄
 * @param filename 文件名
 * @return esp_err_t 执行结果
 */
esp_err_t video_recorder_delete_file(
    video_recorder_handle_t handle,
    const char *filename
);

#ifdef __cplusplus
}
#endif

#endif // VIDEO_RECORDER_H