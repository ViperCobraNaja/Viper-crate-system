/*
 * Snake detection interface (YOLO11n, 320x320 INT8)
 *
 * Input:  O_UYY_E_VYY 800x800 (frame pool buffer)
 * Output: bbox + confidence + has_snake flag
 *
 * Pipeline: PPA scale 800x800 YUV → 320x320 RGB → ESP-DL inference → bbox decode
 * Currently skeleton: PPA scale works, inference returns mock result.
 */
#ifndef SNAKE_DETECT_H
#define SNAKE_DETECT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, w, h;       // bbox normalized coordinates (0.0~1.0), top-left origin
    float confidence;        // confidence [0, 1]
    bool  has_snake;         // snake detected (confidence > threshold)
} snake_detect_result_t;

// Allocate PPA client + inference buffers. Does NOT load the .espdl model yet.
esp_err_t snake_detect_init(void);

// Run detection on one 800x800 O_UYY_E_VYY frame.
// Internally: PPA scale 800→320 RGB (for future ESP-DL) → mock inference
esp_err_t snake_detect_process(const uint8_t *yuv420_800x800, snake_detect_result_t *out);

// Release PPA client and buffers
void snake_detect_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
