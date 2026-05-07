/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "video_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

// 存储管理器状态
typedef enum {
    STORAGE_STATE_UNINITIALIZED = 0,
    STORAGE_STATE_READY,
    STORAGE_STATE_BUSY,
    STORAGE_STATE_ERROR,
    STORAGE_STATE_FULL
} storage_state_t;

// 存储统计信息
typedef struct {
    uint64_t total_space_bytes;     // 总空间（字节）
    uint64_t free_space_bytes;      // 空闲空间（字节）
    uint64_t used_space_bytes;      // 已用空间（字节）
    uint32_t total_files;           // 总文件数
    uint32_t video_files;           // 视频文件数
    uint32_t metadata_files;        // 元数据文件数
    uint32_t log_files;             // 日志文件数
    float usage_percentage;         // 使用百分比
} storage_stats_t;

// 存储管理器配置
typedef struct {
    char base_path[64];             // 基础路径
    char video_subdir[32];          // 视频子目录
    char metadata_subdir[32];       // 元数据子目录
    char log_subdir[32];            // 日志子目录
    uint64_t max_storage_bytes;     // 最大存储空间（字节）
    uint32_t max_files;             // 最大文件数
    bool auto_cleanup;              // 自动清理旧文件
    uint32_t cleanup_threshold;     // 清理阈值（百分比）
    uint32_t min_keep_files;        // 最少保留文件数
    uint32_t min_keep_days;         // 最少保留天数
} storage_manager_config_t;

// 存储管理器句柄
typedef void* storage_manager_handle_t;

/**
 * @brief 初始化存储管理器
 *
 * @param config 管理器配置
 * @return storage_manager_handle_t 管理器句柄
 */
storage_manager_handle_t storage_manager_init(const storage_manager_config_t *config);

/**
 * @brief 获取存储状态
 *
 * @param handle 管理器句柄
 * @param state 存储状态
 * @return esp_err_t
 */
esp_err_t storage_manager_get_state(storage_manager_handle_t handle, storage_state_t *state);

/**
 * @brief 获取存储统计信息
 *
 * @param handle 管理器句柄
 * @param stats 统计信息
 * @return esp_err_t
 */
esp_err_t storage_manager_get_stats(storage_manager_handle_t handle, storage_stats_t *stats);

/**
 * @brief 创建录像文件
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @param width 视频宽度
 * @param height 视频高度
 * @param fps 帧率
 * @param bitrate 码率
 * @return esp_err_t
 */
esp_err_t storage_manager_create_video_file(
    storage_manager_handle_t handle,
    const char *filename,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate
);

/**
 * @brief 写入视频帧数据
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @param data 帧数据
 * @param size 数据大小
 * @param timestamp 时间戳（毫秒）
 * @param is_keyframe 是否为关键帧
 * @return esp_err_t
 */
esp_err_t storage_manager_write_video_frame(
    storage_manager_handle_t handle,
    const char *filename,
    uint8_t *data,
    size_t size,
    uint64_t timestamp,
    bool is_keyframe
);

/**
 * @brief 关闭录像文件
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @param metadata 录像元数据
 * @return esp_err_t
 */
esp_err_t storage_manager_close_video_file(
    storage_manager_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata
);

/**
 * @brief 保存录像元数据
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @param metadata 元数据
 * @return esp_err_t
 */
esp_err_t storage_manager_save_metadata(
    storage_manager_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata
);

/**
 * @brief 获取录像元数据
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @param metadata 元数据
 * @return esp_err_t
 */
esp_err_t storage_manager_get_metadata(
    storage_manager_handle_t handle,
    const char *filename,
    video_metadata_t *metadata
);

/**
 * @brief 获取录像文件列表
 *
 * @param handle 管理器句柄
 * @param max_files 最大文件数
 * @param file_list 文件列表
 * @param file_count 实际文件数
 * @return esp_err_t
 */
esp_err_t storage_manager_list_video_files(
    storage_manager_handle_t handle,
    int max_files,
    video_metadata_t *file_list,
    int *file_count
);

/**
 * @brief 删除录像文件
 *
 * @param handle 管理器句柄
 * @param filename 文件名
 * @return esp_err_t
 */
esp_err_t storage_manager_delete_video_file(
    storage_manager_handle_t handle,
    const char *filename
);

/**
 * @brief 执行存储清理
 *
 * @param handle 管理器句柄
 * @param freed_bytes 释放的字节数
 * @return esp_err_t
 */
esp_err_t storage_manager_cleanup(storage_manager_handle_t handle, uint64_t *freed_bytes);

/**
 * @brief 写入系统日志
 *
 * @param handle 管理器句柄
 * @param level 日志级别（0:DEBUG, 1:INFO, 2:WARN, 3:ERROR）
 * @param module 模块名
 * @param message 日志消息
 * @return esp_err_t
 */
esp_err_t storage_manager_write_log(
    storage_manager_handle_t handle,
    uint8_t level,
    const char *module,
    const char *message
);

/**
 * @brief 检查存储空间是否足够
 *
 * @param handle 管理器句柄
 * @param required_bytes 需要字节数
 * @param available_bytes 可用字节数
 * @return esp_err_t
 */
esp_err_t storage_manager_check_space(
    storage_manager_handle_t handle,
    uint64_t required_bytes,
    uint64_t *available_bytes
);

/**
 * @brief 格式化存储（危险操作）
 *
 * @param handle 管理器句柄
 * @return esp_err_t
 */
esp_err_t storage_manager_format(storage_manager_handle_t handle);

/**
 * @brief 释放存储管理器资源
 *
 * @param handle 管理器句柄
 * @return esp_err_t
 */
esp_err_t storage_manager_deinit(storage_manager_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_MANAGER_H