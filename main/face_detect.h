#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * face_detect_init()  – allocates the HumanFaceDetect model (uses PSRAM).
 *                       Safe to call multiple times; no-op after first call.
 *
 * face_detect_run()   – runs inference on one RGB565 QQVGA frame.
 *                       Returns true if at least one face was found.
 *                       Thread-safe: caller must not call concurrently.
 *
 * Component required: espressif/esp-dl  (added to idf_component.yml)
 * Target model:       HumanFaceDetect MSR01 (two-stage, RGB565 input)
 */

esp_err_t face_detect_init(void);

/* Returns true if at least one face found. */
bool face_detect_run(const uint16_t *rgb565, int width, int height);

/* Extended: returns face count; fills bbox[4] = {x1,y1,x2,y2} for the first
   face (clamped to frame bounds). bbox may be NULL.
   Serialised with other detect functions via an internal mutex. */
int  face_detect_run_ex(const uint16_t *rgb565, int width, int height, int bbox[4]);

/* Full: returns face count; fills bbox[4] = {x1,y1,x2,y2} and
   keypoints[10] = {x0,y0,...,x4,y4} (5 MSRMNP landmarks) for the first face.
   Either pointer may be NULL. Serialised via the same internal mutex. */
int  face_detect_run_full(const uint16_t *rgb565, int width, int height,
                           int bbox[4], int keypoints[10]);

#ifdef __cplusplus
}
#endif
