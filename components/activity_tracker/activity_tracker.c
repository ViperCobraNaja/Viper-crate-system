#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "activity_tracker.h"

static const char *TAG = "activity_tracker";

#define LOG_BASE_DIR  "/sdcard/logs"
#define LOG_FILE_PREFIX "/sdcard/logs/activity_"

/* PSRAM heap to avoid BSS bloat (~43KB) on internal DRAM */
static activity_event_t *g_current_event = NULL;
static daily_stats_t    g_daily_stats;
static uint32_t         g_current_day_5am;
static SemaphoreHandle_t g_lock = NULL;
static bool             g_initialized = false;

/* ---- helpers ---- */

static void make_log_dir(void)
{
    struct stat st;
    if (stat(LOG_BASE_DIR, &st) != 0) {
        mkdir(LOG_BASE_DIR, 0755);
    }
}

static uint32_t compute_day_5am(uint32_t unix_time)
{
    return unix_time - ((unix_time + 19UL * 3600UL) % 86400UL);
}

static void format_date_str(uint32_t day_5am, char *out, size_t out_len)
{
    time_t t = (time_t)day_5am + 5 * 3600;
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    strftime(out, out_len, "%Y%m%d", &tm_buf);
}

static void get_log_path(uint32_t day_5am, char *out, size_t out_len)
{
    char date_str[16];
    format_date_str(day_5am, date_str, sizeof(date_str));
    snprintf(out, out_len, "%s%s.jsonl", LOG_FILE_PREFIX, date_str);
}

static void reset_daily_stats(uint32_t day_5am)
{
    memset(&g_daily_stats, 0, sizeof(g_daily_stats));
    g_daily_stats.day_start_5am = day_5am;
    g_current_day_5am = day_5am;
}

static void accumulate_stats(const activity_event_t *evt)
{
    g_daily_stats.total_active_sec += evt->duration_seconds;
    g_daily_stats.event_count++;
}

/* ---- public API ---- */

esp_err_t activity_tracker_init(void)
{
    if (g_initialized) return ESP_OK;

    make_log_dir();

    g_current_event = heap_caps_calloc(1, sizeof(activity_event_t), MALLOC_CAP_SPIRAM);
    if (!g_current_event) {
        ESP_LOGE(TAG, "Failed to allocate event buffer on PSRAM");
        return ESP_ERR_NO_MEM;
    }

    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        heap_caps_free(g_current_event);
        g_current_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    time_t now = time(NULL);
    if (now < 1000000000) {
        ESP_LOGW(TAG, "NTP not synced, time may be invalid (%ld)", (long)now);
    }
    uint32_t day_5am = compute_day_5am((uint32_t)now);
    reset_daily_stats(day_5am);

    g_initialized = true;
    ESP_LOGI(TAG, "Initialized, day_start=%" PRIu32, day_5am);
    return ESP_OK;
}

esp_err_t activity_tracker_start_event(uint32_t unix_timestamp)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    if (g_current_event->is_active) {
        ESP_LOGW(TAG, "Ending previous active event before starting new one");
        activity_tracker_end_event(unix_timestamp);
    }

    uint32_t day_5am = compute_day_5am(unix_timestamp);
    if (day_5am != g_current_day_5am) {
        activity_tracker_flush_daily_json(g_current_day_5am);
        reset_daily_stats(day_5am);
    }

    memset(g_current_event, 0, sizeof(activity_event_t));
    g_current_event->start_time = unix_timestamp;
    g_current_event->is_active   = true;

    xSemaphoreGive(g_lock);

    ESP_LOGI(TAG, "Event started at %" PRIu32, unix_timestamp);
    return ESP_OK;
}

