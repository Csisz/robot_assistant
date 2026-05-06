#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * face_detect_init()      — loads MSRMNP model eagerly from flash rodata into PSRAM.
 *                           Must be called before any other function.
 *                           Returns ESP_ERR_NOT_SUPPORTED if the managed component
 *                           is absent, ESP_ERR_NO_MEM if PSRAM is insufficient.
 *
 * face_detect_is_ready()  — returns true iff init succeeded and detector is live.
 *                           Check this before calling inference; if false, inference
 *                           will return 0/false immediately (with an error log).
 *
 * face_detect_run()       — bool: at least one face found in the frame.
 * face_detect_run_ex()    — int: face count; fills bbox[4]={x1,y1,x2,y2}.
 * face_detect_run_full()  — int: face count; fills bbox[4] and keypoints[10].
 *
 * face_detect_run_ex2()   — int: face count; explicit pixel type (no global state
 *                           change).  pix_type_int values:
 *                             9  = RGB565LE   10 = RGB565BE
 *                             11 = BGR565LE   12 = BGR565BE
 *                           Used by /debug/detect to probe all four variants.
 *
 * face_detect_set_byte_swap() — selects RGB565LE (swap=true) vs RGB565BE (false)
 *                               for run/run_ex/run_full.  Persists until changed.
 *                               run_ex2() ignores this flag.
 */

esp_err_t face_detect_init(void);
bool      face_detect_is_ready(void);

bool face_detect_run     (const uint16_t *rgb565, int width, int height);
int  face_detect_run_ex  (const uint16_t *rgb565, int width, int height, int bbox[4]);
int  face_detect_run_full(const uint16_t *rgb565, int width, int height,
                           int bbox[4], int keypoints[10]);

int  face_detect_run_ex2 (const uint16_t *data, int width, int height,
                           int pix_type_int, int bbox[4]);

void face_detect_set_byte_swap(bool byte_swap);
bool face_detect_get_byte_swap(void);

#ifdef __cplusplus
}
#endif
