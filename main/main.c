/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file main.c
 * @brief 宠语者 - 智能宠物箱主程序 (v2 优化版)
 *
 * 基于 ESP32-P4，利用硬件 H.264 编码器输出运动矢量实现零开销运动感知，
 * 自动触发高清录像并存储至 SD 卡。
 *
 * 优化要点:
 *   - 摄像头优先 YUV420 格式,省去 RGB→YUV 的 PPA 颜色转换
 *   - 4 DMA 缓冲区替代 2,降低掉帧风险
 *   - 帧率统计实时输出
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "driver/ppa.h"
#include "esp_private/esp_cache_private.h"
#include "esp_cache.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_param_hw.h"
#include "esp_h264_alloc.h"
#include "wifi_manager.h"
#include "app_video.h"
#include "storage_manager.h"
#include "snake_detect.h"
#include "activity_tracker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

/* 异步 SD 写入 —— 解耦编码 pipeline 与 SD 卡写入延迟 */
#define ASYNC_WRITE_BUF_COUNT 5
#define ASYNC_WRITE_BUF_SIZE  (1536 * 1024)  /* 1.5MB 足够最大 IDR 帧 */

/* 调试: 设为 1 用合成彩条替代前10帧O_UYY_E_VYY数据, 验证编码器输入格式 */
#define DEBUG_SYNTHETIC_TEST 0

/* 录像开关: 1=启用SD卡录像, 0=禁用 (运动检测不受影响) */
#define RECORDING_ENABLED 0

/* ========================================================================== */
/* 系统状态                                                                  */
/* ========================================================================== */

typedef enum {
    SYS_STATE_MONITOR,      /* MV 监控，不录像，不 AI */
    SYS_STATE_AI_VERIFY,    /* MV 触发 → AI 确认蛇存在 (每帧推理, 连续2帧命中) */
    SYS_STATE_RECORDING,    /* AI 确认 → 活动追踪 + 双重检测 (暂不开录像) */
} sys_state_t;

/* 固定分辨率 —— MON 和 REC 统一为 800x800, MV 宏块数从 300 升到 2500 */
#define CAM_WIDTH    800
#define CAM_HEIGHT   800
#define MON_WIDTH    800
#define MON_HEIGHT   800

/* 摄像头 DMA 缓冲区数量 */
#define CAM_BUF_NUM  4

/* H.264 监控模式编码配置 (800x800, 高 QP 抑制噪声利于 MV 检测) */
static const esp_h264_enc_cfg_hw_t monitor_enc_cfg = {
    .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
    .gop      = 10,
    .fps      = 10,
    .res      = { .width = MON_WIDTH, .height = MON_HEIGHT },
    .rc       = { .bitrate = 500000, .qp_min = 30, .qp_max = 42 },
};

/* H.264 录像模式编码配置 */
static const esp_h264_enc_cfg_hw_t record_enc_cfg = {
    .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
    .gop      = 15,
    .fps      = 20,
    .res      = { .width = CAM_WIDTH, .height = CAM_HEIGHT },
    .rc       = { .bitrate = 2000000, .qp_min = 18, .qp_max = 35 },
};

static const char *TAG = "pet_whisperer";

/* ========================================================================== */
/* 全局变量                                                                  */
/* ========================================================================== */

static sys_state_t        g_state = SYS_STATE_MONITOR;

/* 摄像头实际输出格式（YUV420 或回退到 RGB565） */
static video_fmt_t        g_camera_fmt = APP_VIDEO_FMT_RGB565;

/* H.264 编码器 */
static esp_h264_enc_handle_t           g_h264_enc = NULL;
static esp_h264_enc_param_hw_handle_t  g_h264_param = NULL;
static esp_h264_enc_mvm_pkt_t          g_mv_pkt = {0};
static uint8_t                        *g_enc_ouev_buf = NULL;     /* O_UYY_E_VYY 编码输入 */
static size_t                          g_enc_ouev_buf_size = 0;
static uint8_t                        *g_enc_out_buf = NULL;      /* H.264 编码输出 */
static size_t                          g_enc_out_buf_size = 0;

/* PPA 客户端 */
static ppa_client_handle_t g_ppa_display = NULL;   /* 显示用 (RGB/RGB→RGB) */
static ppa_client_handle_t g_ppa_encode = NULL;    /* 编码用 (RGB/YUV→YUV) */
static SemaphoreHandle_t   g_ppa_mutex = NULL;     /* 跨核保护 PPA 硬件 */

/* 缓冲区 */
static uint8_t  *g_lcd_rgb_buf = NULL;     /* RGB565 LCD 画布 */

/* 管理器句柄 */
static storage_manager_handle_t g_storage = NULL;
/* LVGL */
static lv_obj_t *g_camera_canvas = NULL;
static size_t    g_cache_line_size = 0;

/* 录像文件 */
static char   g_current_filename[64];
static bool   g_file_open = false;

/* 延迟状态切换 */
static sys_state_t g_pending_state = SYS_STATE_MONITOR;
static bool        g_has_pending_switch = false;

/* ========================================================================== */
/* 异步 SD 写入 —— 解耦 pipeline 与 SD 延迟                                  */
/* ========================================================================== */

typedef struct {
    size_t    len;
    uint64_t  timestamp;
    bool      is_keyframe;
    char      filename[64];
    uint8_t  *pool_buf;        /* PSRAM heap, ASYNC_WRITE_BUF_SIZE */
} async_write_job_t;

static async_write_job_t g_async_jobs[ASYNC_WRITE_BUF_COUNT];
static QueueHandle_t     g_async_free_queue = NULL;   /* 空闲 buffer 索引队列 */
static QueueHandle_t     g_async_ready_queue = NULL;  /* 就绪待写 buffer 索引队列 */
static TaskHandle_t      g_async_writer_task = NULL;

