/*
 * Activity tracker — records snake movement events.
 *
 * One event = one continuous active period (start → end).
 * Each event records position samples at ~1 Hz.
 * Events are flushed to /sdcard/logs/activity_YYYYMMDD.json as JSON append.
 */
#ifndef ACTIVITY_TRACKER_H
#define ACTIVITY_TRACKER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACTIVITY_MAX_SAMPLES 3600  /* max 1 hour at 1 Hz */

typedef struct {
    float x, y;         /* normalized head position (0.0~1.0) */
} activity_position_t;

typedef struct {
    uint32_t start_time;        /* Unix timestamp */
    uint32_t end_time;          /* Unix timestamp, 0 = ongoing */
    uint32_t duration_seconds;

    activity_position_t position_samples[ACTIVITY_MAX_SAMPLES];
    uint32_t position_count;

    uint32_t zone_track[ACTIVITY_MAX_SAMPLES];
    uint32_t zone_track_len;

    bool is_active;
} activity_event_t;

typedef struct {
    uint32_t day_start_5am;     /* 5:00 AM Unix timestamp */
    uint32_t total_active_sec;
    uint32_t total_rest_sec;
    uint32_t event_count;
    uint32_t rest_period_count;
} daily_stats_t;

/* Initialize: create /sdcard/logs/ directory, reset daily counters */
esp_err_t activity_tracker_init(void);

/* Start a new activity event */
esp_err_t activity_tracker_start_event(uint32_t unix_timestamp);

/* Record one position sample (call at ~1 Hz during RECORDING) */
esp_err_t activity_tracker_update_position(uint32_t unix_timestamp, float head_x, float head_y);

/* End current event, compute duration, append to daily JSON file */
esp_err_t activity_tracker_end_event(uint32_t unix_timestamp);

/* Get pointer to current event (for direct access) */
activity_event_t *activity_tracker_get_current_event(void);

/* Get accumulated daily stats */
esp_err_t activity_tracker_get_daily_stats(uint32_t day_5am, daily_stats_t *out);

/* Force flush daily JSON to SD card (also called automatically at end_event) */
esp_err_t activity_tracker_flush_daily_json(uint32_t day_5am);

/* Deinit, flush any pending data */
void activity_tracker_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
