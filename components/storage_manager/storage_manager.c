/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "bsp/esp32_p4_function_ev_board.h"

#include "esp_vfs.h"

#include "storage_manager.h"
#include "esp_muxer.h"
#include "impl/mp4_muxer.h"

static const char *TAG = "storage_manager";

// 存储管理器内部结构
typedef struct {
    storage_manager_config_t config;
    storage_state_t state;
    storage_stats_t stats;
    sdmmc_card_t *sd_card;
    wl_handle_t wl_handle;
    bool sd_card_mounted;
    bool own_hardware;           // true: 本模块初始化了SDMMC硬件，需负责释放

    // 文件句柄缓存
    struct {
        char filename[64];
        char full_path[128];
        esp_muxer_handle_t muxer;
        int video_stream_index;
        bool stream_added;
        uint32_t frame_count;
        video_metadata_t metadata;
    } current_video_file;

    // 注册标志（仅初始化一次）
    bool muxer_registered;

    // 同步锁
    SemaphoreHandle_t lock;
} storage_manager_t;

// 默认配置
static const storage_manager_config_t default_config = {
    .base_path = "/sdcard",
    .video_subdir = "videos",
    .metadata_subdir = "meta",
    .log_subdir = "logs",
    .max_storage_bytes = 4ULL * 1024 * 1024 * 1024,  // 4GB
    .max_files = 100,
    .auto_cleanup = true,
    .cleanup_threshold = 90,  // 90%
    .min_keep_files = 10,
    .min_keep_days = 7
};

// 获取完整路径
// 将 void 改为 esp_err_t，并增加截断检查
static esp_err_t get_full_path(storage_manager_t *manager, const char *subdir,
                               const char *filename, char *fullpath, size_t size)
{
    int ret = snprintf(fullpath, size, "%s/%s/%s", manager->config.base_path, subdir, filename);
    if (ret < 0 || (size_t)ret >= size) {
        ESP_LOGW(TAG, "Path truncated in get_full_path: %s/%s/%s", 
                 manager->config.base_path, subdir, filename);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

// 创建目录（如果不存在）
static esp_err_t create_directory_if_not_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        // 目录已存在
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Path exists but is not a directory: %s", path);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 创建目录
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create directory: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created directory: %s", path);
    return ESP_OK;
}

// 构造目录路径（不以 / 结尾）
static void make_dir_path(storage_manager_t *manager, const char *subdir, char *path, size_t size)
{
    int ret = snprintf(path, size, "%s/%s", manager->config.base_path, subdir);
    if (ret < 0 || (size_t)ret >= size) {
        ESP_LOGW(TAG, "Path truncated in make_dir_path: %s/%s", manager->config.base_path, subdir);
        path[0] = '\0';
    }
}

// 初始化目录结构
static esp_err_t init_directories(storage_manager_t *manager)
{
    char video_path[128];
    char meta_path[128];
    char log_path[128];

    make_dir_path(manager, manager->config.video_subdir, video_path, sizeof(video_path));
    make_dir_path(manager, manager->config.metadata_subdir, meta_path, sizeof(meta_path));
    make_dir_path(manager, manager->config.log_subdir, log_path, sizeof(log_path));

    esp_err_t ret = ESP_OK;

    ret |= create_directory_if_not_exists(manager->config.base_path);
    ret |= create_directory_if_not_exists(video_path);
    ret |= create_directory_if_not_exists(meta_path);
    ret |= create_directory_if_not_exists(log_path);

    return ret;
}

// 获取文件大小
static uint64_t get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

// 更新存储统计信息
static esp_err_t update_storage_stats(storage_manager_t *manager)
{
    if (!manager->sd_card_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    // 获取实际空闲空间
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(manager->config.base_path, &total_bytes, &free_bytes) == ESP_OK) {
        manager->stats.total_space_bytes = total_bytes;
        manager->stats.free_space_bytes = free_bytes;
        manager->stats.used_space_bytes = total_bytes - free_bytes;
    } else if (manager->sd_card) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed, using CSD capacity as fallback");
        uint64_t total_sectors = manager->sd_card->csd.capacity;
        uint64_t sector_size = manager->sd_card->csd.sector_size;
        manager->stats.total_space_bytes = total_sectors * sector_size;
        manager->stats.free_space_bytes = manager->stats.total_space_bytes / 2;
        manager->stats.used_space_bytes = manager->stats.total_space_bytes - manager->stats.free_space_bytes;
    } else {
        ESP_LOGW(TAG, "Cannot determine storage stats");
        manager->stats.total_space_bytes = 0;
        manager->stats.free_space_bytes = 0;
        manager->stats.used_space_bytes = 0;
    }

    if (manager->stats.total_space_bytes > 0) {
        manager->stats.usage_percentage = (float)manager->stats.used_space_bytes * 100.0f /
                                         manager->stats.total_space_bytes;
    } else {
        manager->stats.usage_percentage = 0.0f;
    }

    // 统计文件数
    manager->stats.total_files = 0;
    manager->stats.video_files = 0;
    manager->stats.metadata_files = 0;
    manager->stats.log_files = 0;

    char video_path[128];
    make_dir_path(manager, manager->config.video_subdir, video_path, sizeof(video_path));

    DIR *dir = opendir(video_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcmp(ext, ".mp4") == 0) {
                    manager->stats.video_files++;
                }
            }
        }
        closedir(dir);
    }

    manager->stats.total_files = manager->stats.video_files;  // 简化

    // 更新状态
    if (manager->stats.usage_percentage >= manager->config.cleanup_threshold) {
        manager->state = STORAGE_STATE_FULL;
    } else if (manager->sd_card_mounted) {
        manager->state = STORAGE_STATE_READY;
    }

    return ESP_OK;
}

