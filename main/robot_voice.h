#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOT_VOICE_WAV_PATH "/sdcard/VOICE.WAV"
#define ROBOT_VOICE_DEBUG_SLOT0_PATH "/sdcard/SLOT0.WAV"
#define ROBOT_VOICE_DEBUG_SLOT1_PATH "/sdcard/SLOT1.WAV"
#define ROBOT_VOICE_DEBUG_SLOT2_PATH "/sdcard/SLOT2.WAV"
#define ROBOT_VOICE_DEBUG_SLOT3_PATH "/sdcard/SLOT3.WAV"

typedef struct {
    uint32_t record_ms;
    uint32_t upload_ms;
    uint32_t backend_ms;
    uint32_t audio_download_ms;
    uint32_t total_ms;
    uint32_t sample_rate;
    size_t wav_bytes;
} robot_voice_timing_t;

esp_err_t robot_voice_init(void);
esp_err_t robot_voice_start_push_to_talk(void);
esp_err_t robot_voice_start_push_to_talk_ex(char *error, size_t error_len);
esp_err_t robot_voice_record_wav_file(size_t *bytes_out, char *error, size_t error_len);
void robot_voice_set_timing(const robot_voice_timing_t *timing);
void robot_voice_mark_uploading(void);
void robot_voice_mark_waiting_backend(void);
void robot_voice_mark_playing_reply(void);
void robot_voice_mark_idle(void);
void robot_voice_mark_error_idle(const char *error);
void robot_voice_force_reset(void);
void robot_voice_status_json(char *out, size_t out_len);
const char *robot_voice_state_name(void);

#ifdef __cplusplus
}
#endif
