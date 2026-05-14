#include "robot_voice.h"

#include "audio_driver.h"
#include "bsp_board.h"
#include "camera_driver.h"
#include "robot_chat.h"
#include "robot_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "voice";

#ifndef VOICE_RECORD_DURATION_MS
#define VOICE_RECORD_DURATION_MS 2500
#endif
#ifndef VOICE_MAX_RECORD_DURATION_MS
#define VOICE_MAX_RECORD_DURATION_MS 4000
#endif

#define VOICE_INPUT_SAMPLE_RATE 48000
#define VOICE_UPLOAD_SAMPLE_RATE 16000
#define VOICE_DOWNSAMPLE_RATIO (VOICE_INPUT_SAMPLE_RATE / VOICE_UPLOAD_SAMPLE_RATE)
#define VOICE_CHANNELS 1
#define VOICE_RAW_CHANNELS 4
#define VOICE_BITS_PER_SAMPLE 16
#define VOICE_FRAMES_PER_READ 512
#define VOICE_I2S_DMA_READ_BYTES 2048
#define VOICE_STALE_BUSY_MS 30000
#define VOICE_ERROR_MAX 128
#define VOICE_STATS_SILENCE_THRESHOLD 64
#ifndef VOICE_DEFAULT_SLOT
#define VOICE_DEFAULT_SLOT 0
#endif
#ifndef VOICE_ENABLE_DEBUG_WAVS
#define VOICE_ENABLE_DEBUG_WAVS 0
#endif
#ifndef VOICE_DEBUG_SLOT
#define VOICE_DEBUG_SLOT 0
#endif
#ifndef VOICE_DEBUG_EXTRACT_MODE
#define VOICE_DEBUG_EXTRACT_MODE VOICE_EXTRACT_HIGH16
#endif
#define VOICE_DEBUG_WAV_PATH "/sdcard/DBGVOICE.WAV"

typedef enum {
    VOICE_IDLE = 0,
    VOICE_RECORDING,
    VOICE_UPLOADING,
    VOICE_WAITING_BACKEND,
    VOICE_PLAYING_REPLY,
    VOICE_ERROR,
} voice_state_t;

typedef enum {
    VOICE_EXTRACT_LOW16 = 0,
    VOICE_EXTRACT_HIGH16,
    VOICE_EXTRACT_SIGNED_HIGH16,
} voice_extract_mode_t;

#ifndef VOICE_SAMPLE_EXTRACT_MODE
#define VOICE_SAMPLE_EXTRACT_MODE VOICE_EXTRACT_HIGH16
#endif

static const char *extract_mode_name(voice_extract_mode_t mode);

typedef struct {
    bool valid;
    bool appears_silent;
    size_t file_size;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t duration_ms;
    uint32_t peak_abs;
    uint32_t rms;
    int32_t dbfs_tenths;
    uint32_t zero_ratio_per_mille;
    uint32_t silence_ratio_per_mille;
} voice_audio_stats_t;

typedef struct {
    uint64_t sum_squares;
    uint64_t total_samples;
    uint32_t peak_abs;
} voice_level_accum_t;

typedef struct {
    FILE *file;
    const char *path;
    int slot;
    voice_extract_mode_t mode;
    uint32_t data_size;
    bool ok;
} voice_debug_wav_t;

typedef struct {
    voice_state_t state;
    int64_t state_since_ms;
    char last_error[VOICE_ERROR_MAX];
    size_t last_recording_size;
    voice_audio_stats_t last_stats;
    int last_selected_slot;
    char last_selected_mode[20];
    robot_voice_timing_t last_timing;
} voice_status_t;

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