static bool muxer_registered_flag = false;

storage_manager_handle_t storage_manager_init(const storage_manager_config_t *config)
{
    storage_manager_t *manager = calloc(1, sizeof(storage_manager_t));
    if (!manager) {
        ESP_LOGE(TAG, "Failed to allocate storage manager");
        return NULL;
    }

    // 复制配置
    if (config) {
        memcpy(&manager->config, config, sizeof(storage_manager_config_t));
    } else {
        memcpy(&manager->config, &default_config, sizeof(storage_manager_config_t));
    }

    // 初始化状态
    manager->state = STORAGE_STATE_UNINITIALIZED;
    memset(&manager->stats, 0, sizeof(storage_stats_t));
    manager->sd_card = NULL;
    manager->wl_handle = 0;
    manager->sd_card_mounted = false;
    manager->own_hardware = false;

    // 初始化当前视频文件
    manager->current_video_file.filename[0] = '\0';
    manager->current_video_file.full_path[0] = '\0';
    manager->current_video_file.muxer = NULL;
    manager->current_video_file.video_stream_index = -1;
    manager->current_video_file.stream_added = false;
    manager->current_video_file.frame_count = 0;

    // 注册 MP4 muxer（全局仅一次）
    if (!muxer_registered_flag) {
        esp_muxer_err_t reg_ret = mp4_muxer_register();
        if (reg_ret != ESP_MUXER_ERR_OK) {
            ESP_LOGW(TAG, "mp4_muxer_register returned %d (may already be registered)", reg_ret);
        } else {
            ESP_LOGI(TAG, "MP4 muxer registered");
        }
        muxer_registered_flag = true;
    }
    manager->muxer_registered = true;

    // 创建互斥锁
    manager->lock = xSemaphoreCreateMutex();
    if (!manager->lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(manager);
        return NULL;
    }

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 检测文件系统是否已被 BSP 挂载
    struct stat st;
    esp_err_t ret;

    if (stat(manager->config.base_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        // 文件系统已存在，跳过硬件初始化
        ESP_LOGI(TAG, "SD card filesystem already mounted at %s, reusing", manager->config.base_path);
        manager->sd_card = bsp_sdcard_get_handle();
        manager->sd_card_mounted = true;
        manager->own_hardware = false;
    } else {
        // 使用 BSP API 初始化 SD 卡（含 LDO 电源控制）
        ESP_LOGI(TAG, "Initializing SD card via BSP...");

        ret = bsp_sdcard_mount();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card via BSP: 0x%x", ret);
            xSemaphoreGive(manager->lock);
            storage_manager_deinit((storage_manager_handle_t)manager);
            return NULL;
        }

        manager->sd_card = bsp_sdcard_get_handle();
        if (manager->sd_card) {
            ESP_LOGI(TAG, "SD card detected:");
            ESP_LOGI(TAG, "  Name: %s", manager->sd_card->cid.name);
            ESP_LOGI(TAG, "  Type: %s", (manager->sd_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
            ESP_LOGI(TAG, "  Size: %llu MB", ((uint64_t)manager->sd_card->csd.capacity) *
                     manager->sd_card->csd.sector_size / (1024 * 1024));
        }

        manager->sd_card_mounted = true;
        manager->own_hardware = true;
    }

    // 创建目录结构
    ret = init_directories(manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create directories");
        xSemaphoreGive(manager->lock);
        storage_manager_deinit((storage_manager_handle_t)manager);
        return NULL;
    }

    // 更新存储统计
    update_storage_stats(manager);

    manager->state = STORAGE_STATE_READY;

    ESP_LOGI(TAG, "Storage manager initialized successfully");
    ESP_LOGI(TAG, "  Base path: %s", manager->config.base_path);
    ESP_LOGI(TAG, "  Video dir: %s/%s", manager->config.base_path, manager->config.video_subdir);
    ESP_LOGI(TAG, "  Total space: %.2f MB", manager->stats.total_space_bytes / (1024.0 * 1024.0));
    ESP_LOGI(TAG, "  Used space: %.1f%%", manager->stats.usage_percentage);

    xSemaphoreGive(manager->lock);
    return (storage_manager_handle_t)manager;
}

esp_err_t storage_manager_get_state(storage_manager_handle_t handle, storage_state_t *state)
{
    if (!handle || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);
    *state = manager->state;
    xSemaphoreGive(manager->lock);

    return ESP_OK;
}

esp_err_t storage_manager_get_stats(storage_manager_handle_t handle, storage_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);
    memcpy(stats, &manager->stats, sizeof(storage_stats_t));
    xSemaphoreGive(manager->lock);

    return ESP_OK;
}

// ---- MP4 muxer 回调与辅助函数 ----

// 自定义文件写入器，确保 fsync + 调试日志
static void *muxer_fopen(char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        setvbuf(f, NULL, _IOFBF, 64 * 1024);
        ESP_LOGI(TAG, "muxer: fopen(%s) OK", path);
    } else {
        ESP_LOGE(TAG, "muxer: fopen(%s) FAILED errno=%d", path, errno);
    }
    return f;
}

