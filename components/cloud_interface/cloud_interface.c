/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cloud_interface.h"

static const char *TAG = "cloud_interface";

// 云端接口内部结构
typedef struct {
    cloud_config_t config;
    cloud_connection_state_t state;
    cloud_event_callback_t event_callback;
    void *protocol_handle;           // 具体协议句柄（MQTT/HTTP等）

    // 上传管理
    struct {
        struct {
            char filename[64];
            video_metadata_t metadata;
            uint8_t priority;
            uint32_t retry_count;
            uint32_t start_time;
            bool in_progress;
        } *queue;
        uint32_t queue_size;
        uint32_t queue_head;
        uint32_t queue_tail;
        uint32_t queue_count;
        SemaphoreHandle_t queue_lock;
    } upload_queue;

    // 统计信息
    struct {
        uint32_t total_uploads;
        uint32_t successful_uploads;
        uint32_t failed_uploads;
        uint32_t total_bytes_sent;
        uint32_t connection_time_ms;
    } stats;

    // Wi-Fi管理
    struct {
        char ssid[32];
        char password[64];
        bool configured;
    } wifi;

    // 任务和同步
    TaskHandle_t connection_task;
    TaskHandle_t upload_task;
    SemaphoreHandle_t lock;
    bool running;
} cloud_interface_t;

// 默认配置
static const cloud_config_t default_config = {
    .type = CLOUD_TYPE_MQTT,
    .server_url = "mqtt.example.com",
    .port = 1883,
    .device_id = "esp32-camera-01",
    .username = "user",
    .password = "pass",
    .use_ssl = false,
    .ca_cert = "",
    .reconnect_interval_ms = 5000,
    .keepalive_interval_s = 60,
    .enable_auto_upload = true,
    .upload_retry_count = 3
};