static SemaphoreHandle_t s_voice_lock;
static bool s_camera_paused_for_voice = false;
static voice_status_t s_voice_status = {
    .state = VOICE_IDLE,
    .state_since_ms = 0,
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *voice_state_to_name(voice_state_t state)
{
    switch (state) {
        case VOICE_IDLE: return "idle";
        case VOICE_RECORDING: return "recording";
        case VOICE_UPLOADING: return "uploading";
        case VOICE_WAITING_BACKEND: return "waiting_backend";
        case VOICE_PLAYING_REPLY: return "playing";
        case VOICE_ERROR: return "error";
        default: return "unknown";
    }
}

static void voice_pause_camera_for_workflow(void)
{
    if (!s_camera_paused_for_voice) {
        camera_pause_for_voice();
        s_camera_paused_for_voice = true;
    }
}

static void voice_resume_camera_after_workflow(void)
{
    if (s_camera_paused_for_voice) {
        camera_resume_after_voice();
        s_camera_paused_for_voice = false;
    }
}

static void voice_set_state(voice_state_t state, const char *error)
{
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    s_voice_status.state = state;
    s_voice_status.state_since_ms = now_ms();
    if (error) {
        strlcpy(s_voice_status.last_error, error, sizeof(s_voice_status.last_error));
    } else if (state != VOICE_ERROR) {
        s_voice_status.last_error[0] = '\0';
    }
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
    ESP_LOGI(TAG, "state -> %s", voice_state_to_name(state));
}

static void voice_set_recording_size(size_t size)
{
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    s_voice_status.last_recording_size = size;
    if (size == 0) {
        memset(&s_voice_status.last_stats, 0, sizeof(s_voice_status.last_stats));
        memset(&s_voice_status.last_timing, 0, sizeof(s_voice_status.last_timing));
        s_voice_status.last_selected_slot = 0;
        s_voice_status.last_selected_mode[0] = '\0';
    } else {
        s_voice_status.last_stats.file_size = size;
    }
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
}

void robot_voice_set_timing(const robot_voice_timing_t *timing)
{
    if (!timing) {
        return;
    }
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    s_voice_status.last_timing = *timing;
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
}

static void voice_set_audio_stats(const voice_audio_stats_t *stats)
{
    if (!stats) {
        return;
    }
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    s_voice_status.last_recording_size = stats->file_size;
    s_voice_status.last_stats = *stats;
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
}

static void voice_set_selected_extract(int slot, voice_extract_mode_t mode)
{
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    s_voice_status.last_selected_slot = slot;
    strlcpy(s_voice_status.last_selected_mode, extract_mode_name(mode),
            sizeof(s_voice_status.last_selected_mode));
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
}

static void voice_set_error_then_idle(const char *error)
{
    voice_set_state(VOICE_ERROR, error);
    voice_set_state(VOICE_IDLE, error);
    robot_set_idle();
    voice_resume_camera_after_workflow();
}

static voice_state_t voice_get_state_snapshot(char *last_error, size_t last_error_len,
                                              int64_t *busy_for_ms, size_t *last_size,
                                              voice_audio_stats_t *stats,
                                              int *selected_slot,
                                              char *selected_mode,
                                              size_t selected_mode_len)
{
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    voice_state_t state = s_voice_status.state;
    if (last_error && last_error_len) {
        strlcpy(last_error, s_voice_status.last_error, last_error_len);
    }
    if (busy_for_ms) {
        *busy_for_ms = now_ms() - s_voice_status.state_since_ms;
    }
    if (last_size) {
        *last_size = s_voice_status.last_recording_size;
    }
    if (stats) {
        *stats = s_voice_status.last_stats;
    }
    if (selected_slot) {
        *selected_slot = s_voice_status.last_selected_slot;
    }
    if (selected_mode && selected_mode_len) {
        strlcpy(selected_mode, s_voice_status.last_selected_mode, selected_mode_len);
    }
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
    return state;
}

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t op = value;
    uint64_t res = 0;
    uint64_t one = (uint64_t)1 << 62;

    while (one > op) {
        one >>= 2;
    }
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res += one << 1;
        }
        res >>= 1;
        one >>= 2;
    }
    return (uint32_t)res;
}

static int32_t approx_dbfs_tenths(uint32_t rms)
{
    if (rms == 0) {
        return -1200;
    }
    uint32_t per_mille = (uint32_t)(((uint64_t)rms * 1000U) / 32768U);
    static const uint16_t thresholds[] = {
        1000, 891, 794, 708, 631, 562, 501, 447, 398, 355,
        316, 282, 251, 224, 200, 178, 158, 141, 126, 112,
        100, 89, 79, 71, 63, 56, 50, 45, 40, 35,
        32, 28, 25, 22, 20, 18, 16, 14, 13, 11,
        10, 9, 8, 7, 6, 6, 5, 4, 4, 4,
        3, 3, 3, 2, 2, 2, 2, 1, 1, 1,
        1
    };
    for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
        if (per_mille >= thresholds[i]) {
            return -(int32_t)(i * 10);
        }
    }
    return -700;
}

static void format_tenths(int32_t value, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *sign = value < 0 ? "-" : "";
    uint32_t abs_value = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
    snprintf(out, out_len, "%s%u.%u", sign, (unsigned)(abs_value / 10U), (unsigned)(abs_value % 10U));
}

static const char *extract_mode_name(voice_extract_mode_t mode)
{
    switch (mode) {
        case VOICE_EXTRACT_LOW16: return "low16";
        case VOICE_EXTRACT_HIGH16: return "high16";
        case VOICE_EXTRACT_SIGNED_HIGH16: return "signed_high16";
        default: return "unknown";
    }
}

static int16_t extract_i2s_sample16(int32_t raw, voice_extract_mode_t mode)
{
    switch (mode) {
        case VOICE_EXTRACT_LOW16:
            return (int16_t)(raw & 0xFFFF);
        case VOICE_EXTRACT_HIGH16:
            return (int16_t)((uint32_t)raw >> 16);
        case VOICE_EXTRACT_SIGNED_HIGH16:
        default:
            return (int16_t)(raw >> 16);
    }
}

static void level_accum_add(voice_level_accum_t *accum, int16_t sample)
{
    if (!accum) {
        return;
    }
    int32_t value = sample;
    uint32_t abs_value = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
    if (abs_value > accum->peak_abs) {
        accum->peak_abs = abs_value;
    }
    accum->sum_squares += (uint64_t)abs_value * (uint64_t)abs_value;
    accum->total_samples++;
}

static uint32_t level_accum_rms(const voice_level_accum_t *accum)
{
    if (!accum || accum->total_samples == 0) {
        return 0;
    }
    return isqrt_u64(accum->sum_squares / accum->total_samples);
}

static void log_mic_debug_stats(const voice_level_accum_t levels[3][VOICE_RAW_CHANNELS], int raw_slots)
{
    int slots = raw_slots;
    if (slots > VOICE_RAW_CHANNELS) {
        slots = VOICE_RAW_CHANNELS;
    }
    for (int slot = 0; slot < slots; slot++) {
        for (int mode = 0; mode < 3; mode++) {
            uint32_t rms = level_accum_rms(&levels[mode][slot]);
            char dbfs_text[16];
            format_tenths(approx_dbfs_tenths(rms), dbfs_text, sizeof(dbfs_text));
            ESP_LOGI(TAG, "mic_debug slot=%d %s rms=%u peak=%u dbfs=%s",
                     slot,
                     extract_mode_name((voice_extract_mode_t)mode),
                     (unsigned)rms,
                     (unsigned)levels[mode][slot].peak_abs,
                     dbfs_text);
        }
    }
}