static int muxer_fwrite(void *writer, void *buffer, int len)
{
    size_t written = fwrite(buffer, 1, len, (FILE *)writer);
    if (written != (size_t)len) {
        ESP_LOGE(TAG, "muxer: fwrite(%d) only wrote %d bytes", len, (int)written);
        return -1;
    }
    return len;
}

static int muxer_fseek(void *writer, uint64_t pos)
{
    return fseek((FILE *)writer, (long)pos, SEEK_SET);  // 0 = OK
}

static int muxer_fclose(void *writer)
{
    FILE *f = (FILE *)writer;
    fflush(f);
    fsync(fileno(f));
    int ret = fclose(f);
    ESP_LOGI(TAG, "muxer: fclose ret=%d", ret);
    return ret;
}

static esp_muxer_file_writer_t s_muxer_writer = {
    .on_open  = muxer_fopen,
    .on_write = muxer_fwrite,
    .on_seek  = muxer_fseek,
    .on_close = muxer_fclose,
};

// url_pattern_ex 回调：向 muxer 提供文件路径
static int muxer_url_pattern_cb(esp_muxer_slice_info_t *info, void *ctx)
{
    storage_manager_t *manager = (storage_manager_t *)ctx;
    strncpy(info->file_path, manager->current_video_file.full_path, info->len - 1);
    info->file_path[info->len - 1] = '\0';
    ESP_LOGI(TAG, "muxer: url_pattern path=%s len=%d", info->file_path, info->len);
    return 0;
}

// 从 H.264 Annex B 帧数据中提取 SPS + PPS（含起始码）
static int extract_sps_pps(const uint8_t *data, size_t size,
                           uint8_t *out, size_t out_size)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    const uint8_t *end = data + size;
    const uint8_t *p = data;

    while (p < end - 3) {
        int sc_len = 0;
        if (p[0] == 0 && p[1] == 0) {
            if (p[2] == 0 && p[3] == 1) {
                sc_len = 4;
            } else if (p[2] == 1) {
                sc_len = 3;
            }
        }

        if (sc_len > 0) {
            const uint8_t *nal_start = p;
            uint8_t nal_type = p[sc_len] & 0x1F;

            const uint8_t *next = p + sc_len;
            while (next < end - 3) {
                if (next[0] == 0 && next[1] == 0) {
                    if (next[2] == 0 && next[3] == 1) break;
                    if (next[2] == 1) break;
                }
                next++;
            }

            size_t nal_len = next - nal_start;

            if (nal_type == 7 && !sps) {
                sps = nal_start;
                sps_len = nal_len;
            } else if (nal_type == 8 && !pps) {
                pps = nal_start;
                pps_len = nal_len;
            }

            p = next;
        } else {
            p++;
        }
    }

    if (!sps || !pps) return -1;
    if (sps_len + pps_len > out_size) return -1;

    memcpy(out, sps, sps_len);
    memcpy(out + sps_len, pps, pps_len);
    return (int)(sps_len + pps_len);
}

