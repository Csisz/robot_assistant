#include "face_enroll.h"
#include "camera_web_server.h"
#include "face_detect.h"
#include "robot_state.h"

#include "esp_log.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>

static const char *TAG = "face_enroll";

/* All paths are FAT 8.3 safe:
   /sdcard/FACES/        — 5-char directory
   /sdcard/FACES/<ID>/   — ID max 8 chars (enforced)
   /sdcard/FACES/<ID>/S01.JPG … S09.JPG — 3-char base + 3-char ext
   /sdcard/FACES/DB.TXT  — 2-char base + 3-char ext                */
#define FACES_DIR   "/sdcard/FACES"
#define DB_TXT      "/sdcard/FACES/DB.TXT"
#define MIN_SAMPLES 3
#define MAX_SAMPLES 9

static struct {
    volatile bool active;
    char  id[9];          /* original id as entered, max 8 chars */
    char  id_upper[9];    /* uppercased, used for filesystem paths */
    char  display_name[64];
    char  audio_file[32]; /* /sdcard/XXXX.MP3 — always short */
    int   sample_count;
} s_ctx;

/* ------------------------------------------------------------------ */
/* Validation helpers                                                  */
/* ------------------------------------------------------------------ */

static bool id_valid(const char *id)
{
    if (!id || id[0] == '\0') return false;
    size_t len = strlen(id);
    if (len > 8) return false;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}

static const char * const s_valid_audio[] = {
    "/sdcard/APA.MP3",
    "/sdcard/ANYA.MP3",
    "/sdcard/ZITA.MP3",
    "/sdcard/IDA.MP3",
    "/sdcard/ZSOLI.MP3",
    "/sdcard/TEKI.MP3",
    NULL
};

static bool audio_valid(const char *path)
{
    if (!path || path[0] == '\0') return true; /* audio is optional */
    for (int i = 0; s_valid_audio[i]; i++) {
        if (strcmp(path, s_valid_audio[i]) == 0) return true;
    }
    return false;
}