static void choose_best_extract(const voice_level_accum_t levels[3][VOICE_RAW_CHANNELS],
                                int raw_slots,
                                int *slot_out,
                                voice_extract_mode_t *mode_out)
{
    int best_slot = VOICE_DEFAULT_SLOT;
    voice_extract_mode_t best_mode = VOICE_SAMPLE_EXTRACT_MODE;
    uint32_t best_score = 0;
    int slots = raw_slots;
    if (slots > VOICE_RAW_CHANNELS) {
        slots = VOICE_RAW_CHANNELS;
    }
    for (int slot = 0; slot < slots; slot++) {
        for (int mode = 0; mode < 3; mode++) {
            uint32_t rms = level_accum_rms(&levels[mode][slot]);
            uint32_t peak = levels[mode][slot].peak_abs;
            if (peak >= 32760) {
                rms /= 2;
            }
            if (rms > best_score) {
                best_score = rms;
                best_slot = slot;
                best_mode = (voice_extract_mode_t)mode;
            }
        }
    }
    if (slot_out) {
        *slot_out = best_slot;
    }
    if (mode_out) {
        *mode_out = best_mode;
    }
}

static esp_err_t analyze_voice_wav(const char *path, voice_audio_stats_t *stats)
{
    if (!path || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof(*stats));

    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size <= (long)sizeof(wav_header_t)) {
        return ESP_FAIL;
    }
    stats->file_size = (size_t)st.st_size;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "voice stats: open failed path=%s errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }

    wav_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return ESP_FAIL;
    }
    if (memcmp(header.riff, "RIFF", 4) != 0 ||
        memcmp(header.wave, "WAVE", 4) != 0 ||
        memcmp(header.data, "data", 4) != 0 ||
        header.bits_per_sample != 16 ||
        header.audio_format != 1) {
        ESP_LOGW(TAG, "voice stats: unsupported WAV header format=%u bits=%u",
                 (unsigned)header.audio_format, (unsigned)header.bits_per_sample);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    stats->sample_rate = header.sample_rate;
    stats->bits_per_sample = header.bits_per_sample;
    stats->channels = header.num_channels;
    uint32_t data_bytes = header.data_size;
    size_t max_data = stats->file_size - sizeof(wav_header_t);
    if (data_bytes > max_data) {
        data_bytes = (uint32_t)max_data;
    }
    if (header.byte_rate > 0) {
        stats->duration_ms = (uint32_t)(((uint64_t)data_bytes * 1000U) / header.byte_rate);
    }

    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint64_t total_samples = 0;
    uint64_t sum_squares = 0;
    uint64_t zero_samples = 0;
    uint64_t silent_samples = 0;
    uint32_t peak = 0;
    uint32_t remaining = data_bytes;

    while (remaining > 0) {
        size_t wanted = remaining > 512 ? 512 : remaining;
        size_t n = fread(buf, 1, wanted, f);
        if (n == 0) {
            break;
        }
        remaining -= (uint32_t)n;
        n &= ~(size_t)1;
        for (size_t i = 0; i < n; i += 2) {
            int16_t sample = (int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8));
            int32_t value = sample;
            uint32_t abs_value = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
            if (abs_value > peak) {
                peak = abs_value;
            }
            sum_squares += (uint64_t)abs_value * (uint64_t)abs_value;
            if (abs_value <= 1) {
                zero_samples++;
            }
            if (abs_value <= VOICE_STATS_SILENCE_THRESHOLD) {
                silent_samples++;
            }
            total_samples++;
        }
    }

    free(buf);
    fclose(f);

    if (total_samples == 0) {
        return ESP_FAIL;
    }

    stats->peak_abs = peak;
    stats->rms = isqrt_u64(sum_squares / total_samples);
    stats->dbfs_tenths = approx_dbfs_tenths(stats->rms);
    stats->zero_ratio_per_mille = (uint32_t)((zero_samples * 1000U) / total_samples);
    stats->silence_ratio_per_mille = (uint32_t)((silent_samples * 1000U) / total_samples);
    stats->appears_silent = stats->peak_abs < 128 || stats->rms < 32 || stats->silence_ratio_per_mille > 950;
    stats->valid = true;
    return ESP_OK;
}

static bool sd_mounted(void)
{
    struct stat st = {0};
    return stat("/sdcard", &st) == 0 || Get_SD_Size() > 0;
}

static bool audio_input_available(void)
{
    return esp_get_feed_channel() > 0;
}