esp_err_t storage_manager_create_video_file(
    storage_manager_handle_t handle,
    const char *filename,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    uint32_t bitrate)
{
    if (!handle || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    if (manager->state != STORAGE_STATE_READY) {
        ESP_LOGE(TAG, "Storage not ready, state: %d", manager->state);
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_STATE;
    }

    // 关闭当前文件（如果有）
    if (manager->current_video_file.muxer) {
        esp_muxer_close(manager->current_video_file.muxer);
        manager->current_video_file.muxer = NULL;
        manager->current_video_file.stream_added = false;
    }

    // 生成完整路径
    char fullpath[128];
    get_full_path(manager, manager->config.video_subdir, filename, fullpath, sizeof(fullpath));

    // 保存文件路径和元数据
    strncpy(manager->current_video_file.filename, filename,
            sizeof(manager->current_video_file.filename) - 1);
    manager->current_video_file.filename[sizeof(manager->current_video_file.filename) - 1] = '\0';
    strncpy(manager->current_video_file.full_path, fullpath,
            sizeof(manager->current_video_file.full_path) - 1);
    manager->current_video_file.full_path[sizeof(manager->current_video_file.full_path) - 1] = '\0';
    manager->current_video_file.video_stream_index = -1;
    manager->current_video_file.stream_added = false;
    manager->current_video_file.frame_count = 0;

    // 初始化元数据
    memset(&manager->current_video_file.metadata, 0, sizeof(video_metadata_t));
    strncpy(manager->current_video_file.metadata.filename, filename,
            sizeof(manager->current_video_file.metadata.filename) - 1);
    manager->current_video_file.metadata.width = width;
    manager->current_video_file.metadata.height = height;
    manager->current_video_file.metadata.fps = fps;
    manager->current_video_file.metadata.bitrate = bitrate;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(manager->current_video_file.metadata.create_time,
             sizeof(manager->current_video_file.metadata.create_time),
             "%Y-%m-%dT%H:%M:%S", &timeinfo);

    // 配置 MP4 muxer（文件 I/O 由 muxer 内部通过 url_pattern_ex 回调管理）
    mp4_muxer_config_t muxer_cfg = {
        .base_config = {
            .muxer_type          = ESP_MUXER_TYPE_MP4,
            .slice_duration      = ESP_MUXER_MAX_SLICE_DURATION,
            .url_pattern_ex      = muxer_url_pattern_cb,
            .ctx                 = manager,
            .ram_cache_size      = 32 * 1024,  // muxer 内部 DMA 友好缓存 (官方推荐 ≥16KB)
            .no_key_frame_verify = true,
        },
        .display_in_order  = true,
        .moov_before_mdat  = false,
    };

    manager->current_video_file.muxer = esp_muxer_open(
        (esp_muxer_config_t *)&muxer_cfg, sizeof(muxer_cfg));
    if (!manager->current_video_file.muxer) {
        ESP_LOGE(TAG, "Failed to open MP4 muxer for: %s", fullpath);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created video file: %s", fullpath);

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_write_video_frame(
    storage_manager_handle_t handle,
    const char *filename,
    uint8_t *data,
    size_t size,
    uint64_t timestamp,
    bool is_keyframe)
{
    if (!handle || !filename || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    if (!manager->current_video_file.muxer) {
        ESP_LOGE(TAG, "No active muxer");
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(manager->current_video_file.filename, filename) != 0) {
        ESP_LOGE(TAG, "File mismatch: expected %s, got %s",
                 manager->current_video_file.filename, filename);
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_ARG;
    }

    // 首个关键帧：提取 SPS/PPS，添加视频流
    if (!manager->current_video_file.stream_added && is_keyframe) {
        uint8_t sps_pps_buf[256];
        int spec_len = extract_sps_pps(data, size, sps_pps_buf, sizeof(sps_pps_buf));
        if (spec_len < 0) {
            ESP_LOGW(TAG, "SPS/PPS not found in first keyframe (%u bytes), "
                     "trying without codec_spec_info", (unsigned)size);
        }

        esp_muxer_video_stream_info_t video_info = {
            .codec              = ESP_MUXER_VDEC_H264,
            .width              = (uint16_t)manager->current_video_file.metadata.width,
            .height             = (uint16_t)manager->current_video_file.metadata.height,
            .fps                = (uint8_t)manager->current_video_file.metadata.fps,
            .min_packet_duration = 1000 / manager->current_video_file.metadata.fps,
            .codec_spec_info    = (spec_len > 0) ? sps_pps_buf : NULL,
            .spec_info_len      = (spec_len > 0) ? spec_len : 0,
        };

        esp_muxer_err_t ret = esp_muxer_add_video_stream(
            manager->current_video_file.muxer, &video_info,
            &manager->current_video_file.video_stream_index);
        if (ret != ESP_MUXER_ERR_OK) {
            ESP_LOGE(TAG, "Failed to add video stream: %d", ret);
            xSemaphoreGive(manager->lock);
            return ESP_FAIL;
        }
        manager->current_video_file.stream_added = true;
        ESP_LOGI(TAG, "Video stream added (SPS/PPS %d bytes), stream_idx=%d",
                 spec_len, manager->current_video_file.video_stream_index);
    }

    if (!manager->current_video_file.stream_added) {
        // 尚未收到关键帧，跳过此帧
        xSemaphoreGive(manager->lock);
        return ESP_OK;
    }

    // 写入视频包
    esp_muxer_video_packet_t packet = {
        .data      = data,
        .len       = (int)size,
        .pts       = (uint32_t)timestamp,
        .dts       = (uint32_t)timestamp,
        .key_frame = is_keyframe,
    };

    esp_muxer_err_t ret = esp_muxer_add_video_packet(
        manager->current_video_file.muxer,
        manager->current_video_file.video_stream_index, &packet);
    if (ret != ESP_MUXER_ERR_OK) {
        ESP_LOGE(TAG, "Failed to write video packet: %d", ret);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    manager->current_video_file.frame_count++;

    if (manager->current_video_file.frame_count % 30 == 0) {
        ESP_LOGI(TAG, "SD write: %lu frames",
                 (unsigned long)manager->current_video_file.frame_count);
    }

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_close_video_file(
    storage_manager_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata)
{
    if (!handle || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    if (!manager->current_video_file.muxer) {
        ESP_LOGE(TAG, "No active muxer");
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(manager->current_video_file.filename, filename) != 0) {
        ESP_LOGE(TAG, "File mismatch: expected %s, got %s",
                 manager->current_video_file.filename, filename);
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_ARG;
    }

    // 更新元数据
    if (metadata) {
        memcpy(&manager->current_video_file.metadata, metadata, sizeof(video_metadata_t));
    }

    manager->current_video_file.metadata.duration_ms = 0;
    manager->current_video_file.metadata.frame_count = manager->current_video_file.frame_count;

    // 关闭 muxer（回填 moov 等容器尾部信息）
    if (manager->current_video_file.muxer) {
        esp_muxer_close(manager->current_video_file.muxer);
        manager->current_video_file.muxer = NULL;
        manager->current_video_file.stream_added = false;
    }

    // 获取文件大小
    uint64_t file_size = 0;
    struct stat st;
    if (stat(manager->current_video_file.full_path, &st) == 0) {
        file_size = st.st_size;
    }
    manager->current_video_file.metadata.file_size = (uint32_t)file_size;

    // 保存元数据
    char video_path[128];
    char meta_path[128];
    get_full_path(manager, manager->config.video_subdir, filename, video_path, sizeof(video_path));
    get_full_path(manager, manager->config.metadata_subdir, filename, meta_path, sizeof(meta_path));

    // 修改扩展名为.json
    char *dot = strrchr(meta_path, '.');
    if (dot) {
        strcpy(dot, ".json");
    }

    FILE *meta_file = fopen(meta_path, "w");
    if (meta_file) {
        // TODO: 将元数据写入JSON文件
        fprintf(meta_file, "{\n");
        fprintf(meta_file, "  \"filename\": \"%s\",\n", manager->current_video_file.metadata.filename);
        fprintf(meta_file, "  \"create_time\": \"%s\",\n", manager->current_video_file.metadata.create_time);
        fprintf(meta_file, "  \"duration_ms\": %lu,\n", (unsigned long)manager->current_video_file.metadata.duration_ms);
        fprintf(meta_file, "  \"width\": %lu,\n", (unsigned long)manager->current_video_file.metadata.width);
        fprintf(meta_file, "  \"height\": %lu,\n", (unsigned long)manager->current_video_file.metadata.height);
        fprintf(meta_file, "  \"fps\": %lu,\n", (unsigned long)manager->current_video_file.metadata.fps);
        fprintf(meta_file, "  \"bitrate\": %lu,\n", (unsigned long)manager->current_video_file.metadata.bitrate);
        fprintf(meta_file, "  \"frame_count\": %lu,\n", (unsigned long)manager->current_video_file.metadata.frame_count);
        fprintf(meta_file, "  \"file_size\": %lu\n", (unsigned long)manager->current_video_file.metadata.file_size);
        fprintf(meta_file, "}\n");
        fclose(meta_file);
    }

    ESP_LOGI(TAG, "Closed video file: %s, size: %llu bytes, frames: %lu",
             filename, file_size,
             (unsigned long)manager->current_video_file.frame_count);

    // 重置当前文件
    manager->current_video_file.filename[0] = '\0';
    manager->current_video_file.full_path[0] = '\0';
    manager->current_video_file.muxer = NULL;
    manager->current_video_file.frame_count = 0;

    // 更新存储统计
    update_storage_stats(manager);

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_save_metadata(
    storage_manager_handle_t handle,
    const char *filename,
    const video_metadata_t *metadata)
{
    // 已经在close_video_file中实现
    return ESP_OK;
}

esp_err_t storage_manager_get_metadata(
    storage_manager_handle_t handle,
    const char *filename,
    video_metadata_t *metadata)
{
    if (!handle || !filename || !metadata) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 构建元数据文件路径
    char meta_path[128];
    get_full_path(manager, manager->config.metadata_subdir, filename, meta_path, sizeof(meta_path));

    // 修改扩展名为.json
    char *dot = strrchr(meta_path, '.');
    if (dot) {
        strcpy(dot, ".json");
    }

    // 检查文件是否存在
    struct stat st;
    if (stat(meta_path, &st) != 0) {
        ESP_LOGE(TAG, "Metadata file not found: %s", meta_path);
        xSemaphoreGive(manager->lock);
        return ESP_ERR_NOT_FOUND;
    }

    // 简单行解析器替代cJSON（离线环境下cjson组件不可用）
    metadata->filename[0] = '\0';

    FILE *meta_file = fopen(meta_path, "r");
    if (!meta_file) {
        ESP_LOGE(TAG, "Failed to open metadata file: %s", meta_path);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    char line[256];
    while (fgets(line, sizeof(line), meta_file)) {
        // 跳过空行和括号
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '{' || *p == '}') continue;

        // 提取键
        char *key_start = strchr(p, '"');
        if (!key_start) continue;
        key_start++;
        char *key_end = strchr(key_start, '"');
        if (!key_end) continue;
        *key_end = '\0';

        // 提取值
        char *val_start = strchr(key_end + 1, ':');
        if (!val_start) continue;
        val_start++;
        while (*val_start == ' ' || *val_start == '\t') val_start++;

        if (*val_start == '"') {
            // 字符串值
            val_start++;
            char *val_end = strchr(val_start, '"');
            if (!val_end) continue;
            *val_end = '\0';

            if (strcmp(key_start, "filename") == 0) {
                strncpy(metadata->filename, val_start, sizeof(metadata->filename) - 1);
            } else if (strcmp(key_start, "create_time") == 0) {
                strncpy(metadata->create_time, val_start, sizeof(metadata->create_time) - 1);
            }
        } else if (*val_start >= '0' && *val_start <= '9') {
            // 数字值
            int int_val = atoi(val_start);
            if (strcmp(key_start, "duration_ms") == 0) {
                metadata->duration_ms = (uint32_t)int_val;
            } else if (strcmp(key_start, "width") == 0) {
                metadata->width = (uint32_t)int_val;
            } else if (strcmp(key_start, "height") == 0) {
                metadata->height = (uint32_t)int_val;
            } else if (strcmp(key_start, "fps") == 0) {
                metadata->fps = (uint32_t)int_val;
            } else if (strcmp(key_start, "bitrate") == 0) {
                metadata->bitrate = (uint32_t)int_val;
            } else if (strcmp(key_start, "frame_count") == 0) {
                metadata->frame_count = (uint32_t)int_val;
            } else if (strcmp(key_start, "file_size") == 0) {
                metadata->file_size = (uint32_t)int_val;
            }
        }
    }
    fclose(meta_file);

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_list_video_files(
    storage_manager_handle_t handle,
    int max_files,
    video_metadata_t *file_list,
    int *file_count)
{
    if (!handle || !file_count) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    char video_path[128];
    make_dir_path(manager, manager->config.video_subdir, video_path, sizeof(video_path));

    DIR *dir = opendir(video_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open video directory: %s", video_path);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_files) {
        if (entry->d_type == DT_REG) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".mp4") == 0) {
                if (file_list && count < max_files) {
                    // 获取元数据
                    storage_manager_get_metadata(handle, entry->d_name, &file_list[count]);
                    count++;
                } else if (!file_list) {
                    count++;
                }
            }
        }
    }

    closedir(dir);

    *file_count = count;

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_delete_video_file(
    storage_manager_handle_t handle,
    const char *filename)
{
    if (!handle || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 删除视频文件
    char video_path[128];
    get_full_path(manager, manager->config.video_subdir, filename, video_path, sizeof(video_path));

    if (unlink(video_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete video file: %s", video_path);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    // 删除元数据文件
    char meta_path[128];
    get_full_path(manager, manager->config.metadata_subdir, filename, meta_path, sizeof(meta_path));

    // 修改扩展名为.json
    char *dot = strrchr(meta_path, '.');
    if (dot) {
        strcpy(dot, ".json");
        unlink(meta_path);  // 忽略错误，可能文件不存在
    }

    ESP_LOGI(TAG, "Deleted video file: %s", filename);

    // 更新存储统计
    update_storage_stats(manager);

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_cleanup(storage_manager_handle_t handle, uint64_t *freed_bytes)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 先更新统计信息
    update_storage_stats(manager);

    uint64_t freed = 0;

    // 检查是否需要清理
    if (manager->stats.usage_percentage < manager->config.cleanup_threshold &&
        manager->stats.video_files <= manager->config.max_files) {
        xSemaphoreGive(manager->lock);
        if (freed_bytes) *freed_bytes = 0;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Storage cleanup triggered: %.1f%% used, %d files",
             manager->stats.usage_percentage, manager->stats.video_files);

    // 构建视频目录路径，按文件修改时间排序删除旧文件
    char video_path[128];
    make_dir_path(manager, manager->config.video_subdir, video_path, sizeof(video_path));

    // 收集所有.meta文件按时间排序
    typedef struct {
        char filename[64];
        time_t mtime;
    } file_entry_t;

    int max_entries = 256;
    file_entry_t *entries = calloc(max_entries, sizeof(file_entry_t));
    if (!entries) {
        xSemaphoreGive(manager->lock);
        return ESP_ERR_NO_MEM;
    }

    int entry_count = 0;
    DIR *dir = opendir(video_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && entry_count < max_entries) {
            if (entry->d_type == DT_REG) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcmp(ext, ".mp4") == 0) {
                    char fullpath[192];
                    int ret = snprintf(fullpath, sizeof(fullpath), "%s/%s", video_path, entry->d_name);
                    if (ret < 0 || (size_t)ret >= sizeof(fullpath)) {
                        ESP_LOGW(TAG, "fullpath truncated");
                        continue;
                    }

                    struct stat st;
                    if (stat(fullpath, &st) != 0) {
                        continue;
                    }

                    strncpy(entries[entry_count].filename, entry->d_name, sizeof(entries[entry_count].filename) - 1);
                    entries[entry_count].filename[sizeof(entries[entry_count].filename) - 1] = '\0';
                    entries[entry_count].mtime = st.st_mtime;
                    entry_count++;
                }
            }
        }
        closedir(dir);
    }

    // 按修改时间排序（最旧的在前面）
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = i + 1; j < entry_count; j++) {
            if (entries[j].mtime < entries[i].mtime) {
                file_entry_t tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    // 删除最旧的文件直到满足条件
    time_t now = time(NULL);
    int delete_target = entry_count - (int)manager->config.min_keep_files;
    if (delete_target < 1 && manager->stats.usage_percentage < manager->config.cleanup_threshold) {
        free(entries);
        xSemaphoreGive(manager->lock);
        if (freed_bytes) *freed_bytes = 0;
        return ESP_OK;
    }

    for (int i = 0; i < entry_count && i < delete_target; i++) {
        // 检查文件是否保留期内
        double days_old = difftime(now, entries[i].mtime) / (60 * 60 * 24);
        if (days_old < manager->config.min_keep_days &&
            manager->stats.video_files <= manager->config.min_keep_files) {
            break;  // 保留足够的新文件
        }

        char fullpath[192];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", video_path, entries[i].filename);

        uint64_t file_size = get_file_size(fullpath);
        if (unlink(fullpath) == 0) {
            freed += file_size;
            manager->stats.video_files--;

            // 删除对应元数据
            char meta_path[192];
            snprintf(meta_path, sizeof(meta_path), "%s/%s", video_path, entries[i].filename);
            char *dot = strrchr(meta_path, '.');
            if (dot) {
                strcpy(dot, ".json");
                char meta_fullpath[256];
                get_full_path(manager, manager->config.metadata_subdir,
                             strrchr(meta_path, '/') + 1, meta_fullpath, sizeof(meta_fullpath));
                unlink(meta_fullpath);
            }

            ESP_LOGI(TAG, "Cleaned up old file: %s (%llu bytes)", entries[i].filename, file_size);
        }
    }

    free(entries);

    // 更新统计信息
    update_storage_stats(manager);

    ESP_LOGI(TAG, "Storage cleanup done: freed %llu bytes, remaining: %.1f%% used, %d files",
             freed, manager->stats.usage_percentage, manager->stats.video_files);

    xSemaphoreGive(manager->lock);

    if (freed_bytes) {
        *freed_bytes = freed;
    }
    return ESP_OK;
}

esp_err_t storage_manager_write_log(
    storage_manager_handle_t handle,
    uint8_t level,
    const char *module,
    const char *message)
{
    if (!handle || !message) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 生成日志文件名（按日期）
    char date_str[16];
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(date_str, sizeof(date_str), "%Y%m%d", &timeinfo);

    char log_filename[64];
    snprintf(log_filename, sizeof(log_filename), "%s.log", date_str);

    char log_path[128];
    if (get_full_path(manager, manager->config.log_subdir, log_filename, log_path, sizeof(log_path)) != ESP_OK) {
        xSemaphoreGive(manager->lock);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *log_file = fopen(log_path, "a");
    if (!log_file) {
        ESP_LOGE(TAG, "Failed to open log file: %s", log_path);
        xSemaphoreGive(manager->lock);
        return ESP_FAIL;
    }

    // 生成时间戳
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // 写入日志
    const char *level_str;
    switch (level) {
        case 0: level_str = "DEBUG"; break;
        case 1: level_str = "INFO"; break;
        case 2: level_str = "WARN"; break;
        case 3: level_str = "ERROR"; break;
        default: level_str = "UNKNOWN"; break;
    }

    fprintf(log_file, "[%s] [%s] [%s] %s\n",
            time_str, level_str, module ? module : "system", message);

    fclose(log_file);

    xSemaphoreGive(manager->lock);
    return ESP_OK;
}

esp_err_t storage_manager_check_space(
    storage_manager_handle_t handle,
    uint64_t required_bytes,
    uint64_t *available_bytes)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 更新统计信息
    update_storage_stats(manager);

    if (available_bytes) {
        *available_bytes = manager->stats.free_space_bytes;
    }

    bool space_available = (manager->stats.free_space_bytes >= required_bytes);

    xSemaphoreGive(manager->lock);

    return space_available ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t storage_manager_format(storage_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // 警告：格式化会删除所有数据
    ESP_LOGW(TAG, "Formatting storage - ALL DATA WILL BE LOST!");

    // TODO: 实现格式化
    // 需要卸载文件系统，格式化，然后重新挂载

    ESP_LOGW(TAG, "Storage format not yet implemented");

    return ESP_OK;
}

esp_err_t storage_manager_deinit(storage_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_t *manager = (storage_manager_t *)handle;

    xSemaphoreTake(manager->lock, portMAX_DELAY);

    // 关闭当前文件
    if (manager->current_video_file.muxer) {
        esp_muxer_close(manager->current_video_file.muxer);
        manager->current_video_file.muxer = NULL;
        manager->current_video_file.stream_added = false;
    }

    // 卸载文件系统 / 释放硬件
    if (manager->sd_card_mounted) {
        if (manager->own_hardware) {
            bsp_sdcard_unmount();
        }
        manager->sd_card_mounted = false;
    }

    manager->sd_card = NULL;

    // 删除互斥锁
    if (manager->lock) {
        vSemaphoreDelete(manager->lock);
    }

    free(manager);
    return ESP_OK;
}