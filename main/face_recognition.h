#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  recognized;
    char  person_id[16];
    char  display_name[64];
    char  audio_file[64];
    float confidence;
} face_recog_result_t;

/* Returns true only when a concrete face embedding model is available.
   Currently always false: espressif__human_face_recognition not installed. */
bool face_recog_available(void);

/* Load enrolled persons and build feature database.
   Returns ESP_ERR_NOT_SUPPORTED when the embedding model is absent. */
esp_err_t face_recog_init(void);

/* Run recognition on a detected face from an RGB565BE frame.
   rgb565     — PSRAM copy used by face_detect_run_full().
   bbox       — {x1,y1,x2,y2} from face_detect_run_full(), clamped to frame bounds.
   keypoints  — 10 ints [x,y]*5 (5 MSRMNP landmarks) from detector, or NULL.
   Returns true and fills *result if a known person matches above threshold.
   Always returns false when face_recog_available() is false. */
bool face_recog_run(const uint16_t *rgb565, int width, int height,
                    const int bbox[4], const int *keypoints,
                    face_recog_result_t *result);

#ifdef __cplusplus
}
#endif