static void json_escape_string(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t pos = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    while (*in && pos + 1 < out_len) {
        unsigned char c = (unsigned char)*in++;
        if ((c == '"' || c == '\\') && pos + 2 < out_len) {
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c == '\n' && pos + 2 < out_len) {
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (c == '\r' && pos + 2 < out_len) {
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos] = '\0';
}

static esp_err_t voice_sd_write_test(void)
{
    const char *path = "/sdcard/VTEST.TXT";
    unlink(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "SD write test open failed path=%s errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    const char text[] = "ok\n";
    size_t written = fwrite(text, 1, sizeof(text) - 1, f);
    int close_rc = fclose(f);
    struct stat st = {0};
    int stat_rc = stat(path, &st);
    ESP_LOGI(TAG, "SD write test: written=%u close=%d stat=%d size=%ld",
             (unsigned)written, close_rc, stat_rc, stat_rc == 0 ? (long)st.st_size : -1L);
    unlink(path);
    if (written != sizeof(text) - 1 || close_rc != 0 || stat_rc != 0 || st.st_size <= 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t alloc_record_buffers(int raw_channels,
                                      int32_t **raw_out,
                                      int16_t **mono_out,
                                      int *frames_per_read_out,
                                      size_t *raw_bytes_out)
{
    if (!raw_out || !mono_out || !frames_per_read_out || !raw_bytes_out || raw_channels <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *raw_out = NULL;
    *mono_out = NULL;
    *frames_per_read_out = 0;
    *raw_bytes_out = 0;

    static const size_t candidates[] = {VOICE_I2S_DMA_READ_BYTES, 1024, 512};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        size_t requested = candidates[i];
        size_t frame_bytes = (size_t)raw_channels * sizeof(int32_t);
        size_t raw_bytes = (requested / frame_bytes) * frame_bytes;
        if (raw_bytes < frame_bytes) {
            raw_bytes = frame_bytes;
        }
        int frames = (int)(raw_bytes / frame_bytes);
        size_t mono_bytes = (size_t)frames * sizeof(int16_t);

        ESP_LOGI(TAG, "trying DMA buffer bytes=%u largest_dma=%u free_dma=%u",
                 (unsigned)requested,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));

        int32_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        int16_t *mono = heap_caps_malloc(mono_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (raw && mono) {
            *raw_out = raw;
            *mono_out = mono;
            *frames_per_read_out = frames;
            *raw_bytes_out = raw_bytes;
            ESP_LOGI(TAG, "DMA buffer allocated bytes=%u frames_per_read=%d mono_bytes=%u free_dma=%u largest_dma=%u",
                     (unsigned)raw_bytes,
                     frames,
                     (unsigned)mono_bytes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            return ESP_OK;
        }
        free(raw);
        free(mono);
        ESP_LOGW(TAG, "DMA buffer allocation failed requested=%u raw=%u mono=%u free_dma=%u largest_dma=%u",
                 (unsigned)requested,
                 (unsigned)raw_bytes,
                 (unsigned)mono_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    return ESP_ERR_NO_MEM;
}

static wav_header_t make_wav_header(uint32_t data_size, uint32_t sample_rate)
{
    wav_header_t h = {
        .riff = {'R', 'I', 'F', 'F'},
        .chunk_size = 36 + data_size,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .subchunk1_size = 16,
        .audio_format = 1,
        .num_channels = VOICE_CHANNELS,
        .sample_rate = sample_rate,
        .byte_rate = sample_rate * VOICE_CHANNELS * (VOICE_BITS_PER_SAMPLE / 8),
        .block_align = VOICE_CHANNELS * (VOICE_BITS_PER_SAMPLE / 8),
        .bits_per_sample = VOICE_BITS_PER_SAMPLE,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size,
    };
    return h;
}

static esp_err_t write_wav_header_rate(FILE *f, uint32_t data_size, uint32_t sample_rate)
{
    if (!f) {
        return ESP_ERR_INVALID_ARG;
    }
    wav_header_t header = make_wav_header(data_size, sample_rate);
    if (fseek(f, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fwrite(&header, 1, sizeof(header), f) != sizeof(header)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_wav_header(FILE *f, uint32_t data_size)
{
    return write_wav_header_rate(f, data_size, VOICE_UPLOAD_SAMPLE_RATE);
}

static void debug_wavs_open(voice_debug_wav_t *debug_wavs, size_t count)
{
    if (!debug_wavs) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (!debug_wavs[i].path) {
            continue;
        }
        unlink(debug_wavs[i].path);
        debug_wavs[i].file = fopen(debug_wavs[i].path, "wb");
        if (!debug_wavs[i].file) {
            ESP_LOGW(TAG, "debug WAV open failed path=%s errno=%d (%s)",
                     debug_wavs[i].path, errno, strerror(errno));
            debug_wavs[i].ok = false;
            continue;
        }
        wav_header_t header = make_wav_header(0, VOICE_INPUT_SAMPLE_RATE);
        if (fwrite(&header, 1, sizeof(header), debug_wavs[i].file) != sizeof(header)) {
            ESP_LOGW(TAG, "debug WAV header write failed path=%s errno=%d (%s)",
                     debug_wavs[i].path, errno, strerror(errno));
            fclose(debug_wavs[i].file);
            debug_wavs[i].file = NULL;
            unlink(debug_wavs[i].path);
            debug_wavs[i].ok = false;
            continue;
        }
        debug_wavs[i].ok = true;
        debug_wavs[i].data_size = 0;
    }
}

static void debug_wavs_close(voice_debug_wav_t *debug_wavs, size_t count)
{
    if (!debug_wavs) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (!debug_wavs[i].file) {
            continue;
        }
        if (write_wav_header_rate(debug_wavs[i].file, debug_wavs[i].data_size,
                                  VOICE_INPUT_SAMPLE_RATE) != ESP_OK) {
            ESP_LOGW(TAG, "debug WAV final header failed path=%s", debug_wavs[i].path);
        }
        if (fclose(debug_wavs[i].file) != 0) {
            ESP_LOGW(TAG, "debug WAV close failed path=%s errno=%d (%s)",
                     debug_wavs[i].path, errno, strerror(errno));
        }
        debug_wavs[i].file = NULL;
    }
}

static esp_err_t record_voice_wav(const char *path, size_t *bytes_out)
{
    if (bytes_out) {
        *bytes_out = 0;
    }
    uint32_t duration_ms = VOICE_RECORD_DURATION_MS;
    if (duration_ms > VOICE_MAX_RECORD_DURATION_MS) {
        duration_ms = VOICE_MAX_RECORD_DURATION_MS;
    }
    ESP_LOGI(TAG, "recording started path=%s duration_ms=%u input_sample_rate=%u upload_sample_rate=%u",
             path, (unsigned)duration_ms,
             (unsigned)VOICE_INPUT_SAMPLE_RATE,
             (unsigned)VOICE_UPLOAD_SAMPLE_RATE);
    esp_err_t test_err = voice_sd_write_test();
    if (test_err != ESP_OK) {
        ESP_LOGE(TAG, "SD write test failed before recording");
        return test_err;
    }

    int remove_rc = unlink(path);
    if (remove_rc == 0) {
        ESP_LOGI(TAG, "removed old recording %s", path);
    } else if (errno != ENOENT) {
        ESP_LOGW(TAG, "remove old recording failed path=%s errno=%d (%s)", path, errno, strerror(errno));
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "recording open failed path=%s errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }

    wav_header_t header = make_wav_header(0, VOICE_UPLOAD_SAMPLE_RATE);
    if (fwrite(&header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "recording header write failed errno=%d", errno);
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }

    int raw_channels = esp_get_feed_channel();
    if (raw_channels <= 0 || raw_channels > VOICE_RAW_CHANNELS) {
        ESP_LOGW(TAG, "unexpected feed channels=%d, using %d", raw_channels, VOICE_RAW_CHANNELS);
        raw_channels = VOICE_RAW_CHANNELS;
    }
    ESP_LOGI(TAG, "sample extraction mode=%s default_slot=%d raw_slots=%d",
             extract_mode_name(VOICE_SAMPLE_EXTRACT_MODE), VOICE_DEFAULT_SLOT, raw_channels);
    ESP_LOGI(TAG, "debug WAV writing=%s path=%s slot=%d mode=%s",
             VOICE_ENABLE_DEBUG_WAVS ? "enabled" : "disabled",
             VOICE_DEBUG_WAV_PATH,
             VOICE_DEBUG_SLOT,
             extract_mode_name(VOICE_DEBUG_EXTRACT_MODE));

    voice_level_accum_t levels[3][VOICE_RAW_CHANNELS] = {0};
    voice_debug_wav_t debug_wavs[] = {
        {.path = VOICE_DEBUG_WAV_PATH, .slot = VOICE_DEBUG_SLOT, .mode = VOICE_DEBUG_EXTRACT_MODE},
    };
    const size_t debug_wav_count = sizeof(debug_wavs) / sizeof(debug_wavs[0]);
    if (VOICE_ENABLE_DEBUG_WAVS) {
        debug_wavs_open(debug_wavs, debug_wav_count);
    }

    int32_t *raw = NULL;
    int16_t *mono = NULL;
    int frames_per_read = 0;
    size_t raw_buffer_bytes = 0;
    esp_err_t alloc_err = alloc_record_buffers(raw_channels, &raw, &mono, &frames_per_read, &raw_buffer_bytes);
    if (alloc_err != ESP_OK) {
        ESP_LOGE(TAG, "recording DMA buffer allocation failed internal=%u dma=%u largest_dma=%u heap=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (VOICE_ENABLE_DEBUG_WAVS) {
            debug_wavs_close(debug_wavs, debug_wav_count);
        }
        fclose(f);
        unlink(path);
        return alloc_err;
    }

    const int total_frames = (int)(((uint64_t)VOICE_INPUT_SAMPLE_RATE * duration_ms) / 1000U);
    int frames_read_total = 0;
    uint32_t data_size = 0;
    while (frames_read_total < total_frames) {
        int frames = total_frames - frames_read_total;
        if (frames > frames_per_read) {
            frames = frames_per_read;
        }
        int raw_bytes = frames * raw_channels * sizeof(int32_t);
        if ((size_t)raw_bytes > raw_buffer_bytes) {
            ESP_LOGE(TAG, "recording internal size mismatch raw_bytes=%d buffer=%u",
                     raw_bytes, (unsigned)raw_buffer_bytes);
            if (VOICE_ENABLE_DEBUG_WAVS) {
                debug_wavs_close(debug_wavs, debug_wav_count);
            }
            free(raw);
            free(mono);
            fclose(f);
            unlink(path);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t err = esp_get_feed_data(true, (int16_t *)raw, raw_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "recording read failed: %s", esp_err_to_name(err));
            if (VOICE_ENABLE_DEBUG_WAVS) {
                debug_wavs_close(debug_wavs, debug_wav_count);
            }
            free(raw);
            free(mono);
            fclose(f);
            unlink(path);
            return err;
        }

        int samples_to_write = 0;
        for (int i = 0; i < frames; i++) {
            for (int slot = 0; slot < raw_channels; slot++) {
                int32_t raw_sample = raw[i * raw_channels + slot];
                int16_t low = extract_i2s_sample16(raw_sample, VOICE_EXTRACT_LOW16);
                int16_t high = extract_i2s_sample16(raw_sample, VOICE_EXTRACT_HIGH16);
                int16_t signed_high = extract_i2s_sample16(raw_sample, VOICE_EXTRACT_SIGNED_HIGH16);
                level_accum_add(&levels[VOICE_EXTRACT_LOW16][slot], low);
                level_accum_add(&levels[VOICE_EXTRACT_HIGH16][slot], high);
                level_accum_add(&levels[VOICE_EXTRACT_SIGNED_HIGH16][slot], signed_high);
            }
            int default_slot = VOICE_DEFAULT_SLOT < raw_channels ? VOICE_DEFAULT_SLOT : 0;
            if (((frames_read_total + i) % VOICE_DOWNSAMPLE_RATIO) == 0) {
                mono[samples_to_write++] = extract_i2s_sample16(raw[i * raw_channels + default_slot],
                                                                VOICE_SAMPLE_EXTRACT_MODE);
            }
        }
        size_t written = fwrite(mono, sizeof(int16_t), samples_to_write, f);
        if (written != (size_t)samples_to_write) {
            ESP_LOGE(TAG, "recording data write failed wrote=%u expected=%d errno=%d (%s) offset=%u internal=%u dma=%u largest_dma=%u",
                     (unsigned)written,
                     samples_to_write,
                     errno,
                     strerror(errno),
                     (unsigned)data_size,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            if (VOICE_ENABLE_DEBUG_WAVS) {
                debug_wavs_close(debug_wavs, debug_wav_count);
            }
            free(raw);
            free(mono);
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }

        if (VOICE_ENABLE_DEBUG_WAVS) {
        for (size_t d = 0; d < debug_wav_count; d++) {
            if (!debug_wavs[d].file || debug_wavs[d].slot >= raw_channels) {
                continue;
            }
            for (int i = 0; i < frames; i++) {
                mono[i] = extract_i2s_sample16(raw[i * raw_channels + debug_wavs[d].slot],
                                               debug_wavs[d].mode);
            }
            size_t dbg_written = fwrite(mono, sizeof(int16_t), frames, debug_wavs[d].file);
            if (dbg_written == (size_t)frames) {
                debug_wavs[d].data_size += frames * sizeof(int16_t);
            } else {
                ESP_LOGW(TAG, "debug WAV write failed path=%s wrote=%u expected=%d errno=%d (%s)",
                         debug_wavs[d].path,
                         (unsigned)dbg_written,
                         frames,
                         errno,
                         strerror(errno));
                fclose(debug_wavs[d].file);
                debug_wavs[d].file = NULL;
                unlink(debug_wavs[d].path);
            }
        }
        }
        frames_read_total += frames;
        data_size += samples_to_write * sizeof(int16_t);
    }

    if (write_wav_header(f, data_size) != ESP_OK) {
        ESP_LOGE(TAG, "recording final header write failed errno=%d (%s)", errno, strerror(errno));
        free(raw);
        free(mono);
        fclose(f);
        if (VOICE_ENABLE_DEBUG_WAVS) {
            debug_wavs_close(debug_wavs, debug_wav_count);
        }
        unlink(path);
        return ESP_FAIL;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "recording close failed errno=%d (%s)", errno, strerror(errno));
        free(raw);
        free(mono);
        if (VOICE_ENABLE_DEBUG_WAVS) {
            debug_wavs_close(debug_wavs, debug_wav_count);
        }
        unlink(path);
        return ESP_FAIL;
    }

    if (VOICE_ENABLE_DEBUG_WAVS) {
        debug_wavs_close(debug_wavs, debug_wav_count);
    }

    free(raw);
    free(mono);

    log_mic_debug_stats(levels, raw_channels);
    int best_slot = 0;
    voice_extract_mode_t best_mode = VOICE_SAMPLE_EXTRACT_MODE;
    choose_best_extract(levels, raw_channels, &best_slot, &best_mode);
    uint32_t best_rms = level_accum_rms(&levels[best_mode][best_slot]);
    uint32_t best_peak = levels[best_mode][best_slot].peak_abs;
    ESP_LOGI(TAG, "mic_debug best_candidate slot=%d mode=%s rms=%u peak=%u",
             best_slot,
             extract_mode_name(best_mode),
             (unsigned)best_rms,
             (unsigned)best_peak);
    int default_slot = VOICE_DEFAULT_SLOT < raw_channels ? VOICE_DEFAULT_SLOT : 0;
    ESP_LOGI(TAG, "mic_debug using configured default slot=%d mode=%s for %s",
             default_slot, extract_mode_name(VOICE_SAMPLE_EXTRACT_MODE), path);
    voice_set_selected_extract(default_slot, VOICE_SAMPLE_EXTRACT_MODE);

    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size <= (long)sizeof(wav_header_t)) {
        ESP_LOGE(TAG, "recording stat invalid path=%s errno=%d (%s)", path, errno, strerror(errno));
        unlink(path);
        return ESP_FAIL;
    }
    if (bytes_out) {
        *bytes_out = (size_t)st.st_size;
    }
    return ESP_OK;
}

esp_err_t robot_voice_init(void)
{
    if (!s_voice_lock) {
        s_voice_lock = xSemaphoreCreateMutex();
        if (!s_voice_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    voice_set_state(VOICE_IDLE, NULL);
    ESP_LOGI(TAG, "using shared robot_chat_worker for voice jobs");
    ESP_LOGI(TAG, "no dedicated voice task required");
    return ESP_OK;
}

esp_err_t robot_voice_start_push_to_talk(void)
{
    return robot_voice_start_push_to_talk_ex(NULL, 0);
}

esp_err_t robot_voice_start_push_to_talk_ex(char *error, size_t error_len)
{
    if (!s_voice_lock) {
        if (error && error_len) {
            strlcpy(error, "voice recorder not initialized", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    char last_error[VOICE_ERROR_MAX] = {0};
    int64_t busy_for_ms = 0;
    voice_state_t state = voice_get_state_snapshot(last_error, sizeof(last_error), &busy_for_ms,
                                                   NULL, NULL, NULL, NULL, 0);
    ESP_LOGI(TAG, "record requested");
    ESP_LOGI(TAG, "state before request=%s busy_for_ms=%lld last_error=%s",
             voice_state_to_name(state), (long long)busy_for_ms, last_error[0] ? last_error : "(none)");
    ESP_LOGI(TAG, "audio input available=%s sd mounted=%s free heap=%u free psram=%u target path=%s",
             audio_input_available() ? "true" : "false",
             sd_mounted() ? "true" : "false",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             ROBOT_VOICE_WAV_PATH);

    if (!robot_chat_worker_available()) {
        if (error && error_len) {
            strlcpy(error, "Voice worker not started", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_input_available()) {
        voice_set_state(VOICE_IDLE, "audio input not initialized");
        if (error && error_len) {
            strlcpy(error, "audio input not initialized", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_mounted()) {
        voice_set_state(VOICE_IDLE, "SD card not mounted");
        if (error && error_len) {
            strlcpy(error, "SD card not mounted", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (robot_is_speaking() || Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING) {
        const char *msg = "A robot meg beszel, probald ujra par masodperc mulva.";
        if (error && error_len) {
            strlcpy(error, msg, error_len);
        }
        ESP_LOGW(TAG, "request rejected: audio pipeline currently playing");
        return ESP_ERR_INVALID_STATE;
    }
    if (state != VOICE_IDLE) {
        if (busy_for_ms > VOICE_STALE_BUSY_MS) {
            ESP_LOGW(TAG, "stale busy state detected, resetting");
            robot_voice_force_reset();
            state = VOICE_IDLE;
        } else {
            ESP_LOGW(TAG, "request ignored; state=%s busy_for_ms=%lld last_error=%s",
                     voice_state_to_name(state), (long long)busy_for_ms, last_error[0] ? last_error : "(none)");
            if (error && error_len) {
                snprintf(error, error_len, "voice recorder busy: state=%s", voice_state_to_name(state));
            }
            return ESP_ERR_INVALID_STATE;
        }
    }

    voice_pause_camera_for_workflow();
    voice_set_state(VOICE_RECORDING, NULL);
    esp_err_t err = robot_chat_start_voice_record(error, error_len);
    if (err != ESP_OK) {
        const char *msg = (error && error[0]) ? error : "Voice worker queue unavailable";
        ESP_LOGW(TAG, "%s", msg);
        voice_set_error_then_idle(msg);
        return err;
    }
    ESP_LOGI(TAG, "queued voice job to robot_chat_worker");
    return ESP_OK;
}

esp_err_t robot_voice_record_wav_file(size_t *bytes_out, char *error, size_t error_len)
{
    if (!s_voice_lock) {
        if (error && error_len) {
            strlcpy(error, "voice recorder not initialized", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_input_available()) {
        const char *msg = "audio input not initialized";
        if (error && error_len) {
            strlcpy(error, msg, error_len);
        }
        voice_set_error_then_idle(msg);
        return ESP_ERR_INVALID_STATE;
    }
    if (!sd_mounted()) {
        const char *msg = "SD card not mounted";
        if (error && error_len) {
            strlcpy(error, msg, error_len);
        }
        voice_set_error_then_idle(msg);
        return ESP_ERR_INVALID_STATE;
    }

    voice_pause_camera_for_workflow();
    robot_set_listening();
    esp_err_t err = record_voice_wav(ROBOT_VOICE_WAV_PATH, bytes_out);
    if (err != ESP_OK) {
        char msg[VOICE_ERROR_MAX];
        snprintf(msg, sizeof(msg), "recording failed: %s", esp_err_to_name(err));
        if (error && error_len) {
            strlcpy(error, msg, error_len);
        }
        voice_set_error_then_idle(msg);
        return err;
    }

    voice_set_recording_size(bytes_out ? *bytes_out : 0);
    voice_audio_stats_t stats;
    esp_err_t stats_err = analyze_voice_wav(ROBOT_VOICE_WAV_PATH, &stats);
    if (stats_err == ESP_OK) {
        char dbfs_text[16];
        format_tenths(stats.dbfs_tenths, dbfs_text, sizeof(dbfs_text));
        voice_set_audio_stats(&stats);
        ESP_LOGI(TAG,
                 "voice stats: size=%u sample_rate=%u bits=%u channels=%u duration_ms=%u peak=%u rms=%u dbfs=%s zero_per_mille=%u silence_per_mille=%u",
                 (unsigned)stats.file_size,
                 (unsigned)stats.sample_rate,
                 (unsigned)stats.bits_per_sample,
                 (unsigned)stats.channels,
                 (unsigned)stats.duration_ms,
                 (unsigned)stats.peak_abs,
                 (unsigned)stats.rms,
                 dbfs_text,
                 (unsigned)stats.zero_ratio_per_mille,
                 (unsigned)stats.silence_ratio_per_mille);
        if (stats.appears_silent) {
            ESP_LOGW(TAG, "voice recording appears silent");
        }
    } else {
        ESP_LOGW(TAG, "voice stats: analysis failed: %s", esp_err_to_name(stats_err));
    }
    ESP_LOGI(TAG, "recording complete path=%s size=%u",
             ROBOT_VOICE_WAV_PATH, (unsigned)(bytes_out ? *bytes_out : 0));
    return ESP_OK;
}

void robot_voice_mark_uploading(void)
{
    voice_pause_camera_for_workflow();
    voice_set_state(VOICE_UPLOADING, NULL);
}

void robot_voice_mark_waiting_backend(void)
{
    voice_set_state(VOICE_WAITING_BACKEND, NULL);
}

void robot_voice_mark_playing_reply(void)
{
    voice_set_state(VOICE_PLAYING_REPLY, NULL);
}

void robot_voice_mark_idle(void)
{
    voice_set_state(VOICE_IDLE, NULL);
    voice_resume_camera_after_workflow();
}

void robot_voice_mark_error_idle(const char *error)
{
    voice_set_error_then_idle(error ? error : "voice error");
}

void robot_voice_force_reset(void)
{
    ESP_LOGW(TAG, "force reset requested");
    voice_set_recording_size(0);
    voice_set_state(VOICE_IDLE, "force reset requested");
    voice_resume_camera_after_workflow();
}

const char *robot_voice_state_name(void)
{
    return voice_state_to_name(voice_get_state_snapshot(NULL, 0, NULL, NULL, NULL, NULL, NULL, 0));
}

void robot_voice_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    char last_error[VOICE_ERROR_MAX] = {0};
    char last_error_json[VOICE_ERROR_MAX * 2] = {0};
    int64_t busy_for_ms = 0;
    size_t last_size = 0;
    voice_audio_stats_t stats = {0};
    int selected_slot = 0;
    char selected_mode[20] = {0};
    voice_state_t state = voice_get_state_snapshot(last_error, sizeof(last_error), &busy_for_ms,
                                                   &last_size, &stats,
                                                   &selected_slot, selected_mode, sizeof(selected_mode));
    bool worker_running = robot_chat_worker_available();
    bool available = s_voice_lock && worker_running && audio_input_available() && sd_mounted();
    if (!s_voice_lock && !last_error[0]) {
        strlcpy(last_error, "voice recorder not initialized", sizeof(last_error));
    } else if (!worker_running && !last_error[0]) {
        strlcpy(last_error, "Voice worker not started", sizeof(last_error));
    }
    json_escape_string(last_error, last_error_json, sizeof(last_error_json));
    char dbfs_text[16];
    format_tenths(stats.valid ? stats.dbfs_tenths : -1200, dbfs_text, sizeof(dbfs_text));
    robot_voice_timing_t timing = {0};
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        timing = s_voice_status.last_timing;
        xSemaphoreGive(s_voice_lock);
    }
    snprintf(out, out_len,
             "{\"voice_state\":\"%s\",\"last_voice_error\":\"%s\","
             "\"last_recording_size\":%u,\"voice_available\":%s,"
             "\"worker_running\":%s,\"busy_for_ms\":%lld,"
             "\"sample_rate\":%u,\"bits_per_sample\":%u,\"channels\":%u,"
             "\"duration_ms\":%u,\"peak_abs\":%u,\"rms\":%u,"
             "\"dbfs\":\"%s\",\"dbfs_tenths\":%d,"
             "\"zero_ratio_per_mille\":%u,\"silence_ratio_per_mille\":%u,"
             "\"appears_silent\":%s,\"stats_valid\":%s,"
             "\"selected_slot\":%d,\"selected_mode\":\"%s\","
             "\"voice_timing_record_ms\":%u,"
             "\"voice_timing_upload_ms\":%u,"
             "\"voice_timing_backend_ms\":%u,"
             "\"voice_timing_audio_download_ms\":%u,"
             "\"voice_timing_total_ms\":%u,"
             "\"voice_timing_wav_bytes\":%u,"
             "\"voice_timing_sample_rate\":%u,"
             "\"record_ms\":%u,"
             "\"upload_ms\":%u,"
             "\"backend_ms\":%u,"
             "\"audio_download_ms\":%u,"
             "\"total_ms\":%u,"
             "\"wav_bytes\":%u}",
             voice_state_to_name(state),
             last_error_json,
             (unsigned)last_size,
             available ? "true" : "false",
             worker_running ? "true" : "false",
             (long long)busy_for_ms,
             (unsigned)stats.sample_rate,
             (unsigned)stats.bits_per_sample,
             (unsigned)stats.channels,
             (unsigned)stats.duration_ms,
             (unsigned)stats.peak_abs,
             (unsigned)stats.rms,
             dbfs_text,
             (int)(stats.valid ? stats.dbfs_tenths : -1200),
             (unsigned)stats.zero_ratio_per_mille,
             (unsigned)stats.silence_ratio_per_mille,
             stats.appears_silent ? "true" : "false",
             stats.valid ? "true" : "false",
             selected_slot,
             selected_mode[0] ? selected_mode : "unknown",
             (unsigned)timing.record_ms,
             (unsigned)timing.upload_ms,
             (unsigned)timing.backend_ms,
             (unsigned)timing.audio_download_ms,
             (unsigned)timing.total_ms,
             (unsigned)timing.wav_bytes,
             (unsigned)timing.sample_rate,
             (unsigned)timing.record_ms,
             (unsigned)timing.upload_ms,
             (unsigned)timing.backend_ms,
             (unsigned)timing.audio_download_ms,
             (unsigned)timing.total_ms,
             (unsigned)timing.wav_bytes);
}