esp_err_t activity_tracker_update_position(uint32_t unix_timestamp, float head_x, float head_y)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    if (!g_current_event->is_active) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t idx = g_current_event->position_count;
    if (idx >= ACTIVITY_MAX_SAMPLES) {
        xSemaphoreGive(g_lock);
        ESP_LOGW(TAG, "Position buffer full (max %d samples)", ACTIVITY_MAX_SAMPLES);
        return ESP_ERR_NO_MEM;
    }

    g_current_event->position_samples[idx].x = head_x;
    g_current_event->position_samples[idx].y = head_y;
    g_current_event->position_count++;

    if (g_current_event->zone_track_len < ACTIVITY_MAX_SAMPLES) {
        g_current_event->zone_track[g_current_event->zone_track_len++] = 0;
    }

    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t activity_tracker_end_event(uint32_t unix_timestamp)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    if (!g_current_event->is_active) {
        xSemaphoreGive(g_lock);
        ESP_LOGW(TAG, "No active event to end");
        return ESP_ERR_INVALID_STATE;
    }

    g_current_event->end_time = unix_timestamp;
    g_current_event->is_active = false;

    if (unix_timestamp > g_current_event->start_time) {
        g_current_event->duration_seconds = unix_timestamp - g_current_event->start_time;
    }

    accumulate_stats(g_current_event);

    char path[128];
    get_log_path(g_current_day_5am, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "{\"start\":%" PRIu32 ",\"end\":%" PRIu32 ",\"duration\":%" PRIu32,
                g_current_event->start_time,
                g_current_event->end_time,
                g_current_event->duration_seconds);

        fprintf(f, ",\"positions\":[");
        for (uint32_t i = 0; i < g_current_event->position_count; i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "{\"x\":%.2f,\"y\":%.2f}",
                    g_current_event->position_samples[i].x,
                    g_current_event->position_samples[i].y);
        }
        fprintf(f, "]");

        fprintf(f, ",\"zones\":[");
        for (uint32_t i = 0; i < g_current_event->zone_track_len; i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "%" PRIu32, g_current_event->zone_track[i]);
        }
        fprintf(f, "]");

        fprintf(f, "}\n");
        fclose(f);

        ESP_LOGI(TAG, "Event saved: duration=%" PRIu32 "s, samples=%" PRIu32,
                 g_current_event->duration_seconds, g_current_event->position_count);
    } else {
        ESP_LOGE(TAG, "Failed to open %s for append", path);
    }

    xSemaphoreGive(g_lock);
    return ESP_OK;
}

activity_event_t *activity_tracker_get_current_event(void)
{
    if (!g_initialized || !g_current_event || !g_current_event->is_active) return NULL;
    return g_current_event;
}

esp_err_t activity_tracker_get_daily_stats(uint32_t day_5am, daily_stats_t *out)
{
    if (!g_initialized || !out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(g_lock, portMAX_DELAY);
    memcpy(out, &g_daily_stats, sizeof(daily_stats_t));

    uint32_t day_end = day_5am + 86400;
    uint32_t now = (uint32_t)time(NULL);
    if (now < day_end) day_end = now;
    out->total_rest_sec = day_end - day_5am - out->total_active_sec;

    xSemaphoreGive(g_lock);
    return ESP_OK;
}

esp_err_t activity_tracker_flush_daily_json(uint32_t day_5am)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_lock, portMAX_DELAY);

    daily_stats_t stats;
    memcpy(&stats, &g_daily_stats, sizeof(daily_stats_t));

    char path[128];
    get_log_path(day_5am, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "{\"type\":\"daily_summary\",\"day_start\":%" PRIu32
                ",\"total_active_sec\":%" PRIu32
                ",\"event_count\":%" PRIu32
                ",\"total_rest_sec\":%" PRIu32
                ",\"rest_period_count\":%" PRIu32 "}\n",
                stats.day_start_5am,
                stats.total_active_sec,
                stats.event_count,
                stats.total_rest_sec,
                stats.rest_period_count);
        fclose(f);
        ESP_LOGI(TAG, "Daily summary flushed to %s", path);
    } else {
        ESP_LOGE(TAG, "Failed to open %s for summary", path);
    }

    xSemaphoreGive(g_lock);
    return ESP_OK;
}

void activity_tracker_deinit(void)
{
    if (!g_initialized) return;

    if (g_current_event) {
        if (g_current_event->is_active) {
            activity_tracker_end_event((uint32_t)time(NULL));
        }
        activity_tracker_flush_daily_json(g_current_day_5am);
        heap_caps_free(g_current_event);
        g_current_event = NULL;
    }

    if (g_lock) {
        vSemaphoreDelete(g_lock);
        g_lock = NULL;
    }
    g_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}
