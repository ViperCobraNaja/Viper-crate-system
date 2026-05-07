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
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_enc_param_hw.h"
#include "esp_h264_alloc.h"
#include "app_video.h"
#include "motion_detector.h"
#include "video_recorder.h"
#include "storage_manager.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

/* ========================================================================== */
/* 系统状态                                                                  */
/* ========================================================================== */

typedef enum {
    SYS_STATE_MONITOR,      /* 低分辨率监控，不录像 */
    SYS_STATE_RECORDING,    /* 高分辨率录像 */
} sys_state_t;

/* 固定分辨率 —— 摄像头始终以录像分辨率运行 */
#define CAM_WIDTH    1280
#define CAM_HEIGHT   960
#define MON_WIDTH    320
#define MON_HEIGHT   240

/* 摄像头 DMA 缓冲区数量 */
#define CAM_BUF_NUM  4

/* H.264 监控模式编码配置 (HW -> O_UYY_E_VYY / NV12) */
static const esp_h264_enc_cfg_hw_t monitor_enc_cfg = {
    .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
    .gop      = 10,
    .fps      = 10,
    .res      = { .width = MON_WIDTH, .height = MON_HEIGHT },
    .rc       = { .bitrate = 256000, .qp_min = 24, .qp_max = 42 },
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
static uint8_t                        *g_enc_nv12_buf = NULL;     /* NV12 编码输入 */
static size_t                          g_enc_nv12_buf_size = 0;
static uint8_t                        *g_enc_out_buf = NULL;      /* H.264 编码输出 */
static size_t                          g_enc_out_buf_size = 0;

/* PPA 客户端 */
static ppa_client_handle_t g_ppa_display = NULL;   /* 显示用 (RGB/RGB→RGB) */
static ppa_client_handle_t g_ppa_encode = NULL;    /* 编码用 (RGB/YUV→YUV) */

/* 缓冲区 */
static uint8_t  *g_lcd_rgb_buf = NULL;     /* RGB565 LCD 画布 */
static uint8_t  *g_enc_yuv_buf = NULL;     /* PPA 输出的 YUV420 I420 */
static size_t    g_enc_yuv_buf_size = 0;

/* 管理器句柄 */
static storage_manager_handle_t g_storage = NULL;
static video_recorder_handle_t  g_recorder = NULL;

/* LVGL */
static lv_obj_t *g_camera_canvas = NULL;
static size_t    g_cache_line_size = 0;

/* 录像文件 */
static char   g_current_filename[64];
static bool   g_file_open = false;

/* 延迟状态切换 */
static sys_state_t g_pending_state = SYS_STATE_MONITOR;
static bool        g_has_pending_switch = false;

/* 自动测试录像标志（绕过 video_recorder 状态机检查） */
static bool g_auto_recording = false;

/* 帧率统计 */
static int64_t g_fps_last_time = 0;
static int     g_fps_frame_count = 0;
static int     g_fps_display_count = 0;

/* ========================================================================== */
/* 色彩格式转换: I420 → NV12 (O_UYY_E_VYY)                                   */
/* ========================================================================== */

static void i420_to_nv12(const uint8_t *i420, uint8_t *nv12,
                         uint32_t width, uint32_t height)
{
    uint32_t y_size  = width * height;
    uint32_t uv_size = y_size / 4;

    memcpy(nv12, i420, y_size);

    const uint8_t *u_plane = i420 + y_size;
    const uint8_t *v_plane = i420 + y_size + uv_size;
    uint8_t *uv_dst = nv12 + y_size;

    for (uint32_t i = 0; i < uv_size; i++) {
        uv_dst[i * 2]     = u_plane[i];
        uv_dst[i * 2 + 1] = v_plane[i];
    }
}

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
    return ppa_do_scale_rotate_mirror(g_ppa_display, &cfg);
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
    return ppa_do_scale_rotate_mirror(g_ppa_encode, &cfg);
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
                                             &g_mv_pkt.len, ESP_H264_MEM_SPIRAM);
    if (!g_mv_pkt.data) {
        ESP_LOGE(TAG, "Failed to allocate MV buffer");
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_enc_nv12_buf_size = ALIGN_UP(cfg->res.width, 16) *
                          ALIGN_UP(cfg->res.height, 16) * 3 / 2;
    g_enc_nv12_buf = heap_caps_aligned_alloc(g_cache_line_size,
                                              g_enc_nv12_buf_size,
                                              MALLOC_CAP_SPIRAM);
    if (!g_enc_nv12_buf) {
        ESP_LOGE(TAG, "Failed to allocate NV12 encode buffer");
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_enc_out_buf_size = g_enc_nv12_buf_size;
    g_enc_out_buf = heap_caps_aligned_alloc(g_cache_line_size,
                                             g_enc_out_buf_size,
                                             MALLOC_CAP_SPIRAM);
    if (!g_enc_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate encode output buffer");
        heap_caps_free(g_enc_nv12_buf);
        g_enc_nv12_buf = NULL;
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
        heap_caps_free(g_enc_nv12_buf);
        g_enc_nv12_buf = NULL;
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
        esp_h264_enc_close(g_h264_enc);
        esp_h264_enc_del(g_h264_enc);
        g_h264_enc = NULL;
        g_h264_param = NULL;
    }
    if (g_mv_pkt.data) {
        esp_h264_free(g_mv_pkt.data);
        g_mv_pkt.data = NULL;
    }
    if (g_enc_nv12_buf) {
        heap_caps_free(g_enc_nv12_buf);
        g_enc_nv12_buf = NULL;
        g_enc_nv12_buf_size = 0;
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
             new_state == SYS_STATE_MONITOR ? "MONITOR" : "RECORDING");
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
    uint32_t timestamp = esp_timer_get_time() / 1000;

    /* ---- 帧率统计 ---- */
    g_fps_frame_count++;
    g_fps_display_count++;
    int64_t now = esp_timer_get_time();
    if (now - g_fps_last_time >= 5000000) {  /* 每 5 秒输出 */
        float fps = g_fps_frame_count * 1000000.0f / (now - g_fps_last_time);
        float disp_fps = g_fps_display_count * 1000000.0f / (now - g_fps_last_time);
        ESP_LOGI(TAG, "FPS: %.1f (disp), state=%s, fmt=%s",
                 disp_fps,
                 g_state == SYS_STATE_MONITOR ? "MON" : "REC",
                 g_camera_fmt == APP_VIDEO_FMT_YUV420 ? "YUV420" : "RGB565");
        g_fps_frame_count = 0;
        g_fps_display_count = 0;
        g_fps_last_time = now;
    }

    /* 启动自动测试录像 */
    static int startup_frame_count = 0;
    if (startup_frame_count == 0) {
        startup_frame_count = 1;
    } else if (startup_frame_count == 20 && g_state == SYS_STATE_MONITOR && g_recorder) {
        ESP_LOGI(TAG, ">>> Startup auto-record triggered <<<");
        g_auto_recording = true;
        g_pending_state = SYS_STATE_RECORDING;
        g_has_pending_switch = true;
    } else if (startup_frame_count >= 80 && startup_frame_count < 81 &&
               g_state == SYS_STATE_RECORDING) {
        ESP_LOGI(TAG, ">>> Stopping auto-record <<<");
        g_auto_recording = false;
        stop_recording();
        g_pending_state = SYS_STATE_MONITOR;
        g_has_pending_switch = true;
    }
    if (startup_frame_count > 0 && startup_frame_count < 200) {
        startup_frame_count++;
    }

    /* ---- 0. 延迟状态切换 ---- */
    if (g_has_pending_switch) {
        g_has_pending_switch = false;
        switch_state(g_pending_state);

        if (g_state == SYS_STATE_RECORDING) {
            uint32_t ts = esp_timer_get_time() / 1000000;
            snprintf(g_current_filename, sizeof(g_current_filename),
                     "VID_%08" PRIu32 ".mp4", ts);

            esp_err_t ret = storage_manager_create_video_file(
                g_storage, g_current_filename,
                CAM_WIDTH, CAM_HEIGHT, record_enc_cfg.fps, record_enc_cfg.rc.bitrate);

            if (ret == ESP_OK) {
                g_file_open = true;
                ESP_LOGI(TAG, "Recording START: %s", g_current_filename);
            }
        }
    }

    /* 摄像头为 YUV420 时的输入色彩模式 */
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

    /* ---- 1. PPA: 显示路径 (→ RGB565 1024x600) ---- */
    bsp_display_lock(0);
    ppa_display_process(camera_buf, in_cm,
                        camera_buf_hes, camera_buf_ves,
                        g_lcd_rgb_buf, BSP_LCD_H_RES, BSP_LCD_V_RES,
                        BSP_LCD_H_RES * BSP_LCD_V_RES * 2, rotation);
    lv_canvas_set_buffer(g_camera_canvas, g_lcd_rgb_buf,
                         BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(g_camera_canvas);
    lv_obj_invalidate(g_camera_canvas);
    bsp_display_unlock();

    /* ---- 2. 编码路径 ---- */
    uint32_t enc_w, enc_h;
    if (g_state == SYS_STATE_MONITOR) {
        enc_w = MON_WIDTH;
        enc_h = MON_HEIGHT;
    } else {
        enc_w = CAM_WIDTH;
        enc_h = CAM_HEIGHT;
    }

    const uint8_t *i420_src;
    if (g_camera_fmt == APP_VIDEO_FMT_YUV420) {
        /* YUV420 摄像头: 直接使用摄像头缓冲区，仅需要缩放时过 PPA */
        if (enc_w == camera_buf_hes && enc_h == camera_buf_ves) {
            /* 录像模式: 同分辨率，直接转 NV12，无需 PPA */
            i420_src = camera_buf;
        } else {
            /* 监控模式: 需要 YUV→YUV 缩放 */
            ppa_encode_scale(camera_buf, PPA_SRM_COLOR_MODE_YUV420,
                             camera_buf_hes, camera_buf_ves,
                             g_enc_yuv_buf, enc_w, enc_h, g_enc_yuv_buf_size);
            i420_src = g_enc_yuv_buf;
        }
    } else {
        /* RGB565 摄像头: 需要 PPA RGB→YUV 色域转换 + 缩放 */
        ppa_encode_scale(camera_buf, PPA_SRM_COLOR_MODE_RGB565,
                         camera_buf_hes, camera_buf_ves,
                         g_enc_yuv_buf, enc_w, enc_h, g_enc_yuv_buf_size);
        i420_src = g_enc_yuv_buf;
    }

    /* ---- 3. I420 → NV12 ---- */
    i420_to_nv12(i420_src, g_enc_nv12_buf, enc_w, enc_h);

    /* ---- 4. H.264 硬件编码 ---- */
    esp_h264_enc_in_frame_t in_frame = {
        .raw_data.buffer = g_enc_nv12_buf,
        .raw_data.len    = g_enc_nv12_buf_size,
        .pts             = timestamp,
    };
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data.buffer = g_enc_out_buf,
        .raw_data.len    = g_enc_out_buf_size,
    };

    esp_h264_enc_hw_set_mv_pkt(g_h264_param, g_mv_pkt);

    esp_h264_err_t h264_ret = esp_h264_enc_process(g_h264_enc,
                                                    &in_frame, &out_frame);
    if (h264_ret != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "H.264 encode failed: %d", h264_ret);
        return;
    }

    /* ---- 5. 提取硬件运动矢量 ---- */
    uint32_t mv_len = 0;
    esp_h264_enc_hw_get_mv_data_len(g_h264_param, &mv_len);

    static int debug_counter = 0;
    if (++debug_counter % 50 == 0) {
        video_record_state_t dbg_vr_state = RECORD_STATE_IDLE;
        if (g_recorder) {
            video_recorder_get_status(g_recorder, &dbg_vr_state, NULL, NULL);
        }
        const char *vr_str[] = {"IDLE","PRE_REC","REC","POST_REC","WRITE","ERR"};

        if (mv_len > 0) {
            uint32_t max_motion = 0, total_motion = 0;
            const esp_h264_enc_mv_data_t *mvs = (const esp_h264_enc_mv_data_t *)g_mv_pkt.data;
            for (uint32_t j = 0; j < mv_len; j++) {
                uint32_t m = abs(mvs[j].mv_x) + abs(mvs[j].mv_y);
                if (m > max_motion) max_motion = m;
                total_motion += m;
            }
            ESP_LOGI(TAG, "MV:%" PRIu32 " max=%" PRIu32 " avg=%.1f | rec:%s file:%s",
                     mv_len, max_motion,
                     mv_len > 0 ? (float)total_motion / mv_len : 0.0f,
                     vr_str[dbg_vr_state <= RECORD_STATE_ERROR ? dbg_vr_state : 0],
                     g_file_open ? "OPEN" : "-");
        } else {
            ESP_LOGW(TAG, "MV:0 (no data!) | rec:%s file:%s",
                     vr_str[dbg_vr_state <= RECORD_STATE_ERROR ? dbg_vr_state : 0],
                     g_file_open ? "OPEN" : "-");
        }
    }

    if (mv_len > 0) {
        if (g_recorder) {
            video_recorder_process_hw_motion_vectors(
                g_recorder, (const uint32_t *)g_mv_pkt.data,
                mv_len, enc_w, enc_h, timestamp);

            video_record_state_t vr_state;
            video_recorder_get_status(g_recorder, &vr_state, NULL, NULL);

            if (vr_state == RECORD_STATE_RECORDING &&
                g_state == SYS_STATE_MONITOR) {
                ESP_LOGI(TAG, ">>> Motion trigger → switching to RECORD <<<");
                g_pending_state = SYS_STATE_RECORDING;
                g_has_pending_switch = true;
            }
        }
    }

    /* 检查录像停止条件 */
    if (g_state == SYS_STATE_RECORDING && g_file_open && g_recorder && !g_auto_recording) {
        video_record_state_t vr_state;
        video_recorder_get_status(g_recorder, &vr_state, NULL, NULL);

        if (vr_state == RECORD_STATE_IDLE) {
            stop_recording();
            g_pending_state = SYS_STATE_MONITOR;
            g_has_pending_switch = true;
        }
    }

    /* ---- 6. 录像态：写入 SD 卡 ---- */
    if (g_state == SYS_STATE_RECORDING && g_file_open && out_frame.length > 0) {
        static int write_counter = 0;
        write_counter++;
        if (write_counter % 50 == 1) {
            ESP_LOGI(TAG, "SD write: %s frame=%d size=%" PRIu32,
                     g_current_filename, write_counter, out_frame.length);
        }
        bool is_keyframe = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                            out_frame.frame_type == ESP_H264_FRAME_TYPE_I);
        storage_manager_write_video_frame(
            g_storage, g_current_filename,
            out_frame.raw_data.buffer, out_frame.length,
            timestamp, is_keyframe);
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

    /* ---- 分配缓冲区 ---- */
    size_t lcd_buf_size = ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * 2,
                                   g_cache_line_size);
    g_lcd_rgb_buf = heap_caps_aligned_calloc(g_cache_line_size, 1,
                                              lcd_buf_size, MALLOC_CAP_SPIRAM);
    if (!g_lcd_rgb_buf) {
        ESP_LOGE(TAG, "Failed to allocate LCD buffer");
        return;
    }

    g_enc_yuv_buf_size = ALIGN_UP(CAM_WIDTH * CAM_HEIGHT * 3 / 2,
                                  g_cache_line_size);
    g_enc_yuv_buf = heap_caps_aligned_calloc(g_cache_line_size, 1,
                                              g_enc_yuv_buf_size,
                                              MALLOC_CAP_SPIRAM);
    if (!g_enc_yuv_buf) {
        ESP_LOGE(TAG, "Failed to allocate encode YUV buffer");
        return;
    }

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
        ESP_LOGW(TAG, "SD card init failed, recording disabled");
    }

    /* ---- 录像器 ---- */
    video_record_config_t vr_cfg = {
        .monitor = {
            .width   = MON_WIDTH,
            .height  = MON_HEIGHT,
            .fps     = monitor_enc_cfg.fps,
            .bitrate = monitor_enc_cfg.rc.bitrate,
            .format  = 0,
        },
        .recording = {
            .width   = CAM_WIDTH,
            .height  = CAM_HEIGHT,
            .fps     = record_enc_cfg.fps,
            .bitrate = record_enc_cfg.rc.bitrate,
            .max_duration_ms  = 5 * 60 * 1000,
            .post_trigger_ms  = 3000,
        },
        .storage = {
            .base_path      = "/sdcard/videos",
            .max_files      = 100,
            .max_storage_mb = 4096,
            .auto_delete    = true,
        },
        .motion_config = {
            .threshold          = 5,
            .min_area           = 1,
            .cooldown_ms        = 2000,
            .trigger_frames     = 1,
            .pre_trigger_frames = 10,
            .enable             = true,
            .grid_cols          = MOTION_GRID_COLS,
            .grid_rows          = MOTION_GRID_ROWS,
        },
    };
    g_recorder = video_recorder_init(&vr_cfg);
    if (!g_recorder) {
        ESP_LOGW(TAG, "Video recorder init failed");
    }

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
    ESP_LOGI(TAG, "  监控: %dx%d@%dfps → 运动触发 → 录像: %dx%d@%dfps",
             MON_WIDTH, MON_HEIGHT, monitor_enc_cfg.fps,
             CAM_WIDTH, CAM_HEIGHT, record_enc_cfg.fps);
    ESP_LOGI(TAG, "  SD卡: %s", g_storage ? "已就绪" : "未就绪");
    ESP_LOGI(TAG, "  H.264编码器: %s", g_h264_enc ? "已就绪" : "故障");
    ESP_LOGI(TAG, "========================================");

    /* 阻塞等待停止信号 */
    app_video_wait_video_stop();

    /* 清理 */
    stop_recording();
    app_video_close(fd);
    deinit_h264_encoder();
    if (g_recorder) video_recorder_deinit(g_recorder);
    if (g_storage) storage_manager_deinit(g_storage);
}