static void async_sd_writer_task(void *arg)
{
    int idx;
    while (1) {
        if (xQueueReceive(g_async_ready_queue, &idx, portMAX_DELAY) == pdTRUE) {
            async_write_job_t *job = &g_async_jobs[idx];
            storage_manager_write_video_frame(
                g_storage, job->filename,
                job->pool_buf, job->len,
                job->timestamp, job->is_keyframe);
            /* 写完后归还 buffer 到空闲队列 */
            xQueueSend(g_async_free_queue, &idx, 0);
        }
    }
}

/* ========================================================================== */
/* 帧队列流水线: 相机回调 → 帧池 → 编码处理 task → SD 写入 task                */
/* ========================================================================== */

#define FRAME_POOL_COUNT 5
#define FRAME_POOL_SIZE   ALIGN_UP(CAM_WIDTH * CAM_HEIGHT * 3 / 2, 64)

typedef struct {
    uint8_t  *i420_data;   /* 帧池中 I420 数据指针 */
    int       pool_idx;     /* 帧池索引 */
    uint32_t  enc_w, enc_h;
    uint32_t  timestamp;
    bool      do_display;
    ppa_srm_color_mode_t     in_cm;
    ppa_srm_rotation_angle_t rotation;
} frame_job_t;

static uint8_t     *g_frame_pool[FRAME_POOL_COUNT];
static QueueHandle_t g_frame_free_queue  = NULL;
static QueueHandle_t g_frame_ready_queue = NULL;
static TaskHandle_t  g_encode_task = NULL;

/* 处理 task 帧计数器 */
static int g_encode_frame_count = 0;

/* 两级 MV 运动检测:
 *   弱噪声: mag < 4     → 丢弃
 *   强信号: mag >= 6    → 用于 trigger/sustain 判决
 *   800x800 噪声 ~1500 块 (mag 2-5), 手 ~200 强块 (mag 6+)
 */
#define MV_NOISE_THRESHOLD        5    /* mag < 5 丢弃 */
#define MV_STRONG_THRESHOLD       8    /* mag >= 8 为强运动块 */
#define MOTION_TRIGGER_STRONG     200  /* 强运动块数 > 200 触发 */
#define MOTION_TRIGGER_AVG        25
#define MOTION_TRIGGER_FRAMES      5   /* 连续 5 帧触发 */
#define MOTION_SUSTAIN_STRONG     100  /* 强运动块数 > 100 续录 */
#define MOTION_SUSTAIN_MV_COUNT   500
#define MOTION_SUSTAIN_AVG        15
#define RECORDING_CHECK_INTERVAL  10000000  /* 10 秒 */

static int     g_motion_trigger_count = 0;
static int64_t g_last_motion_check_us = 0;

/* AI_VERIFY 态变量 */
static int     g_ai_verify_count = 0;       /* 连续 AI 确认帧数 */
static int     g_ai_verify_total = 0;       /* AI_VERIFY 态已运行帧数 */
#define AI_VERIFY_CONFIRM_FRAMES  2          /* 连续确认帧数 → 进入 RECORDING */
#define AI_VERIFY_MAX_FRAMES     10          /* 超时帧数 → 退回 MONITOR */

/* RECORDING 态双重检测变量 */
static int     g_dual_check_frame_count = 0; /* 帧计数器，用于每秒一次 AI */
#define DUAL_CHECK_INTERVAL_FRAMES 60        /* ~5秒 (12fps) */

/* 前向声明 —— 函数定义在文件后面 */
static esp_err_t ppa_display_process(uint8_t *in_buf, ppa_srm_color_mode_t in_cm,
                                      uint32_t in_w, uint32_t in_h,
                                      uint8_t *rgb_out, uint32_t out_w, uint32_t out_h,
                                      size_t out_buf_size, ppa_srm_rotation_angle_t rotation);
static esp_err_t switch_state(sys_state_t new_state);
static esp_err_t stop_recording(void);
#if DEBUG_SYNTHETIC_TEST
static void fill_color_bars_ouev(uint8_t *ouev, uint32_t w, uint32_t h);
#endif

