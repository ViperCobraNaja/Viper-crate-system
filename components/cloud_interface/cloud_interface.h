/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CLOUD_INTERFACE_H
#define CLOUD_INTERFACE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "video_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

// 云端接口类型
typedef enum {
    CLOUD_TYPE_MQTT = 0,      // MQTT协议
    CLOUD_TYPE_HTTP,          // HTTP REST API
    CLOUD_TYPE_WEBSOCKET,     // WebSocket
    CLOUD_TYPE_CUSTOM         // 自定义协议
} cloud_interface_type_t;

// 云端连接状态
typedef enum {
    CLOUD_STATE_DISCONNECTED = 0,
    CLOUD_STATE_CONNECTING,
    CLOUD_STATE_CONNECTED,
    CLOUD_STATE_UPLOADING,
    CLOUD_STATE_ERROR
} cloud_connection_state_t;

// 云端配置
typedef struct {
    cloud_interface_type_t type;      // 接口类型
    char server_url[128];             // 服务器地址
    uint16_t port;                    // 端口
    char device_id[64];               // 设备ID
    char username[32];                // 用户名
    char password[32];                // 密码
    bool use_ssl;                     // 使用SSL/TLS
    char ca_cert[256];                // CA证书路径
    uint32_t reconnect_interval_ms;   // 重连间隔（毫秒）
    uint32_t keepalive_interval_s;    // 心跳间隔（秒）
    bool enable_auto_upload;          // 启用自动上传
    uint32_t upload_retry_count;      // 上传重试次数
} cloud_config_t;

// 云端事件
typedef enum {
    CLOUD_EVENT_CONNECTED = 0,        // 连接成功
    CLOUD_EVENT_DISCONNECTED,         // 连接断开
    CLOUD_EVENT_MESSAGE_RECEIVED,     // 收到消息
    CLOUD_EVENT_UPLOAD_STARTED,       // 上传开始
    CLOUD_EVENT_UPLOAD_PROGRESS,      // 上传进度
    CLOUD_EVENT_UPLOAD_COMPLETED,     // 上传完成
    CLOUD_EVENT_UPLOAD_FAILED,        // 上传失败
    CLOUD_EVENT_COMMAND_RECEIVED      // 收到控制命令
} cloud_event_t;

// 云端事件回调函数类型
typedef void (*cloud_event_callback_t)(cloud_event_t event, void *data, size_t data_len);

// 云端接口句柄
typedef void* cloud_interface_handle_t;

/**
 * @brief 初始化云端接口
 *
 * @param config 云端配置
 * @param callback 事件回调函数
 * @return cloud_interface_handle_t 接口句柄
 */
cloud_interface_handle_t cloud_interface_init(const cloud_config_t *config, cloud_event_callback_t callback);

/**
 * @brief 连接到云端服务器
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_connect(cloud_interface_handle_t handle);

/**
 * @brief 断开云端连接
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_disconnect(cloud_interface_handle_t handle);

/**
 * @brief 获取连接状态
 *
 * @param handle 接口句柄
 * @param state 连接状态
 * @return esp_err_t
 */
esp_err_t cloud_interface_get_state(cloud_interface_handle_t handle, cloud_connection_state_t *state);

/**
 * @brief 上传录像文件到云端
 *
 * @param handle 接口句柄
 * @param filename 文件名
 * @param metadata 录像元数据
 * @param priority 上传优先级（0-9，0最高）
 * @return esp_err_t
 */
esp_err_t cloud_interface_upload_video(
    cloud_interface_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata,
    uint8_t priority
);

/**
 * @brief 上传运动检测事件
 *
 * @param handle 接口句柄
 * @param timestamp 时间戳
 * @param motion_level 运动强度
 * @param heatmap 运动热度图（可选）
 * @param snapshot_data 快照数据（可选）
 * @param snapshot_size 快照大小
 * @return esp_err_t
 */
esp_err_t cloud_interface_report_motion(
    cloud_interface_handle_t handle,
    uint32_t timestamp,
    uint8_t motion_level,
    const motion_heatmap_t *heatmap,
    uint8_t *snapshot_data,
    size_t snapshot_size
);

/**
 * @brief 上传系统状态
 *
 * @param handle 接口句柄
 * @param cpu_usage CPU使用率（0-100）
 * @param memory_usage 内存使用率（0-100）
 * @param storage_usage 存储使用率（0-100）
 * @param temperature 温度（摄氏度）
 * @param uptime_ms 运行时间（毫秒）
 * @return esp_err_t
 */
esp_err_t cloud_interface_report_status(
    cloud_interface_handle_t handle,
    uint8_t cpu_usage,
    uint8_t memory_usage,
    uint8_t storage_usage,
    float temperature,
    uint32_t uptime_ms
);

/**
 * @brief 发送设备心跳
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_send_heartbeat(cloud_interface_handle_t handle);

/**
 * @brief 订阅云端命令
 *
 * @param handle 接口句柄
 * @param command_topic 命令主题
 * @return esp_err_t
 */
esp_err_t cloud_interface_subscribe_commands(cloud_interface_handle_t handle, const char *command_topic);

/**
 * @brief 处理接收到的命令
 *
 * @param handle 接口句柄
 * @param command 命令字符串
 * @param data 命令数据
 * @param data_len 数据长度
 * @return esp_err_t
 */
esp_err_t cloud_interface_process_command(
    cloud_interface_handle_t handle,
    const char *command,
    void *data,
    size_t data_len
);

/**
 * @brief 获取上传队列状态
 *
 * @param handle 接口句柄
 * @param pending_files 待上传文件数
 * @param uploading_files 正在上传文件数
 * @param failed_files 失败文件数
 * @return esp_err_t
 */
esp_err_t cloud_interface_get_upload_status(
    cloud_interface_handle_t handle,
    uint32_t *pending_files,
    uint32_t *uploading_files,
    uint32_t *failed_files
);

/**
 * @brief 暂停上传
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_pause_upload(cloud_interface_handle_t handle);

/**
 * @brief 恢复上传
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_resume_upload(cloud_interface_handle_t handle);

/**
 * @brief 取消上传
 *
 * @param handle 接口句柄
 * @param filename 文件名（NULL取消所有）
 * @return esp_err_t
 */
esp_err_t cloud_interface_cancel_upload(cloud_interface_handle_t handle, const char *filename);

/**
 * @brief 设置Wi-Fi配置
 *
 * @param handle 接口句柄
 * @param ssid Wi-Fi SSID
 * @param password Wi-Fi密码
 * @return esp_err_t
 */
esp_err_t cloud_interface_set_wifi_config(
    cloud_interface_handle_t handle,
    const char *ssid,
    const char *password
);

/**
 * @brief 释放云端接口资源
 *
 * @param handle 接口句柄
 * @return esp_err_t
 */
esp_err_t cloud_interface_deinit(cloud_interface_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // CLOUD_INTERFACE_H