static void str_upper(const char *src, char *dst, size_t dst_len)
{
    size_t i;
    for (i = 0; src[i] && i < dst_len - 1; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Minimal JSON string extractor — no external library needed.        */
/* Finds "key":"value" and copies value into out.                     */
/* ------------------------------------------------------------------ */

static void json_str(const char *body, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* ------------------------------------------------------------------ */

esp_err_t face_enroll_init(void)
{
    memset((void *)&s_ctx, 0, sizeof(s_ctx));
    mkdir(FACES_DIR, 0755);
    return ESP_OK;
}

bool face_enroll_is_active(void)
{
    return s_ctx.active;
}

enroll_status_t face_enroll_get_status(void)
{
    enroll_status_t st = {
        .active       = s_ctx.active,
        .sample_count = s_ctx.sample_count,
    };
    strlcpy(st.id,           s_ctx.id,           sizeof(st.id));
    strlcpy(st.display_name, s_ctx.display_name, sizeof(st.display_name));
    return st;
}

/* ------------------------------------------------------------------ */
/* Session management                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t start_session(const char *id, const char *display_name,
                                const char *audio_file)
{
    if (!id_valid(id)) {
        ESP_LOGE(TAG, "Invalid id '%s': max 8 chars, A-Za-z0-9_ only",
                 id ? id : "null");
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_name || display_name[0] == '\0') {
        ESP_LOGE(TAG, "display_name is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_valid(audio_file)) {
        ESP_LOGE(TAG, "Unknown audio_file '%s'", audio_file);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx.active) {
        ESP_LOGW(TAG, "Previous session active — cancelling");
        face_enroll_cancel();
    }

    memset((void *)&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.id,           id,           sizeof(s_ctx.id));
    strlcpy(s_ctx.display_name, display_name, sizeof(s_ctx.display_name));
    if (audio_file) strlcpy(s_ctx.audio_file, audio_file, sizeof(s_ctx.audio_file));
    str_upper(id, s_ctx.id_upper, sizeof(s_ctx.id_upper));

    /* Create per-person directory with uppercase name */
    char dir[64];
    mkdir(FACES_DIR, 0755);
    snprintf(dir, sizeof(dir), "%s/%s", FACES_DIR, s_ctx.id_upper);
    mkdir(dir, 0755);

    s_ctx.sample_count = 0;
    s_ctx.active       = true;

    ESP_LOGI(TAG, "Started: id='%s' dir='%s' name='%s' audio='%s'",
             s_ctx.id, dir, s_ctx.display_name, s_ctx.audio_file);
    return ESP_OK;
}

esp_err_t face_enroll_start_json(const char *json_body)
{
    if (!json_body) return ESP_ERR_INVALID_ARG;

    char id[16] = {0}, name[80] = {0}, audio[40] = {0};
    json_str(json_body, "id",           id,    sizeof(id));
    json_str(json_body, "display_name", name,  sizeof(name));
    json_str(json_body, "audio_file",   audio, sizeof(audio));

    return start_session(id, name, audio);
}

void face_enroll_cancel(void)
{
    ESP_LOGI(TAG, "Cancelled: '%s'", s_ctx.id);
    memset((void *)&s_ctx, 0, sizeof(s_ctx));
}

/* ------------------------------------------------------------------ */
/* Sample capture                                                      */
/* ------------------------------------------------------------------ */

esp_err_t face_enroll_capture(int *sample_count_out)
{
    if (!s_ctx.active) return ESP_ERR_INVALID_STATE;

    if (s_ctx.sample_count >= MAX_SAMPLES) {
        ESP_LOGW(TAG, "Already at max samples (%d)", MAX_SAMPLES);
        return ESP_ERR_NOT_SUPPORTED; /* sentinel: max reached */
    }

    if (robot_is_speaking()) {
        ESP_LOGW(TAG, "Capture blocked: audio is playing");
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = NULL;
    if (camera_capture_frame(&fb) != ESP_OK || !fb) {
        ESP_LOGE(TAG, "camera_capture_frame failed");
        return ESP_FAIL;
    }

    int    w   = (int)fb->width;
    int    h   = (int)fb->height;
    size_t len = fb->len;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        camera_release_frame(fb);
        ESP_LOGE(TAG, "PSRAM alloc failed (%zu B)", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, fb->buf, len);
    camera_release_frame(fb);

    int bbox[4] = {0};
    int count = face_detect_run_ex((const uint16_t *)buf, w, h, bbox);
    ESP_LOGI(TAG, "Capture: %d face(s) at [%d,%d,%d,%d]",
             count, bbox[0], bbox[1], bbox[2], bbox[3]);

    if (count == 0) {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    if (count > 1) {
        free(buf);
        ESP_LOGW(TAG, "%d faces in frame — rejected", count);
        return ESP_ERR_INVALID_STATE;
    }

    /* Encode full frame to JPEG */
    uint8_t *jpg     = NULL;
    size_t   jpg_len = 0;
    bool ok = fmt2jpg(buf, len, (uint16_t)w, (uint16_t)h,
                      PIXFORMAT_RGB565, 80, &jpg, &jpg_len);
    free(buf);

    if (!ok || !jpg) {
        ESP_LOGE(TAG, "JPEG encode failed");
        free(jpg);
        return ESP_FAIL;
    }

    /* 8.3-safe path: /sdcard/FACES/APA/S01.JPG */
    s_ctx.sample_count++;
    char path[64];
    snprintf(path, sizeof(path), "%s/%s/S%02d.JPG",
             FACES_DIR, s_ctx.id_upper, s_ctx.sample_count);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open '%s'", path);
        free(jpg);
        s_ctx.sample_count--;
        return ESP_FAIL;
    }
    size_t written = fwrite(jpg, 1, jpg_len, f);
    fclose(f);
    free(jpg);

    if (written != jpg_len) {
        ESP_LOGE(TAG, "SD write incomplete %zu/%zu", written, jpg_len);
        s_ctx.sample_count--;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sample %d → %s (%zu B)", s_ctx.sample_count, path, jpg_len);
    if (sample_count_out) *sample_count_out = s_ctx.sample_count;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* DB.TXT persistence                                                  */
/* Format: id,display_name,audio_file,sample_count                   */
/* Example: apa,Apa,/sdcard/APA.MP3,9                                */
/* ------------------------------------------------------------------ */

#define DB_MAX_LINES 32
#define DB_LINE_MAX  128

static esp_err_t db_update(const char *id, const char *display_name,
                            const char *audio_file, int samples)
{
    char  *lines[DB_MAX_LINES] = {0};
    int    n     = 0;
    size_t id_len = strlen(id);

    /* Read existing lines, skipping any entry with the same id */
    FILE *f = fopen(DB_TXT, "r");
    if (f) {
        char buf[DB_LINE_MAX];
        while (n < DB_MAX_LINES && fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, id, id_len) == 0 && buf[id_len] == ',') continue;
            lines[n] = strdup(buf);
            if (lines[n]) n++;
        }
        fclose(f);
    }

    /* Write back filtered lines + new entry */
    f = fopen(DB_TXT, "w");
    if (!f) {
        for (int i = 0; i < n; i++) free(lines[i]);
        ESP_LOGE(TAG, "Cannot open " DB_TXT " for writing");
        return ESP_FAIL;
    }
    for (int i = 0; i < n; i++) {
        if (lines[i]) { fputs(lines[i], f); free(lines[i]); }
    }
    fprintf(f, "%s,%s,%s,%d\n", id,
            display_name ? display_name : "",
            audio_file   ? audio_file   : "",
            samples);
    fclose(f);

    ESP_LOGI(TAG, DB_TXT " updated: %s/%s samples=%d", id, display_name, samples);
    return ESP_OK;
}

esp_err_t face_enroll_finish(int *samples_out, char *id_out, size_t id_out_len)
{
    if (!s_ctx.active) return ESP_ERR_INVALID_STATE;
    if (s_ctx.sample_count < MIN_SAMPLES) {
        ESP_LOGW(TAG, "Need %d samples, have %d", MIN_SAMPLES, s_ctx.sample_count);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = db_update(s_ctx.id, s_ctx.display_name,
                               s_ctx.audio_file, s_ctx.sample_count);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Finished: '%s' (%d samples)", s_ctx.id, s_ctx.sample_count);

    if (samples_out) *samples_out = s_ctx.sample_count;
    /* Return uppercase id for UI display ("APA" not "apa") */
    if (id_out) strlcpy(id_out, s_ctx.id_upper, id_out_len);

    memset((void *)&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}