static void encode_processor_task(void *arg)
{
    frame_job_t job;
    while (1) {
        if (xQueueReceive(g_frame_ready_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        g_encode_frame_count++;

        int64_t t_proc_start = esp_timer_get_time();

        /* ---- 0. 延迟状态切换 ---- */
        if (g_has_pending_switch) {
            g_has_pending_switch = false;
            sys_state_t target = g_pending_state;

            /* MONITOR → AI_VERIFY: 同编码器配置, 直接切状态 */
            if (g_state == SYS_STATE_MONITOR && target == SYS_STATE_AI_VERIFY) {
                g_state = SYS_STATE_AI_VERIFY;
                g_ai_verify_count = 0;
                g_ai_verify_total = 0;
                g_last_motion_check_us = esp_timer_get_time();
                ESP_LOGI(TAG, "→ AI_VERIFY (confirming snake...)");
            }
            /* AI_VERIFY → RECORDING: 活动追踪, 不录像, 保持 monitor 编码器 (只需 MV) */
            else if (target == SYS_STATE_RECORDING) {
                g_state = SYS_STATE_RECORDING;
                uint32_t now_s = (uint32_t)time(NULL);
                activity_tracker_start_event(now_s);
                g_last_motion_check_us = esp_timer_get_time();
                g_motion_trigger_count = 0;
                g_dual_check_frame_count = 0;
                ESP_LOGI(TAG, "→ RECORDING (tracking started, no video)");
            }
            /* 任意态 → MONITOR (停止).
             * RECORDING 一直用的是 monitor 编码器, 不需重建, 直接改状态即可 */
            else if (target == SYS_STATE_MONITOR) {
                if (g_state == SYS_STATE_RECORDING) {
                    activity_tracker_end_event((uint32_t)time(NULL));
                }
                g_state = SYS_STATE_MONITOR;
                g_ai_verify_count = 0;
                g_ai_verify_total = 0;
                ESP_LOGI(TAG, "→ MONITOR");
            }
            else {
                switch_state(target);
            }
        }

        /* ---- 1. PPA 显示 (每 N 帧) ---- */
        if (job.do_display) {
            bsp_display_lock(0);
            ppa_display_process(job.i420_data, job.in_cm,
                                job.enc_w, job.enc_h,
                                g_lcd_rgb_buf,
                                BSP_LCD_H_RES, BSP_LCD_V_RES,
                                BSP_LCD_H_RES * BSP_LCD_V_RES * 2,
                                job.rotation);
            lv_canvas_set_buffer(g_camera_canvas, g_lcd_rgb_buf,
                                 BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565);
            lv_obj_center(g_camera_canvas);
            lv_obj_invalidate(g_camera_canvas);
            bsp_display_unlock();
        }
        int64_t t_ppa_disp = esp_timer_get_time();

        /* ---- 2. 帧池 (O_UYY_E_VYY) 直接喂 H.264, 零拷贝 ---- */
        /* PPA 输出 = O_UYY_E_VYY = H.264 输入, 无需格式转换也无需 memcpy.
         * H.264 编码器通过 DMA 读 PSRAM, 绕过 CPU cache, 直接读到 PPA 写入的数据.
         * ppa_yuv_copy 已完成 M2C, CPU 不在中间修改此 buffer, 无需额外缓存操作. */
        int64_t t_copy = esp_timer_get_time();

        /* ---- 3. H.264 硬件编码 (零拷贝: 直接从帧池 DMA 读取) ---- */
        esp_h264_enc_in_frame_t in_frame = {
            .raw_data.buffer = job.i420_data,
            .raw_data.len    = job.enc_w * job.enc_h * 3 / 2,
            .pts             = job.timestamp,
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data.buffer = g_enc_out_buf,
            .raw_data.len    = g_enc_out_buf_size,
        };

        esp_h264_enc_hw_set_mv_pkt(g_h264_param, g_mv_pkt);
        esp_h264_err_t h264_ret = esp_h264_enc_process(g_h264_enc, &in_frame, &out_frame);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "H.264 encode failed: %d", h264_ret);
            xQueueSend(g_frame_free_queue, &job.pool_idx, 0);
            continue;
        }
        int64_t t_h264 = esp_timer_get_time();

        /* 编码器 DMA 写输出后使 cache 无效，确保 CPU 读到最新数据 */
        if (out_frame.length > 0) {
            esp_cache_msync(g_enc_out_buf, g_enc_out_buf_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        }
        /* MV buffer: 编码器 DMA 写入 PSRAM, CPU cache 已过时, 必须 M2C 失效 */
        esp_cache_msync(g_mv_pkt.data, g_mv_pkt.len,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

        /* ---- 4. 两级 MV 分类: 弱噪声 vs 强运动 ---- */
        uint32_t mv_len = 0;
        esp_h264_enc_hw_get_mv_data_len(g_h264_param, &mv_len);

        uint32_t max_motion = 0, strong_mv = 0;
        if (mv_len > 0) {
            const esp_h264_enc_mv_data_t *mvs = (const esp_h264_enc_mv_data_t *)g_mv_pkt.data;
            for (uint32_t j = 0; j < mv_len; j++) {
                uint32_t mag = (uint32_t)(abs(mvs[j].mv_x) + abs(mvs[j].mv_y));
                if (mag < MV_NOISE_THRESHOLD) continue;      /* 丢弃弱噪声 */
                if (mag > max_motion) max_motion = mag;
                if (mag >= MV_STRONG_THRESHOLD) strong_mv++; /* 强运动块计数 */
            }

            static int debug_counter = 0;
            if (++debug_counter % 30 == 0) {
                ESP_LOGI(TAG, "MV: raw=%" PRIu32 " strong=%" PRIu32 " max=%" PRIu32
                         " | state=%s",
                         mv_len, strong_mv, max_motion,
                         g_state == SYS_STATE_MONITOR ? "MON" :
                         g_state == SYS_STATE_AI_VERIFY ? "AI_V" : "REC");
            }
        }

        /* ---- 5. MV 运动检测 (基于强运动块数 strong_mv) ---- */

        /* 5a. MONITOR: MV 触发 → AI_VERIFY */
        if (g_state == SYS_STATE_MONITOR) {
            if (strong_mv > MOTION_TRIGGER_STRONG) {
                g_motion_trigger_count++;
                if (g_motion_trigger_count >= MOTION_TRIGGER_FRAMES) {
                    ESP_LOGI(TAG, ">>> Motion trigger (strong=%" PRIu32 " x%d) → AI_VERIFY <<<",
                             strong_mv, g_motion_trigger_count);
                    g_motion_trigger_count = 0;
                    g_pending_state = SYS_STATE_AI_VERIFY;
                    g_has_pending_switch = true;
                }
            } else {
                g_motion_trigger_count = 0;
            }
        }

        /* 5b. AI_VERIFY: 每帧跑 AI 推理, 连续2帧确认蛇存在 → RECORDING */
        else if (g_state == SYS_STATE_AI_VERIFY) {
            snake_detect_result_t ai_result;
            esp_err_t ai_ret = snake_detect_process(job.i420_data, &ai_result);
            g_ai_verify_total++;

            if (ai_ret == ESP_OK && ai_result.has_snake && ai_result.confidence > 0.5f) {
                g_ai_verify_count++;
                if (g_ai_verify_count >= AI_VERIFY_CONFIRM_FRAMES) {
                    ESP_LOGI(TAG, ">>> AI snake confirmed (conf=%.2f, x%d/%d) → RECORDING <<<",
                             ai_result.confidence, g_ai_verify_count, g_ai_verify_total);
                    g_pending_state = SYS_STATE_RECORDING;
                    g_has_pending_switch = true;
                }
            } else {
                g_ai_verify_count = 0;
            }

            /* 超时: 10 帧内未确认 → 退回 MONITOR */
            if (g_ai_verify_total >= AI_VERIFY_MAX_FRAMES) {
                ESP_LOGI(TAG, ">>> AI verify timeout (%d frames) → MONITOR <<<",
                         g_ai_verify_total);
                g_pending_state = SYS_STATE_MONITOR;
                g_has_pending_switch = true;
            }
        }

        /* 5c. RECORDING: 每秒双重检测 (MV + AI), 每秒记录位置, 暂不录像 */
        else if (g_state == SYS_STATE_RECORDING) {
            g_dual_check_frame_count++;

            /* 每秒一次双重检测 */
            if (g_dual_check_frame_count >= DUAL_CHECK_INTERVAL_FRAMES) {
                g_dual_check_frame_count = 0;

                bool mv_pass = (strong_mv > MOTION_SUSTAIN_STRONG);

                snake_detect_result_t ai_result;
                esp_err_t ai_ret = snake_detect_process(job.i420_data, &ai_result);
                bool ai_pass = (ai_ret == ESP_OK && ai_result.has_snake && ai_result.confidence > 0.5f);

                if (mv_pass && ai_pass) {
                    g_last_motion_check_us = esp_timer_get_time();
                }

                ESP_LOGI(TAG, "Dual check: MV=%s AI=%s (conf=%.2f)",
                         mv_pass ? "Y" : "N", ai_pass ? "Y" : "N",
                         ai_result.confidence);
            }

            /* 双重检测 10 秒失败 → 停止追踪 */
            int64_t now = esp_timer_get_time();
            if (now - g_last_motion_check_us >= RECORDING_CHECK_INTERVAL) {
                ESP_LOGI(TAG, ">>> No activity for 10s → stopping tracking <<<");
                g_pending_state = SYS_STATE_MONITOR;
                g_has_pending_switch = true;
            }

            /* 每秒记录位置 (从 AI bbox 推算蛇头位置) */
            static int64_t g_last_pos_update_us = 0;
            if (now - g_last_pos_update_us >= 1000000) {
                g_last_pos_update_us = now;
                uint32_t now_s = (uint32_t)time(NULL);

                /* 位置用 bbox 中心（真实蛇头方向需要运动方向推算，延后） */
                snake_detect_result_t ai_result;
                if (snake_detect_process(job.i420_data, &ai_result) == ESP_OK && ai_result.has_snake) {
                    float head_x = ai_result.x + ai_result.w * 0.5f;
                    float head_y = ai_result.y + ai_result.h * 0.33f; /* bbox 前方 1/3 */
                    activity_tracker_update_position(now_s, head_x, head_y);
                }
            }
        }

        int64_t t_mv_proc = esp_timer_get_time();

        /* ---- 6. 录像态：异步写入 SD 卡 ---- */
#if RECORDING_ENABLED
        if (g_state == SYS_STATE_RECORDING && g_file_open && out_frame.length > 0) {
            bool is_keyframe = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                                out_frame.frame_type == ESP_H264_FRAME_TYPE_I);
            int idx;
            if (xQueueReceive(g_async_free_queue, &idx, 0) == pdTRUE) {
                async_write_job_t *wjob = &g_async_jobs[idx];
                memcpy(wjob->pool_buf, out_frame.raw_data.buffer, out_frame.length);
                wjob->len = out_frame.length;
                wjob->timestamp = job.timestamp;
                wjob->is_keyframe = is_keyframe;
                strncpy(wjob->filename, g_current_filename, sizeof(wjob->filename));
                wjob->filename[sizeof(wjob->filename) - 1] = '\0';
                xQueueSend(g_async_ready_queue, &idx, 0);
            }
        }
#endif

        int64_t t_sd = esp_timer_get_time();

        /* ---- 逐操作耗时 (每 30 帧输出) ---- */
        static int perf_counter = 0;
        if (++perf_counter % 30 == 0) {
            int64_t total = t_sd - t_proc_start;
            ESP_LOGI(TAG, "⏱ perf: total=%lldms | ppa_disp=%lld copy=%lld h264=%lld mv_proc=%lld sd_queue=%lld",
                     total / 1000,
                     (t_ppa_disp - t_proc_start) / 1000,
                     (t_copy - t_ppa_disp) / 1000,
                     (t_h264 - t_copy) / 1000,
                     (t_mv_proc - t_h264) / 1000,
                     (t_sd - t_mv_proc) / 1000);
        }

        /* 归还帧缓冲到池 */
        xQueueSend(g_frame_free_queue, &job.pool_idx, 0);
    }
}

/* 帧率统计 */
static int64_t g_fps_last_time = 0;
static int     g_fps_frame_count = 0;
static int     g_fps_display_count = 0;
static int     g_display_skip_count = 0;   /* 选择性显示计数器 */
#define DISPLAY_EVERY_N  1  /* 每N帧显示1次 */

#if DEBUG_SYNTHETIC_TEST
/* 生成 O_UYY_E_VYY 格式合成彩条测试帧:
 *   竖条: 白→黄→青→绿→红 (各 160px @800w)
 *   Y/U/V 值按 BT.601 全范围计算
 *   布局参照 esp_h264 官方测试代码 h264_io.c:read_enc_cb_420() */
static void fill_color_bars_ouev(uint8_t *ouev, uint32_t w, uint32_t h)
{
    static const uint8_t bar_y[5] = {255, 226, 179, 150,  76};
    static const uint8_t bar_u[5] = {128,   0, 171,  43,  85};
    static const uint8_t bar_v[5] = {128, 149,   0,  21, 255};
    uint32_t bar_w = w / 5;

    for (uint32_t row = 0; row < h; row += 2) {
        uint8_t *p_u_row = ouev + row * (w * 3 / 2);       /* 偶数行: UYY */
        uint8_t *p_v_row = ouev + (row + 1) * (w * 3 / 2);  /* 奇数行: VYY */

        for (uint32_t col = 0; col < w; col += 2) {
            int bar = col / bar_w;
            if (bar > 4) bar = 4;

            /* 偶数行: U + Y_left + Y_right */
            *p_u_row++ = bar_u[bar];
            *p_u_row++ = bar_y[bar];
            *p_u_row++ = bar_y[bar];

            /* 奇数行: V + Y_left + Y_right */
            *p_v_row++ = bar_v[bar];
            *p_v_row++ = bar_y[bar];
            *p_v_row++ = bar_y[bar];
        }
    }
}
#endif

/* ========================================================================== */
/* PPA 初始化与操作                                                          */
/* ========================================================================== */

static esp_err_t ppa_display_init(void)
{
    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
    return ppa_register_client(&cfg, &g_ppa_display);
}

static esp_err_t ppa_encode_init(void)
{
    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
    return ppa_register_client(&cfg, &g_ppa_encode);
}

/**
 * @brief PPA: RGB/YUV 输入 → RGB565 输出（缩放，用于 LCD 显示）
 */
static esp_err_t ppa_display_process(
    uint8_t *in_buf, ppa_srm_color_mode_t in_cm,
    uint32_t in_w, uint32_t in_h,
    uint8_t *rgb_out, uint32_t out_w, uint32_t out_h, size_t out_buf_size,
    ppa_srm_rotation_angle_t rotation)
{
    float scale_x = (float)out_w / in_w;
    float scale_y = (float)out_h / in_h;

    if (rotation == PPA_SRM_ROTATION_ANGLE_90 ||
        rotation == PPA_SRM_ROTATION_ANGLE_270) {
        scale_x = (float)out_h / in_w;
        scale_y = (float)out_w / in_h;
    }

    ppa_srm_oper_config_t cfg = {
        .in.buffer         = in_buf,
        .in.pic_w          = in_w,
        .in.pic_h          = in_h,
        .in.block_w        = in_w,
        .in.block_h        = in_h,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm         = in_cm,
        .out.buffer        = rgb_out,
        .out.buffer_size   = out_buf_size,
        .out.pic_w         = out_w,
        .out.pic_h         = out_h,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle    = rotation,
        .scale_x           = scale_x,
        .scale_y           = scale_y,
        .rgb_swap          = 0,
        .byte_swap         = 0,
        .mode              = PPA_TRANS_MODE_BLOCKING,
    };
    xSemaphoreTake(g_ppa_mutex, portMAX_DELAY);
    esp_err_t ret = ppa_do_scale_rotate_mirror(g_ppa_display, &cfg);
    xSemaphoreGive(g_ppa_mutex);
    return ret;
}

/**
 * @brief PPA: RGB/YUV 输入 → YUV420 I420（缩放 + 色域转换，用于编码器输入）
 */
static esp_err_t ppa_encode_scale(
    uint8_t *in_buf, ppa_srm_color_mode_t in_cm,
    uint32_t in_w, uint32_t in_h,
    uint8_t *yuv_out, uint32_t out_w, uint32_t out_h, size_t out_buf_size)
{
    ppa_srm_oper_config_t cfg = {
        .in.buffer         = in_buf,
        .in.pic_w          = in_w,
        .in.pic_h          = in_h,
        .in.block_w        = in_w,
        .in.block_h        = in_h,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm         = in_cm,
        .out.buffer        = yuv_out,
        .out.buffer_size   = out_buf_size,
        .out.pic_w         = out_w,
        .out.pic_h         = out_h,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm        = PPA_SRM_COLOR_MODE_YUV420,
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = (float)out_w / in_w,
        .scale_y           = (float)out_h / in_h,
        .rgb_swap          = 0,
        .byte_swap         = 0,
        .mode              = PPA_TRANS_MODE_BLOCKING,
    };
    xSemaphoreTake(g_ppa_mutex, portMAX_DELAY);
    esp_err_t ret = ppa_do_scale_rotate_mirror(g_ppa_encode, &cfg);
    xSemaphoreGive(g_ppa_mutex);
    if (ret == ESP_OK) {
        esp_cache_msync(yuv_out, out_buf_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    }
    return ret;
}

/**
 * @brief PPA: YUV420→YUV420 1:1 硬件 DMA 拷贝 (绕过 CPU uncached 读瓶颈)
 *
 * PPA 用内部 DMA 从 uncached camera buffer 高效读取，写入 cached PSRAM 目标，
 * CPU 后续从 cached 目标读取即可获得全速。~10ms vs CPU memcpy 84ms。
 */
static esp_err_t ppa_yuv_copy(uint8_t *in_buf, uint32_t w, uint32_t h,
                               uint8_t *yuv_out, size_t out_buf_size)
{
    ppa_srm_oper_config_t cfg = {
        .in.buffer         = in_buf,
        .in.pic_w          = w,
        .in.pic_h          = h,
        .in.block_w        = w,
        .in.block_h        = h,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm         = PPA_SRM_COLOR_MODE_YUV420,
        .out.buffer        = yuv_out,
        .out.buffer_size   = out_buf_size,
        .out.pic_w         = w,
        .out.pic_h         = h,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm        = PPA_SRM_COLOR_MODE_YUV420,
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = 1.0f,
        .scale_y           = 1.0f,
        .rgb_swap          = 0,
        .byte_swap         = 0,
        .mode              = PPA_TRANS_MODE_BLOCKING,
    };
    xSemaphoreTake(g_ppa_mutex, portMAX_DELAY);
    esp_err_t ret = ppa_do_scale_rotate_mirror(g_ppa_encode, &cfg);
    xSemaphoreGive(g_ppa_mutex);
    if (ret == ESP_OK) {
        /* PPA DMA 写入后需使 CPU cache 无效，确保 CPU 读取到最新数据 */
        esp_cache_msync(yuv_out, out_buf_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    }
    return ret;
}

/* ========================================================================== */
/* H.264 编码器管理                                                          */
/* ========================================================================== */

static esp_err_t init_h264_encoder(const esp_h264_enc_cfg_hw_t *cfg)
{
    esp_h264_err_t ret;

    ret = esp_h264_enc_hw_new(cfg, &g_h264_enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed: %d", ret);
        return ESP_FAIL;
    }

    ret = esp_h264_enc_hw_get_param_hd(g_h264_enc, &g_h264_param);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_get_param_hd failed: %d", ret);
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_FAIL;
    }

    esp_h264_enc_mv_cfg_t mv_cfg = {
        .mv_mode = ESP_H264_MVM_MODE_P16X16,
        .mv_fmt  = ESP_H264_MVM_FMT_ALL,
    };
    ret = esp_h264_enc_hw_cfg_mv(g_h264_param, mv_cfg);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_cfg_mv failed: %d", ret);
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_FAIL;
    }

    uint16_t mb_w = (cfg->res.width  + 15) >> 4;
    uint16_t mb_h = (cfg->res.height + 15) >> 4;
    g_mv_pkt.len = mb_w * mb_h * sizeof(esp_h264_enc_mv_data_t);
    g_mv_pkt.data = esp_h264_aligned_calloc(16, 1, g_mv_pkt.len,
                                             &g_mv_pkt.len, ESP_H264_MEM_INTERNAL);
    if (!g_mv_pkt.data) {
        ESP_LOGE(TAG, "Failed to allocate MV buffer");
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_enc_ouev_buf_size = ALIGN_UP(cfg->res.width, 16) *
                          ALIGN_UP(cfg->res.height, 16) * 3 / 2;
    g_enc_ouev_buf = heap_caps_aligned_alloc(g_cache_line_size,
                                              g_enc_ouev_buf_size,
                                              MALLOC_CAP_SPIRAM);
    if (!g_enc_ouev_buf) {
        ESP_LOGE(TAG, "Failed to allocate O_UYY_E_VYY encode buffer");
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_enc_out_buf_size = g_enc_ouev_buf_size;
    g_enc_out_buf = heap_caps_aligned_alloc(g_cache_line_size,
                                             g_enc_out_buf_size,
                                             MALLOC_CAP_SPIRAM);
    if (!g_enc_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate encode output buffer");
        heap_caps_free(g_enc_ouev_buf);
        g_enc_ouev_buf = NULL;
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_ERR_NO_MEM;
    }

    ret = esp_h264_enc_open(g_h264_enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed: %d", ret);
        heap_caps_free(g_enc_out_buf);
        g_enc_out_buf = NULL;
        heap_caps_free(g_enc_ouev_buf);
        g_enc_ouev_buf = NULL;
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "H.264 encoder: %" PRIu16 "x%" PRIu16 "@%" PRIu8 "fps "
             "(bitrate=%" PRIu32 " bps)",
             cfg->res.width, cfg->res.height, cfg->fps, cfg->rc.bitrate);
    return ESP_OK;
}

static void deinit_h264_encoder(void)
{
    if (g_h264_enc) {
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        g_h264_param = NULL;
    }
    if (g_mv_pkt.data) {
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
    }
    if (g_enc_ouev_buf) {
        heap_caps_free(g_enc_ouev_buf);
        g_enc_ouev_buf = NULL;
        g_enc_ouev_buf_size = 0;
    }
    if (g_enc_out_buf) {
        heap_caps_free(g_enc_out_buf);
        g_enc_out_buf = NULL;
        g_enc_out_buf_size = 0;
    }
}

/* ========================================================================== */
/* 系统状态切换                                                              */
/* ========================================================================== */

static esp_err_t switch_state(sys_state_t new_state)
{
    if (g_state == new_state) return ESP_OK;

    deinit_h264_encoder();

    const esp_h264_enc_cfg_hw_t *cfg =
        (new_state == SYS_STATE_MONITOR) ? &monitor_enc_cfg : &record_enc_cfg;

    esp_err_t ret = init_h264_encoder(cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch encoder state");
        return ret;
    }

    g_state = new_state;
    ESP_LOGI(TAG, "State → %s",
             new_state == SYS_STATE_MONITOR ? "MONITOR" :
             new_state == SYS_STATE_AI_VERIFY ? "AI_VERIFY" : "RECORDING");
    return ESP_OK;
}

/* ========================================================================== */
/* 录像文件管理                                                              */
/* ========================================================================== */

static esp_err_t stop_recording(void)
{
    if (!g_file_open) return ESP_OK;

    video_metadata_t meta = {0};
    snprintf(meta.filename, sizeof(meta.filename), "%s", g_current_filename);
    meta.width   = CAM_WIDTH;
    meta.height  = CAM_HEIGHT;
    meta.fps     = record_enc_cfg.fps;
    meta.bitrate = record_enc_cfg.rc.bitrate;

    esp_err_t ret = storage_manager_close_video_file(g_storage,
                                                      g_current_filename, &meta);
    g_file_open = false;
    g_current_filename[0] = '\0';
    ESP_LOGI(TAG, "Recording STOP");
    return ret;
}

/* ========================================================================== */
/* 帧处理回调                                                                */
/* ========================================================================== */

static void camera_frame_cb(
    uint8_t *camera_buf, uint8_t camera_buf_index,
    uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len)
{
    int64_t t_start = esp_timer_get_time();
    uint32_t timestamp = t_start / 1000;

    /* ---- 帧率统计 ---- */
    g_fps_frame_count++;
    g_fps_display_count++;
    int64_t now = t_start;
    if (now - g_fps_last_time >= 5000000) {
        float disp_fps = g_fps_display_count * 1000000.0f / (now - g_fps_last_time);
        ESP_LOGI(TAG, "FPS: %.1f (capture), state=%s, fmt=%s",
                 disp_fps,
                 g_state == SYS_STATE_MONITOR ? "MON" :
                 g_state == SYS_STATE_AI_VERIFY ? "AI_V" : "REC",
                 g_camera_fmt == APP_VIDEO_FMT_YUV420 ? "YUV420" : "RGB565");
        g_fps_frame_count = 0;
        g_fps_display_count = 0;
        g_fps_last_time = now;
    }

    /* ---- 分辨率 ---- */
    uint32_t enc_w = (g_state == SYS_STATE_MONITOR) ? MON_WIDTH  : CAM_WIDTH;
    uint32_t enc_h = (g_state == SYS_STATE_MONITOR) ? MON_HEIGHT : CAM_HEIGHT;

    /* ---- 颜色模式和旋转 ---- */
    ppa_srm_color_mode_t in_cm = (g_camera_fmt == APP_VIDEO_FMT_YUV420)
                                 ? PPA_SRM_COLOR_MODE_YUV420
                                 : PPA_SRM_COLOR_MODE_RGB565;
    ppa_srm_rotation_angle_t rotation = PPA_SRM_ROTATION_ANGLE_0;
    switch (BSP_CAMERA_ROTATION) {
    case 90:  rotation = PPA_SRM_ROTATION_ANGLE_90;  break;
    case 180: rotation = PPA_SRM_ROTATION_ANGLE_180; break;
    case 270: rotation = PPA_SRM_ROTATION_ANGLE_270; break;
    default:  break;
    }

    /* ---- 从帧池获取空闲 buffer ---- */
    int pool_idx;
    if (xQueueReceive(g_frame_free_queue, &pool_idx, 0) != pdTRUE) {
        /* 帧池耗尽 → 丢帧（编码 pipeline 落后于相机） */
        return;
    }

    /* ---- PPA DMA: camera (uncached) → 帧池 buffer (cached) ---- */
    uint8_t *dst = g_frame_pool[pool_idx];
    if (g_camera_fmt == APP_VIDEO_FMT_YUV420) {
        if (enc_w == camera_buf_hes && enc_h == camera_buf_ves) {
            ppa_yuv_copy(camera_buf, enc_w, enc_h, dst, enc_w * enc_h * 3 / 2);
        } else {
            ppa_encode_scale(camera_buf, PPA_SRM_COLOR_MODE_YUV420,
                             camera_buf_hes, camera_buf_ves,
                             dst, enc_w, enc_h, enc_w * enc_h * 3 / 2);
        }
    } else {
        ppa_encode_scale(camera_buf, PPA_SRM_COLOR_MODE_RGB565,
                         camera_buf_hes, camera_buf_ves,
                         dst, enc_w, enc_h, enc_w * enc_h * 3 / 2);
    }
    int64_t t_copy_done = esp_timer_get_time();

    /* ---- 本帧是否需要显示 ---- */
    g_display_skip_count++;
    bool do_disp = (g_display_skip_count % DISPLAY_EVERY_N == 0);

    /* ---- 构建 job 推入编码队列 ---- */
    frame_job_t job = {
        .i420_data = dst,
        .pool_idx  = pool_idx,
        .enc_w     = enc_w,
        .enc_h     = enc_h,
        .timestamp = timestamp,
        .do_display = do_disp,
        .in_cm      = in_cm,
        .rotation   = rotation,
    };
    xQueueSend(g_frame_ready_queue, &job, 0);

    /* ---- 相机回调耗时 ---- */
    static int cb_perf_counter = 0;
    if (++cb_perf_counter % 30 == 0) {
        int64_t cb_time = t_copy_done - t_start;
        ESP_LOGI(TAG, "⏱ cam_cb: %lldms (copy to pool)", cb_time / 1000);
    }
}

/* ========================================================================== */
/* 主入口                                                                    */
/* ========================================================================== */

void app_main(void)
{
    esp_err_t ret;

    /* ---- 显示 ---- */
    bsp_display_start();
    bsp_display_backlight_on();

    /* ---- 摄像头 ---- */
    bsp_camera_start(NULL);

    /* 获取缓存对齐 */
    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &g_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get cache alignment: 0x%x", ret);
        return;
    }

    /* ---- PPA 初始化 ---- */
    if (ppa_display_init() != ESP_OK || ppa_encode_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init PPA");
        return;
    }
    g_ppa_mutex = xSemaphoreCreateMutex();

    /* ---- AI 蛇检测 (骨架: PPA 缩放就绪, 推理模拟) ---- */
    if (snake_detect_init() != ESP_OK) {
        ESP_LOGW(TAG, "Snake detect init failed, AI features disabled");
    }

    /* ---- 分配缓冲区 ---- */
    size_t lcd_buf_size = ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * 2,
                                   g_cache_line_size);
    g_lcd_rgb_buf = heap_caps_aligned_calloc(g_cache_line_size, 1,
                                              lcd_buf_size, MALLOC_CAP_SPIRAM);
    if (!g_lcd_rgb_buf) {
        ESP_LOGE(TAG, "Failed to allocate LCD buffer");
        return;
    }

    /* ---- 帧池: cached PSRAM 缓冲 (相机→PPA DMA→帧池→编码 task) ---- */
    for (int i = 0; i < FRAME_POOL_COUNT; i++) {
        g_frame_pool[i] = heap_caps_aligned_calloc(g_cache_line_size, 1,
                                                    FRAME_POOL_SIZE,
                                                    MALLOC_CAP_SPIRAM);
        if (!g_frame_pool[i]) {
            ESP_LOGE(TAG, "Failed to allocate frame pool buffer %d", i);
            return;
        }
    }
    g_frame_free_queue = xQueueCreate(FRAME_POOL_COUNT, sizeof(int));
    g_frame_ready_queue = xQueueCreate(FRAME_POOL_COUNT, sizeof(frame_job_t));
    for (int i = 0; i < FRAME_POOL_COUNT; i++) {
        xQueueSend(g_frame_free_queue, &i, 0);
    }
    ESP_LOGI(TAG, "Frame pool ready: %d x %uKB (total %uKB)",
             FRAME_POOL_COUNT,
             (unsigned)(FRAME_POOL_SIZE / 1024),
             (unsigned)(FRAME_POOL_COUNT * FRAME_POOL_SIZE / 1024));

    /* ---- LVGL 画布 ---- */
    bsp_display_lock(0);
    g_camera_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(g_camera_canvas, g_lcd_rgb_buf,
                         BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(g_camera_canvas);
    bsp_display_unlock();

    /* ---- H.264 编码器（监控模式启动）---- */
    ret = init_h264_encoder(&monitor_enc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init H.264 encoder");
        return;
    }
    g_state = SYS_STATE_MONITOR;

    /* ---- SD 卡存储管理器 ---- */
    storage_manager_config_t storage_cfg = {
        .base_path       = "/sdcard",
        .video_subdir    = "videos",
        .metadata_subdir = "meta",
        .log_subdir      = "logs",
        .max_storage_bytes = 4ULL * 1024 * 1024 * 1024,
        .max_files       = 100,
        .auto_cleanup    = true,
        .cleanup_threshold = 90,
        .min_keep_files  = 10,
        .min_keep_days   = 7,
    };
    g_storage = storage_manager_init(&storage_cfg);
    if (!g_storage) {
        ESP_LOGW(TAG, "SD card init failed, some features disabled");
    }

    /* ---- 活动追踪器 ---- */
    activity_tracker_init();

    /* ---- 异步 SD 写入队列初始化 ---- */
#if RECORDING_ENABLED
    if (g_storage) {
        /* PSRAM heap alloc for async write buffers */
        for (int i = 0; i < ASYNC_WRITE_BUF_COUNT; i++) {
            g_async_jobs[i].pool_buf = heap_caps_aligned_calloc(
                g_cache_line_size, 1, ASYNC_WRITE_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (!g_async_jobs[i].pool_buf) {
                ESP_LOGE(TAG, "Failed to alloc async write buffer %d", i);
            }
        }
        g_async_free_queue = xQueueCreate(ASYNC_WRITE_BUF_COUNT, sizeof(int));
        g_async_ready_queue = xQueueCreate(ASYNC_WRITE_BUF_COUNT, sizeof(int));
        for (int i = 0; i < ASYNC_WRITE_BUF_COUNT; i++) {
            int idx = i;
            xQueueSend(g_async_free_queue, &idx, 0);
        }
        xTaskCreatePinnedToCore(async_sd_writer_task, "sd_writer",
                                4096, NULL, 2, &g_async_writer_task, 1);
        ESP_LOGI(TAG, "Async SD writer ready (%d x %dKB buffers, PSRAM)",
                 ASYNC_WRITE_BUF_COUNT, ASYNC_WRITE_BUF_SIZE / 1024);
    }
#endif

    /* ---- 编码处理 task (从帧队列取帧: memcpy + H264 + MV + 状态机) ---- */
    xTaskCreatePinnedToCore(encode_processor_task, "encode_proc",
                            8192, NULL, 5, &g_encode_task, 1);
    ESP_LOGI(TAG, "Encode processor task started on core 1");

    /* ---- WiFi 配网启动 (非阻塞, SoftAP + HTTP server) ---- */
    wifi_manager_init();

    /* ---- 打开摄像头：优先 YUV420，回退到 RGB565 ---- */
    video_fmt_t try_fmts[] = { APP_VIDEO_FMT_YUV420, APP_VIDEO_FMT_RGB565 };
    int fd = -1;
    const char *fmt_names[] = { "YUV420", "RGB565" };

    for (int i = 0; i < sizeof(try_fmts) / sizeof(try_fmts[0]); i++) {
        fd = app_video_open(BSP_CAMERA_DEVICE, try_fmts[i], CAM_WIDTH, CAM_HEIGHT);
        if (fd >= 0) {
            g_camera_fmt = try_fmts[i];
            ESP_LOGI(TAG, "Camera opened: %dx%d %s", CAM_WIDTH, CAM_HEIGHT, fmt_names[i]);
            break;
        }
        ESP_LOGW(TAG, "Camera format %s not supported, trying next...", fmt_names[i]);
    }

    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open video device (all formats failed)");
        return;
    }

    /* 设置 DMA 缓冲区 */
    ret = app_video_set_bufs(fd, CAM_BUF_NUM, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set video buffers: 0x%x", ret);
        return;
    }

    /* 注册帧回调 */
    ret = app_video_register_frame_operation_cb(camera_frame_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame callback: 0x%x", ret);
        return;
    }

    /* 启动视频流 */
    ret = app_video_stream_task_start(fd, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream: 0x%x", ret);
        return;
    }

    g_fps_last_time = esp_timer_get_time();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  宠语者 (PetWhisperer) v2 已启动");
    ESP_LOGI(TAG, "  摄像头: %dx%d %s (bufs=%d)",
             CAM_WIDTH, CAM_HEIGHT,
             g_camera_fmt == APP_VIDEO_FMT_YUV420 ? "YUV420" : "RGB565",
             CAM_BUF_NUM);
    ESP_LOGI(TAG, "  状态机: MONITOR → AI_VERIFY → RECORDING");
    ESP_LOGI(TAG, "  AI 蛇检测: 骨架就绪 (模拟推理)");
    ESP_LOGI(TAG, "  SD卡: %s", g_storage ? "已就绪" : "未就绪");
    ESP_LOGI(TAG, "  活动追踪: %s", "已就绪");
    ESP_LOGI(TAG, "  H.264编码器: %s", g_h264_enc ? "已就绪" : "故障");
    ESP_LOGI(TAG, "  WiFi: %s", wifi_is_connected() ? "已连接" : "配网模式");
    ESP_LOGI(TAG, "========================================");

    /* 阻塞等待停止信号 */
    app_video_wait_video_stop();

    /* 清理 */
    stop_recording();
    app_video_close(fd);
    deinit_h264_encoder();
    if (g_storage) storage_manager_deinit(g_storage);
}
