#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "driver/ppa.h"
#include "snake_detect.h"

static const char *TAG = "snake_detect";

/* AI model input resolution */
#define AI_W 320
#define AI_H 320

static ppa_client_handle_t g_ppa_ai = NULL;
static SemaphoreHandle_t   g_ai_mutex = NULL;
static uint8_t            *g_ai_rgb_buf = NULL;  /* 320x320 RGB565 for future ESP-DL input */
static size_t              g_cache_line_size = 0;

esp_err_t snake_detect_init(void)
{
    esp_err_t ret;

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &g_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get cache alignment");
        return ret;
    }

    /* PPA client for AI scaling: 800x800 YUV → 320x320 RGB565 */
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ret = ppa_register_client(&ppa_cfg, &g_ppa_ai);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA AI client: 0x%x", ret);
        return ret;
    }

    /* 320x320 RGB565 buffer for AI model input */
    g_ai_rgb_buf = heap_caps_aligned_calloc(g_cache_line_size, 1,
                                             AI_W * AI_H * 2, MALLOC_CAP_SPIRAM);
    if (!g_ai_rgb_buf) {
        ESP_LOGE(TAG, "Failed to allocate AI RGB buffer");
        ppa_unregister_client(g_ppa_ai);
        g_ppa_ai = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_ai_mutex = xSemaphoreCreateMutex();
    if (!g_ai_mutex) {
        ESP_LOGE(TAG, "Failed to create AI mutex");
        heap_caps_free(g_ai_rgb_buf);
        ppa_unregister_client(g_ppa_ai);
        g_ppa_ai = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized (skeleton), PPA %dx%d→%dx%d", 800, 800, AI_W, AI_H);
    return ESP_OK;
}

esp_err_t snake_detect_process(const uint8_t *yuv420_800x800, snake_detect_result_t *out)
{
    if (!g_ppa_ai || !g_ai_rgb_buf || !yuv420_800x800 || !out) {
        return ESP_ERR_INVALID_STATE;
    }

    /* PPA scale: 800x800 YUV420 → 320x320 RGB565 */
    ppa_srm_oper_config_t cfg = {
        .in.buffer         = (void *)yuv420_800x800,
        .in.pic_w          = 800,
        .in.pic_h          = 800,
        .in.block_w        = 800,
        .in.block_h        = 800,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm         = PPA_SRM_COLOR_MODE_YUV420,
        .out.buffer        = g_ai_rgb_buf,
        .out.buffer_size   = AI_W * AI_H * 2,
        .out.pic_w         = AI_W,
        .out.pic_h         = AI_H,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = (float)AI_W / 800.0f,
        .scale_y           = (float)AI_H / 800.0f,
        .rgb_swap          = 0,
        .byte_swap         = 0,
        .mode              = PPA_TRANS_MODE_BLOCKING,
    };

    xSemaphoreTake(g_ai_mutex, portMAX_DELAY);
    esp_err_t ret = ppa_do_scale_rotate_mirror(g_ppa_ai, &cfg);
    if (ret == ESP_OK) {
        esp_cache_msync(g_ai_rgb_buf, AI_W * AI_H * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    } else {
        ESP_LOGW(TAG, "PPA AI scale failed: 0x%x", ret);
        xSemaphoreGive(g_ai_mutex);
        return ret;
    }
    xSemaphoreGive(g_ai_mutex);

    /* TODO: ESP-DL inference on g_ai_rgb_buf (320x320 RGB565)
     * For now, return mock detection result for state machine testing */
    static int mock_call_count = 0;
    mock_call_count++;

    memset(out, 0, sizeof(*out));
    out->has_snake  = true;
    out->confidence = 0.80f;
    out->x = 0.25f;
    out->y = 0.25f;
    out->w = 0.50f;
    out->h = 0.30f;

    return ESP_OK;
}

void snake_detect_deinit(void)
{
    if (g_ppa_ai) {
        ppa_unregister_client(g_ppa_ai);
        g_ppa_ai = NULL;
    }
    if (g_ai_rgb_buf) {
        heap_caps_free(g_ai_rgb_buf);
        g_ai_rgb_buf = NULL;
    }
    if (g_ai_mutex) {
        vSemaphoreDelete(g_ai_mutex);
        g_ai_mutex = NULL;
    }
    ESP_LOGI(TAG, "Deinitialized");
}