// 初始化上传队列
static esp_err_t init_upload_queue(cloud_interface_t *interface, uint32_t queue_size)
{
    interface->upload_queue.queue = calloc(queue_size, sizeof(*interface->upload_queue.queue));
    if (!interface->upload_queue.queue) {
        ESP_LOGE(TAG, "Failed to allocate upload queue");
        return ESP_ERR_NO_MEM;
    }

    interface->upload_queue.queue_size = queue_size;
    interface->upload_queue.queue_head = 0;
    interface->upload_queue.queue_tail = 0;
    interface->upload_queue.queue_count = 0;

    interface->upload_queue.queue_lock = xSemaphoreCreateMutex();
    if (!interface->upload_queue.queue_lock) {
        ESP_LOGE(TAG, "Failed to create upload queue lock");
        free(interface->upload_queue.queue);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// 添加文件到上传队列
static esp_err_t add_to_upload_queue(
    cloud_interface_t *interface,
    const char *filename,
    const video_metadata_t *metadata,
    uint8_t priority)
{
    xSemaphoreTake(interface->upload_queue.queue_lock, portMAX_DELAY);

    if (interface->upload_queue.queue_count >= interface->upload_queue.queue_size) {
        ESP_LOGW(TAG, "Upload queue is full");
        xSemaphoreGive(interface->upload_queue.queue_lock);
        return ESP_ERR_NO_MEM;
    }

    uint32_t index = interface->upload_queue.queue_tail;

    strncpy(interface->upload_queue.queue[index].filename, filename,
            sizeof(interface->upload_queue.queue[index].filename) - 1);

    if (metadata) {
        memcpy(&interface->upload_queue.queue[index].metadata, metadata, sizeof(video_metadata_t));
    } else {
        memset(&interface->upload_queue.queue[index].metadata, 0, sizeof(video_metadata_t));
        strncpy(interface->upload_queue.queue[index].metadata.filename, filename,
                sizeof(interface->upload_queue.queue[index].metadata.filename) - 1);
    }

    interface->upload_queue.queue[index].priority = priority;
    interface->upload_queue.queue[index].retry_count = 0;
    interface->upload_queue.queue[index].start_time = esp_timer_get_time() / 1000;
    interface->upload_queue.queue[index].in_progress = false;

    interface->upload_queue.queue_tail = (interface->upload_queue.queue_tail + 1) % interface->upload_queue.queue_size;
    interface->upload_queue.queue_count++;

    ESP_LOGI(TAG, "Added to upload queue: %s (priority: %d, queue: %d/%d)",
             filename, priority, interface->upload_queue.queue_count, interface->upload_queue.queue_size);

    xSemaphoreGive(interface->upload_queue.queue_lock);
    return ESP_OK;
}

// 从上传队列获取下一个文件
static esp_err_t get_next_upload_file(
    cloud_interface_t *interface,
    char *filename,
    video_metadata_t *metadata,
    uint8_t *priority)
{
    xSemaphoreTake(interface->upload_queue.queue_lock, portMAX_DELAY);

    if (interface->upload_queue.queue_count == 0) {
        xSemaphoreGive(interface->upload_queue.queue_lock);
        return ESP_ERR_NOT_FOUND;
    }

    // 查找最高优先级的文件
    uint32_t best_index = interface->upload_queue.queue_head;
    uint32_t current_index = interface->upload_queue.queue_head;

    for (uint32_t i = 0; i < interface->upload_queue.queue_count; i++) {
        if (!interface->upload_queue.queue[current_index].in_progress) {
            if (interface->upload_queue.queue[current_index].priority <
                interface->upload_queue.queue[best_index].priority) {
                best_index = current_index;
            }
        }
        current_index = (current_index + 1) % interface->upload_queue.queue_size;
    }

    if (interface->upload_queue.queue[best_index].in_progress) {
        // 所有文件都在上传中
        xSemaphoreGive(interface->upload_queue.queue_lock);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(filename, 64, "%s", interface->upload_queue.queue[best_index].filename);
    filename[63] = '\0';

    if (metadata) {
        memcpy(metadata, &interface->upload_queue.queue[best_index].metadata, sizeof(video_metadata_t));
    }

    if (priority) {
        *priority = interface->upload_queue.queue[best_index].priority;
    }

    interface->upload_queue.queue[best_index].in_progress = true;

    xSemaphoreGive(interface->upload_queue.queue_lock);
    return ESP_OK;
}

// 标记上传完成
static esp_err_t mark_upload_complete(cloud_interface_t *interface, const char *filename, bool success)
{
    xSemaphoreTake(interface->upload_queue.queue_lock, portMAX_DELAY);

    // 查找文件
    for (uint32_t i = 0; i < interface->upload_queue.queue_count; i++) {
        uint32_t index = (interface->upload_queue.queue_head + i) % interface->upload_queue.queue_size;

        if (strcmp(interface->upload_queue.queue[index].filename, filename) == 0) {
            if (success) {
                // 上传成功，从队列中移除
                ESP_LOGI(TAG, "Upload completed: %s", filename);
                interface->stats.successful_uploads++;

                // 移动队列元素
                for (uint32_t j = i; j < interface->upload_queue.queue_count - 1; j++) {
                    uint32_t src = (interface->upload_queue.queue_head + j + 1) % interface->upload_queue.queue_size;
                    uint32_t dst = (interface->upload_queue.queue_head + j) % interface->upload_queue.queue_size;
                    memcpy(&interface->upload_queue.queue[dst], &interface->upload_queue.queue[src],
                           sizeof(interface->upload_queue.queue[0]));
                }

                interface->upload_queue.queue_count--;
                interface->upload_queue.queue_tail = (interface->upload_queue.queue_tail - 1 + interface->upload_queue.queue_size) %
                                                    interface->upload_queue.queue_size;
            } else {
                // 上传失败，增加重试计数
                interface->upload_queue.queue[index].retry_count++;
                interface->upload_queue.queue[index].in_progress = false;
                interface->stats.failed_uploads++;

                ESP_LOGW(TAG, "Upload failed: %s (retry %d/%d)", filename,
                         interface->upload_queue.queue[index].retry_count, interface->config.upload_retry_count);

                if (interface->upload_queue.queue[index].retry_count >= interface->config.upload_retry_count) {
                    // 重试次数超限，从队列中移除
                    ESP_LOGE(TAG, "Upload permanently failed: %s", filename);

                    for (uint32_t j = i; j < interface->upload_queue.queue_count - 1; j++) {
                        uint32_t src = (interface->upload_queue.queue_head + j + 1) % interface->upload_queue.queue_size;
                        uint32_t dst = (interface->upload_queue.queue_head + j) % interface->upload_queue.queue_size;
                        memcpy(&interface->upload_queue.queue[dst], &interface->upload_queue.queue[src],
                               sizeof(interface->upload_queue.queue[0]));
                    }

                    interface->upload_queue.queue_count--;
                    interface->upload_queue.queue_tail = (interface->upload_queue.queue_tail - 1 + interface->upload_queue.queue_size) %
                                                        interface->upload_queue.queue_size;
                }
            }

            interface->stats.total_uploads++;
            xSemaphoreGive(interface->upload_queue.queue_lock);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "File not found in upload queue: %s", filename);
    xSemaphoreGive(interface->upload_queue.queue_lock);
    return ESP_ERR_NOT_FOUND;
}

// 连接管理任务
static void connection_task_func(void *arg)
{
    cloud_interface_t *interface = (cloud_interface_t *)arg;

    while (interface->running) {
        switch (interface->state) {
            case CLOUD_STATE_DISCONNECTED:
                // 尝试连接
                ESP_LOGI(TAG, "Attempting to connect to cloud...");
                interface->state = CLOUD_STATE_CONNECTING;

                // TODO: 实现实际连接逻辑
                // 这里模拟连接过程
                vTaskDelay(2000 / portTICK_PERIOD_MS);

                // 模拟连接成功
                interface->state = CLOUD_STATE_CONNECTED;
                ESP_LOGI(TAG, "Connected to cloud server");

                if (interface->event_callback) {
                    interface->event_callback(CLOUD_EVENT_CONNECTED, NULL, 0);
                }
                break;

            case CLOUD_STATE_CONNECTED:
                // 发送心跳
                vTaskDelay(interface->config.keepalive_interval_s * 1000 / portTICK_PERIOD_MS);

                // TODO: 发送实际心跳
                ESP_LOGD(TAG, "Sending heartbeat");
                break;

            case CLOUD_STATE_ERROR:
                // 错误状态，等待后重试
                ESP_LOGE(TAG, "Connection error, retrying in %d ms", interface->config.reconnect_interval_ms);
                vTaskDelay(interface->config.reconnect_interval_ms / portTICK_PERIOD_MS);
                interface->state = CLOUD_STATE_DISCONNECTED;
                break;

            default:
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                break;
        }
    }

    vTaskDelete(NULL);
}

// 上传管理任务
static void upload_task_func(void *arg)
{
    cloud_interface_t *interface = (cloud_interface_t *)arg;

    while (interface->running) {
        if (interface->state != CLOUD_STATE_CONNECTED) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        // 检查是否有文件需要上传
        char filename[64];
        video_metadata_t metadata;
        uint8_t priority;

        esp_err_t ret = get_next_upload_file(interface, filename, &metadata, &priority);
        if (ret != ESP_OK) {
            // 没有文件需要上传
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Starting upload: %s", filename);

        if (interface->event_callback) {
            interface->event_callback(CLOUD_EVENT_UPLOAD_STARTED, filename, strlen(filename));
        }

        // TODO: 实现实际上传逻辑
        // 这里模拟上传过程
        interface->state = CLOUD_STATE_UPLOADING;

        // 模拟上传进度
        for (int progress = 0; progress <= 100; progress += 10) {
            if (!interface->running) break;

            if (interface->event_callback) {
                interface->event_callback(CLOUD_EVENT_UPLOAD_PROGRESS, &progress, sizeof(progress));
            }

            vTaskDelay(500 / portTICK_PERIOD_MS);
        }

        interface->state = CLOUD_STATE_CONNECTED;

        // 模拟上传完成
        bool success = true;  // 模拟成功

        if (success) {
            ESP_LOGI(TAG, "Upload completed: %s", filename);

            if (interface->event_callback) {
                interface->event_callback(CLOUD_EVENT_UPLOAD_COMPLETED, filename, strlen(filename));
            }
        } else {
            ESP_LOGE(TAG, "Upload failed: %s", filename);

            if (interface->event_callback) {
                interface->event_callback(CLOUD_EVENT_UPLOAD_FAILED, filename, strlen(filename));
            }
        }

        mark_upload_complete(interface, filename, success);
    }

    vTaskDelete(NULL);
}

cloud_interface_handle_t cloud_interface_init(const cloud_config_t *config, cloud_event_callback_t callback)
{
    cloud_interface_t *interface = calloc(1, sizeof(cloud_interface_t));
    if (!interface) {
        ESP_LOGE(TAG, "Failed to allocate cloud interface");
        return NULL;
    }

    // 复制配置
    if (config) {
        memcpy(&interface->config, config, sizeof(cloud_config_t));
    } else {
        memcpy(&interface->config, &default_config, sizeof(cloud_config_t));
    }

    // 初始化状态
    interface->state = CLOUD_STATE_DISCONNECTED;
    interface->event_callback = callback;
    interface->protocol_handle = NULL;
    interface->running = true;

    // 初始化统计信息
    memset(&interface->stats, 0, sizeof(interface->stats));

    // 初始化Wi-Fi配置
    interface->wifi.configured = false;

    // 初始化上传队列
    if (init_upload_queue(interface, 10) != ESP_OK) {  // 10个文件的队列
        free(interface);
        return NULL;
    }

    // 创建互斥锁
    interface->lock = xSemaphoreCreateMutex();
    if (!interface->lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(interface->upload_queue.queue);
        free(interface);
        return NULL;
    }

    // 创建连接管理任务
    BaseType_t ret = xTaskCreate(
        connection_task_func,
        "cloud_conn",
        4096,
        interface,
        5,
        &interface->connection_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connection task");
        vSemaphoreDelete(interface->lock);
        free(interface->upload_queue.queue);
        free(interface);
        return NULL;
    }

    // 创建上传管理任务
    ret = xTaskCreate(
        upload_task_func,
        "cloud_upload",
        8192,  // 需要更大的栈用于文件上传
        interface,
        4,
        &interface->upload_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        vTaskDelete(interface->connection_task);
        vSemaphoreDelete(interface->lock);
        free(interface->upload_queue.queue);
        free(interface);
        return NULL;
    }

    ESP_LOGI(TAG, "Cloud interface initialized");
    ESP_LOGI(TAG, "  Server: %s:%d", interface->config.server_url, interface->config.port);
    ESP_LOGI(TAG, "  Device ID: %s", interface->config.device_id);
    ESP_LOGI(TAG, "  Auto upload: %s", interface->config.enable_auto_upload ? "enabled" : "disabled");

    return (cloud_interface_handle_t)interface;
}

esp_err_t cloud_interface_connect(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    xSemaphoreTake(interface->lock, portMAX_DELAY);

    if (interface->state == CLOUD_STATE_CONNECTED || interface->state == CLOUD_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Already connected or connecting");
        xSemaphoreGive(interface->lock);
        return ESP_OK;
    }

    interface->state = CLOUD_STATE_DISCONNECTED;  // 触发重连

    xSemaphoreGive(interface->lock);
    return ESP_OK;
}

esp_err_t cloud_interface_disconnect(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    xSemaphoreTake(interface->lock, portMAX_DELAY);

    if (interface->state == CLOUD_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "Already disconnected");
        xSemaphoreGive(interface->lock);
        return ESP_OK;
    }

    interface->state = CLOUD_STATE_DISCONNECTED;

    if (interface->event_callback) {
        interface->event_callback(CLOUD_EVENT_DISCONNECTED, NULL, 0);
    }

    ESP_LOGI(TAG, "Disconnected from cloud");

    xSemaphoreGive(interface->lock);
    return ESP_OK;
}

esp_err_t cloud_interface_get_state(cloud_interface_handle_t handle, cloud_connection_state_t *state)
{
    if (!handle || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    xSemaphoreTake(interface->lock, portMAX_DELAY);
    *state = interface->state;
    xSemaphoreGive(interface->lock);

    return ESP_OK;
}

esp_err_t cloud_interface_upload_video(
    cloud_interface_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata,
    uint8_t priority)
{
    if (!handle || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    if (!interface->config.enable_auto_upload) {
        ESP_LOGI(TAG, "Auto upload disabled, file queued but not uploaded: %s", filename);
        return ESP_OK;
    }

    return add_to_upload_queue(interface, filename, metadata, priority);
}

esp_err_t cloud_interface_report_motion(
    cloud_interface_handle_t handle,
    uint32_t timestamp,
    uint8_t motion_level,
    const motion_heatmap_t *heatmap,
    uint8_t *snapshot_data,
    size_t snapshot_size)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现运动事件上报
    ESP_LOGI(TAG, "Motion detected: level=%d, timestamp=%u", motion_level, timestamp);

    return ESP_OK;
}

esp_err_t cloud_interface_report_status(
    cloud_interface_handle_t handle,
    uint8_t cpu_usage,
    uint8_t memory_usage,
    uint8_t storage_usage,
    float temperature,
    uint32_t uptime_ms)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现状态上报
    ESP_LOGD(TAG, "System status: CPU=%d%%, MEM=%d%%, STORAGE=%d%%, TEMP=%.1fC, UPTIME=%us",
             cpu_usage, memory_usage, storage_usage, temperature, uptime_ms / 1000);

    return ESP_OK;
}

esp_err_t cloud_interface_send_heartbeat(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现心跳发送
    ESP_LOGD(TAG, "Sending heartbeat");

    return ESP_OK;
}

esp_err_t cloud_interface_subscribe_commands(cloud_interface_handle_t handle, const char *command_topic)
{
    if (!handle || !command_topic) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现命令订阅
    ESP_LOGI(TAG, "Subscribed to command topic: %s", command_topic);

    return ESP_OK;
}

esp_err_t cloud_interface_process_command(
    cloud_interface_handle_t handle,
    const char *command,
    void *data,
    size_t data_len)
{
    if (!handle || !command) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    ESP_LOGI(TAG, "Received command: %s", command);

    if (interface->event_callback) {
        interface->event_callback(CLOUD_EVENT_COMMAND_RECEIVED, (void *)command, strlen(command));
    }

    // TODO: 处理具体命令
    if (strcmp(command, "start_recording") == 0) {
        ESP_LOGI(TAG, "Manual recording start requested");
    } else if (strcmp(command, "stop_recording") == 0) {
        ESP_LOGI(TAG, "Manual recording stop requested");
    } else if (strcmp(command, "reboot") == 0) {
        ESP_LOGI(TAG, "Reboot requested");
    }

    return ESP_OK;
}

esp_err_t cloud_interface_get_upload_status(
    cloud_interface_handle_t handle,
    uint32_t *pending_files,
    uint32_t *uploading_files,
    uint32_t *failed_files)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    xSemaphoreTake(interface->upload_queue.queue_lock, portMAX_DELAY);

    if (pending_files) {
        uint32_t pending = 0;
        for (uint32_t i = 0; i < interface->upload_queue.queue_count; i++) {
            uint32_t index = (interface->upload_queue.queue_head + i) % interface->upload_queue.queue_size;
            if (!interface->upload_queue.queue[index].in_progress) {
                pending++;
            }
        }
        *pending_files = pending;
    }

    if (uploading_files) {
        uint32_t uploading = 0;
        for (uint32_t i = 0; i < interface->upload_queue.queue_count; i++) {
            uint32_t index = (interface->upload_queue.queue_head + i) % interface->upload_queue.queue_size;
            if (interface->upload_queue.queue[index].in_progress) {
                uploading++;
            }
        }
        *uploading_files = uploading;
    }

    if (failed_files) {
        *failed_files = interface->stats.failed_uploads;
    }

    xSemaphoreGive(interface->upload_queue.queue_lock);

    return ESP_OK;
}

esp_err_t cloud_interface_pause_upload(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现上传暂停
    ESP_LOGI(TAG, "Upload paused");

    return ESP_OK;
}

esp_err_t cloud_interface_resume_upload(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: 实现上传恢复
    ESP_LOGI(TAG, "Upload resumed");

    return ESP_OK;
}

esp_err_t cloud_interface_cancel_upload(cloud_interface_handle_t handle, const char *filename)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    if (filename) {
        // 取消特定文件
        ESP_LOGI(TAG, "Cancelling upload: %s", filename);
        mark_upload_complete(interface, filename, false);
    } else {
        // 取消所有上传
        ESP_LOGI(TAG, "Cancelling all uploads");

        xSemaphoreTake(interface->upload_queue.queue_lock, portMAX_DELAY);
        interface->upload_queue.queue_count = 0;
        interface->upload_queue.queue_head = 0;
        interface->upload_queue.queue_tail = 0;
        xSemaphoreGive(interface->upload_queue.queue_lock);
    }

    return ESP_OK;
}

esp_err_t cloud_interface_set_wifi_config(
    cloud_interface_handle_t handle,
    const char *ssid,
    const char *password)
{
    if (!handle || !ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    xSemaphoreTake(interface->lock, portMAX_DELAY);

    strncpy(interface->wifi.ssid, ssid, sizeof(interface->wifi.ssid) - 1);
    strncpy(interface->wifi.password, password, sizeof(interface->wifi.password) - 1);
    interface->wifi.configured = true;

    ESP_LOGI(TAG, "Wi-Fi config updated: SSID=%s", ssid);

    xSemaphoreGive(interface->lock);
    return ESP_OK;
}

esp_err_t cloud_interface_deinit(cloud_interface_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_interface_t *interface = (cloud_interface_t *)handle;

    // 停止任务
    interface->running = false;

    if (interface->connection_task) {
        vTaskDelete(interface->connection_task);
    }

    if (interface->upload_task) {
        vTaskDelete(interface->upload_task);
    }

    // 等待任务结束
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // 释放资源
    if (interface->upload_queue.queue) {
        free(interface->upload_queue.queue);
    }

    if (interface->upload_queue.queue_lock) {
        vSemaphoreDelete(interface->upload_queue.queue_lock);
    }

    if (interface->lock) {
        vSemaphoreDelete(interface->lock);
    }

    free(interface);

    ESP_LOGI(TAG, "Cloud interface deinitialized");
    return ESP_OK;
}