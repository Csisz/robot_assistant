#include "robot_voice.h"
#include "robot_chat.h"
#include "robot_state.h"
#include "web_server.h"
#include "bsp_board.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CONFIG_ROBOT_ENABLE_VOICE_INPUT
#define CONFIG_ROBOT_ENABLE_VOICE_INPUT 0
#endif

static const char *TAG = "voice";

/* -------------------------------------------------------------------------
 * Recording parameters
 * -------------------------------------------------------------------------
 * Codec runs at 48 kHz, stereo, 32-bit (from bsp_board init).
 * esp_get_feed_data returns raw frames: each frame = 4 × int16_t
 *   [upper16_ch0, lower16_ch0, upper16_ch1, lower16_ch1]
 * upper16_ch0 is MIC1 audio (significant bits of the 32-bit ADC sample).
 * We extract one int16_t per frame and write a mono 48 kHz 16-bit WAV.
 * 4 seconds × 48000 frames/s × 2 bytes = 384 000 bytes audio data.
 * ----------------------------------------------------------------------- */
#define VOICE_SAMPLE_RATE       48000
#define VOICE_DURATION_MS       4000
#define VOICE_CHUNK_FRAMES      480             /* 10 ms at 48 kHz */
#define VOICE_RAW_FRAME_I16     4               /* int16_t values per raw frame */
#define VOICE_RAW_CHUNK_BYTES   (VOICE_CHUNK_FRAMES * VOICE_RAW_FRAME_I16 * sizeof(int16_t))
#define VOICE_PCM_CHUNK_BYTES   (VOICE_CHUNK_FRAMES * sizeof(int16_t))
#define VOICE_TOTAL_FRAMES      ((VOICE_SAMPLE_RATE * VOICE_DURATION_MS) / 1000)
#define VOICE_WAV_DATA_BYTES    (VOICE_TOTAL_FRAMES * sizeof(int16_t))
#define VOICE_CHUNK_ITERS       (VOICE_TOTAL_FRAMES / VOICE_CHUNK_FRAMES)
#define PTT_TASK_STACK          (20 * 1024)
#define VOICE_SCAN_ITERS        20              /* 200 ms pre-scan to probe TDM slots */
#define VOICE_MIN_RMS           100.0f          /* below this RMS, slot treated as inactive */

/* -------------------------------------------------------------------------
 * Module state
 * ----------------------------------------------------------------------- */
typedef enum {
    VS_IDLE = 0,
    VS_RECORDING,
    VS_UPLOADING,
    VS_PLAYING_REPLY,
    VS_ERROR,
} voice_state_t;

static volatile voice_state_t s_state    = VS_IDLE;
static char s_last_error[128]            = "";
static robot_voice_timing_t s_timing     = {0};
static char s_transcript[256]            = "";

/* -------------------------------------------------------------------------
 * WAV header (little-endian, PCM, mono, 48 kHz, 16-bit)
 * ----------------------------------------------------------------------- */
static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    const uint16_t channels    = 1;
    const uint32_t sample_rate = VOICE_SAMPLE_RATE;
    const uint16_t bits        = 16;
    const uint32_t byte_rate   = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t chunk_size  = 36 + data_bytes;
    const uint32_t fmt_size    = 16;
    const uint16_t audio_fmt   = 1; /* PCM */

    fwrite("RIFF",        1, 4, f);
    fwrite(&chunk_size,   4, 1, f);
    fwrite("WAVE",        1, 4, f);
    fwrite("fmt ",        1, 4, f);
    fwrite(&fmt_size,     4, 1, f);
    fwrite(&audio_fmt,    2, 1, f);
    fwrite(&channels,     2, 1, f);
    fwrite(&sample_rate,  4, 1, f);
    fwrite(&byte_rate,    4, 1, f);
    fwrite(&block_align,  2, 1, f);
    fwrite(&bits,         2, 1, f);
    fwrite("data",        1, 4, f);
    fwrite(&data_bytes,   4, 1, f);
}

/* -------------------------------------------------------------------------
 * Public API — state helpers
 * ----------------------------------------------------------------------- */
void robot_voice_set_timing(const robot_voice_timing_t *timing)
{
    if (timing) s_timing = *timing;
}

void robot_voice_set_last_transcript(const char *transcript)
{
    strlcpy(s_transcript, transcript ? transcript : "", sizeof(s_transcript));
}

