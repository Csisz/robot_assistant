#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    int  sample_count;
    char id[32];
    char display_name[64];
} enroll_status_t;

/* Call once at startup — creates /sdcard/FACES/ if missing. */
esp_err_t face_enroll_init(void);

/* True while an enrollment session is active.
   face_detect_task must skip normal inference during this time. */
bool face_enroll_is_active(void);

/* Parse {"id":...,"display_name":...,"audio_file":...} and start session.
   id: max 8 chars, A-Za-z0-9_ only.
   audio_file: must be one of the known SD-card MP3 paths, e.g. /sdcard/APA.MP3, or empty. */
esp_err_t face_enroll_start_json(const char *json_body);

/* Capture one face sample from the camera.
   Requires exactly one face visible, no audio playing, session active.
   Sets *sample_count_out on success.
   Returns:
     ESP_OK               — sample saved to /sdcard/FACES/<ID>/S0N.JPG
     ESP_ERR_NOT_SUPPORTED — already at MAX_SAMPLES (9)
     ESP_ERR_NOT_FOUND    — no face detected
     ESP_ERR_INVALID_STATE — multiple faces, audio playing, or not enrolling
     ESP_ERR_NO_MEM       — PSRAM allocation failed
     ESP_FAIL             — camera or SD error */
esp_err_t face_enroll_capture(int *sample_count_out);

/* Save profile to /sdcard/FACES/DB.TXT (CSV).
   Requires at least 3 samples.
   id_out receives the uppercase person id (e.g. "APA") for UI display. */
esp_err_t face_enroll_finish(int *samples_out, char *id_out, size_t id_out_len);

/* Discard current session without saving. */
void face_enroll_cancel(void);

enroll_status_t face_enroll_get_status(void);

#ifdef __cplusplus
}
#endif
