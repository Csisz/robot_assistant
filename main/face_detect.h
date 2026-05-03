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
bool      face_detect_run(const uint16_t *rgb565, int width, int height);

#ifdef __cplusplus
}
#endif