void robot_voice_mark_uploading(void)        { s_state = VS_UPLOADING; }
void robot_voice_mark_waiting_backend(void)  { s_state = VS_UPLOADING; }
void robot_voice_mark_playing_reply(void)    { s_state = VS_PLAYING_REPLY; }
void robot_voice_mark_idle(void)             { s_state = VS_IDLE; }

void robot_voice_mark_error_idle(const char *error)
{
    strlcpy(s_last_error, error ? error : "voice error", sizeof(s_last_error));
    s_state = VS_ERROR;
}

void robot_voice_force_reset(void)
{
    s_state = VS_IDLE;
    s_last_error[0] = '\0';
    memset(&s_timing, 0, sizeof(s_timing));
    s_transcript[0] = '\0';
}

const char *robot_voice_state_name(void)
{
    switch (s_state) {
    case VS_IDLE:           return "idle";
    case VS_RECORDING:      return "recording";
    case VS_UPLOADING:      return "uploading";
    case VS_PLAYING_REPLY:  return "playing_reply";
    case VS_ERROR:          return "error";
    default:                return "unknown";
    }
}

void robot_voice_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    snprintf(out, out_len,
             "{\"voice_state\":\"%s\",\"voice_available\":%s,"
             "\"last_voice_error\":\"%s\",\"transcript\":\"%s\","
             "\"record_ms\":%u,\"upload_ms\":%u,\"backend_ms\":%u,"
             "\"audio_download_ms\":%u,\"total_ms\":%u,\"wav_bytes\":%u}",
#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
             robot_voice_state_name(), "true",
#else
             "disabled", "false",
#endif
             s_last_error,
             s_transcript,
             (unsigned)s_timing.record_ms,
             (unsigned)s_timing.upload_ms,
             (unsigned)s_timing.backend_ms,
             (unsigned)s_timing.audio_download_ms,
             (unsigned)s_timing.total_ms,
             (unsigned)s_timing.wav_bytes);
}

/* -------------------------------------------------------------------------
 * Init
 * ----------------------------------------------------------------------- */
esp_err_t robot_voice_init(void)
{
#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
    s_state = VS_IDLE;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "voice input ready (duration=%dms rate=%dHz)",
             VOICE_DURATION_MS, VOICE_SAMPLE_RATE);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "voice input disabled by build config");
    strlcpy(s_last_error, "voice input disabled", sizeof(s_last_error));
    return ESP_OK;
#endif
}

/* -------------------------------------------------------------------------
 * Record WAV
 * ----------------------------------------------------------------------- */
esp_err_t robot_voice_record_wav_file(size_t *bytes_out, char *error, size_t error_len)
{
    if (bytes_out) *bytes_out = 0;
#if !CONFIG_ROBOT_ENABLE_VOICE_INPUT
    if (error && error_len) strlcpy(error, "voice input disabled", error_len);
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* Small buffers — use internal SRAM (I2S read doesn't need DMA dest) */
    int16_t *raw = heap_caps_malloc(VOICE_RAW_CHUNK_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(VOICE_PCM_CHUNK_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw || !pcm) {
        free(raw);
        free(pcm);
        if (error && error_len) strlcpy(error, "record buffer alloc failed", error_len);
        return ESP_ERR_NO_MEM;
    }

    /* --- Pre-scan: 200 ms to detect which TDM slot carries mic audio ------- */
    int64_t scan_sum_sq[4] = {0, 0, 0, 0};
    int32_t scan_peak[4]   = {0, 0, 0, 0};
    for (int iter = 0; iter < VOICE_SCAN_ITERS; iter++) {
        esp_err_t rd = esp_get_feed_data(true, raw, (int)VOICE_RAW_CHUNK_BYTES);
        if (rd != ESP_OK) memset(raw, 0, VOICE_RAW_CHUNK_BYTES);
        for (int i = 0; i < VOICE_CHUNK_FRAMES; i++) {
            for (int s = 0; s < 4; s++) {
                int16_t samp  = raw[VOICE_RAW_FRAME_I16 * i + s];
                int32_t abs_s = samp < 0 ? -(int32_t)samp : (int32_t)samp;
                scan_sum_sq[s] += (int64_t)samp * samp;
                if (abs_s > scan_peak[s]) scan_peak[s] = abs_s;
            }
        }
    }

    /* Select slot with highest RMS; fall back to 0 if all slots are dead */
    int selected_slot = 0;
    float best_rms = 0.0f;
    const uint32_t scan_frames = VOICE_SCAN_ITERS * VOICE_CHUNK_FRAMES;
    for (int s = 0; s < 4; s++) {
        float rms = sqrtf((float)scan_sum_sq[s] / scan_frames);
        if (rms > best_rms) { best_rms = rms; selected_slot = s; }
    }
    const char *mode_str;
    if (best_rms >= VOICE_MIN_RMS) {
        mode_str = "auto";
    } else {
        selected_slot = 0;
        mode_str = "default_fallback";
    }
    ESP_LOGI(TAG, "voice selected slot=%d mode=%s scan_rms=%.1f",
             selected_slot, mode_str, best_rms);

    /* Open WAV file after slot selection */
    unlink(ROBOT_VOICE_WAV_PATH);
    FILE *f = fopen(ROBOT_VOICE_WAV_PATH, "wb");
    if (!f) {
        free(raw);
        free(pcm);
        if (error && error_len)
            snprintf(error, error_len, "cannot create %s (errno=%d)",
                     ROBOT_VOICE_WAV_PATH, errno);
        return ESP_FAIL;
    }
    write_wav_header(f, VOICE_WAV_DATA_BYTES);

    /* --- Recording phase: extract selected slot, accumulate per-slot stats - */
    esp_err_t err = ESP_OK;
    size_t frames_written = 0;

    int64_t slot_sum_sq[4] = {0, 0, 0, 0};
    int32_t slot_peak[4]   = {0, 0, 0, 0};
    int32_t slot_silent[4] = {0, 0, 0, 0};

    for (int iter = 0; iter < VOICE_CHUNK_ITERS; iter++) {
        esp_err_t rd = esp_get_feed_data(true, raw, (int)VOICE_RAW_CHUNK_BYTES);
        if (rd != ESP_OK) {
            memset(raw, 0, VOICE_RAW_CHUNK_BYTES);
        }
        for (int i = 0; i < VOICE_CHUNK_FRAMES; i++) {
            pcm[i] = raw[VOICE_RAW_FRAME_I16 * i + selected_slot];
            for (int s = 0; s < 4; s++) {
                int16_t samp  = raw[VOICE_RAW_FRAME_I16 * i + s];
                int32_t abs_s = samp < 0 ? -(int32_t)samp : (int32_t)samp;
                slot_sum_sq[s] += (int64_t)samp * samp;
                if (abs_s > slot_peak[s]) slot_peak[s] = abs_s;
                if (abs_s < 100) slot_silent[s]++;
            }
        }
        size_t wrote = fwrite(pcm, sizeof(int16_t), VOICE_CHUNK_FRAMES, f);
        if (wrote != VOICE_CHUNK_FRAMES) {
            err = ESP_FAIL;
            break;
        }
        frames_written += wrote;
    }

    /* Log per-slot audio diagnostics (recording phase) */
    {
        uint32_t n = frames_written > 0 ? (uint32_t)frames_written : 1;
        int best_slot = 0;
        for (int s = 0; s < 4; s++) {
            float rms    = sqrtf((float)slot_sum_sq[s] / n);
            float dbfs   = rms > 0.5f ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
            int   sil_pm = (int)((int64_t)slot_silent[s] * 1000 / n);
            ESP_LOGI(TAG, "mic_debug slot=%d rms=%.1f peak=%d dbfs=%.1f silence_pm=%d",
                     s, rms, (int)slot_peak[s], dbfs, sil_pm);
            if (slot_peak[s] > slot_peak[best_slot]) best_slot = s;
        }
        ESP_LOGI(TAG, "mic_debug best_candidate slot=%d", best_slot);
        float rmss  = sqrtf((float)slot_sum_sq[selected_slot] / n);
        float dbfss = rmss > 0.5f ? 20.0f * log10f(rmss / 32768.0f) : -96.0f;
        int   sils  = (int)((int64_t)slot_silent[selected_slot] * 1000 / n);
        ESP_LOGI(TAG, "voice stats: size=%u sample_rate=%u bits=16 channels=1 "
                 "peak=%d rms=%.1f dbfs=%.1f silence_per_mille=%d slot=%d mode=%s",
                 (unsigned)(44 + frames_written * sizeof(int16_t)),
                 (unsigned)VOICE_SAMPLE_RATE,
                 (int)slot_peak[selected_slot], rmss, dbfss, sils,
                 selected_slot, mode_str);
    }

    if (fclose(f) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(raw);
    free(pcm);

    if (err == ESP_OK && bytes_out) {
        *bytes_out = 44 + frames_written * sizeof(int16_t);
    }
    if (err != ESP_OK && error && error_len) {
        strlcpy(error, "wav write failed", error_len);
    }
    ESP_LOGI(TAG, "record done: frames=%u bytes=%u err=%s",
             (unsigned)frames_written,
             (unsigned)(44 + frames_written * sizeof(int16_t)),
             esp_err_to_name(err));
    return err;
#endif
}

/* -------------------------------------------------------------------------
 * Push-to-talk task
 * ----------------------------------------------------------------------- */
#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
static void ptt_task(void *arg)
{
    (void)arg;
    robot_voice_timing_t timing = {0};
    int64_t t_start = esp_timer_get_time() / 1000;

    /* Visual indicator during recording */
    robot_set_thinking();

    /* --- Record phase -------------------------------------------------- */
    s_state = VS_RECORDING;
    char err_buf[128] = {0};
    size_t wav_bytes = 0;

    int64_t rec_start = esp_timer_get_time() / 1000;
    esp_err_t err = robot_voice_record_wav_file(&wav_bytes, err_buf, sizeof(err_buf));
    timing.record_ms = (uint32_t)(esp_timer_get_time() / 1000 - rec_start);
    timing.wav_bytes  = wav_bytes;

    if (err != ESP_OK) {
        strlcpy(s_last_error, err_buf[0] ? err_buf : "record failed", sizeof(s_last_error));
        s_state = VS_ERROR;
        robot_set_error("Mikrofon hiba");
        vTaskDelay(pdMS_TO_TICKS(600));
        robot_set_idle();
        s_state = VS_IDLE;
        vTaskDelete(NULL);
        return;
    }

    /* --- Upload + reply phase ------------------------------------------ */
    s_state = VS_UPLOADING;

    int64_t upload_start = esp_timer_get_time() / 1000;
    robot_chat_response_t resp = {0};
    err = robot_chat_send_voice_file(ROBOT_VOICE_WAV_PATH, &resp);
    timing.upload_ms = (uint32_t)(esp_timer_get_time() / 1000 - upload_start);
    timing.total_ms  = (uint32_t)(esp_timer_get_time() / 1000 - t_start);

    robot_voice_set_timing(&timing);

    if (resp.transcript[0]) {
        strlcpy(s_transcript, resp.transcript, sizeof(s_transcript));
        ESP_LOGI(TAG, "voice transcript received: %s", s_transcript);
        web_server_set_last_transcript(s_transcript);
    } else {
        ESP_LOGW(TAG, "voice transcript missing or empty");
        web_server_set_last_transcript("");
    }

    if (err != ESP_OK) {
        strlcpy(s_last_error,
                resp.error[0] ? resp.error : "voice upload failed",
                sizeof(s_last_error));
        s_state = VS_ERROR;
    } else {
        s_last_error[0] = '\0';
        /* Audio playback may still be running; voice flow is complete */
        s_state = VS_IDLE;
    }

    vTaskDelete(NULL);
}
#endif /* CONFIG_ROBOT_ENABLE_VOICE_INPUT */

/* -------------------------------------------------------------------------
 * Public API — push-to-talk
 * ----------------------------------------------------------------------- */
esp_err_t robot_voice_start_push_to_talk(void)
{
    return robot_voice_start_push_to_talk_ex(NULL, 0);
}

esp_err_t robot_voice_start_push_to_talk_ex(char *error, size_t error_len)
{
#if !CONFIG_ROBOT_ENABLE_VOICE_INPUT
    const char *msg = "voice input disabled";
    strlcpy(s_last_error, msg, sizeof(s_last_error));
    if (error && error_len) strlcpy(error, msg, error_len);
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (robot_get_state() != ROBOT_IDLE) {
        const char *msg = "robot busy";
        strlcpy(s_last_error, msg, sizeof(s_last_error));
        if (error && error_len) strlcpy(error, msg, error_len);
        ESP_LOGW(TAG, "PTT ignored: robot_state=%s", robot_state_name(robot_get_state()));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != VS_IDLE) {
        const char *msg = "voice busy";
        strlcpy(s_last_error, msg, sizeof(s_last_error));
        if (error && error_len) strlcpy(error, msg, error_len);
        ESP_LOGW(TAG, "PTT ignored: voice_state=%s", robot_voice_state_name());
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(ptt_task, "ptt_task", PTT_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        const char *msg = "ptt task create failed";
        strlcpy(s_last_error, msg, sizeof(s_last_error));
        if (error && error_len) strlcpy(error, msg, error_len);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PTT started");
    return ESP_OK;
#endif
}
