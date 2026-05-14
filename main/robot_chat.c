#include "robot_chat.h"
#include "kids_safety.h"
#include "robot_state.h"
#include "face_status.h"
#include "audio_driver.h"
#include "bsp_board.h"
#include "robot_voice.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "robot_chat";

#define CHAT_TIMEOUT_MS 15000
#define CHAT_AUDIO_TIMEOUT_MS 15000
#define CHAT_REQ_MAX    1400
#define CHAT_RESP_MAX   4096
#define ROBOT_REPLY_AUDIO_PATH "/sdcard/REPLY.MP3"
#define ROBOT_REPLY_AUDIO_URL  "file://sdcard/REPLY.MP3"
#define CHAT_AUDIO_CHUNK 512
#define CHAT_VOICE_PATH_MAX 64
#define CHAT_VOICE_TIMEOUT_MS 90000
#define CHAT_VOICE_UPLOAD_START_PATH  "/api/robot/voice-upload/start"
#define CHAT_VOICE_UPLOAD_CHUNK_PATH  "/api/robot/voice-upload/chunk"
#define CHAT_VOICE_UPLOAD_FINISH_PATH "/api/robot/voice-upload/finish"
#define CHAT_VOICE_UPLOAD_CANCEL_PATH "/api/robot/voice-upload/cancel"
#ifndef VOICE_UPLOAD_CHUNK_BYTES
#define VOICE_UPLOAD_CHUNK_BYTES 8192
#endif
#define CHAT_VOICE_UPLOAD_CHUNK_SIZE VOICE_UPLOAD_CHUNK_BYTES
#define CHAT_VOICE_UPLOAD_CHUNK_TIMEOUT_MS 5000
#define CHAT_VOICE_UPLOAD_CHUNK_MAX_ATTEMPTS 3
#define CHAT_VOICE_UPLOAD_SAMPLE_RATE 16000
#define CHAT_WORKER_STACK_BYTES (24 * 1024)
#define CHAT_WORKER_QUEUE_LEN   1
#define CHAT_WAIT_TIMEOUT_MS    90000
#define CHAT_EVENT_DONE         BIT0

static char s_backend_url[192];
static char s_backend_base_url[160];
static char s_voice_start_url[192];
static char s_voice_chunk_url[192];
static char s_voice_finish_url[192];
static char s_voice_cancel_url[192];
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_worker_lock;
static QueueHandle_t s_worker_queue;
static EventGroupHandle_t s_worker_events;
static TaskHandle_t s_worker_task;
static char s_conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
static char s_profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
static char s_active_mode[ROBOT_CHAT_ACTIVE_MODE_MAX] = "kids_chat";

typedef struct {
    uint32_t upload_ms;
    uint32_t backend_ms;
} voice_request_timing_t;

typedef enum {
    ROBOT_CHAT_JOB_TEXT = 0,
    ROBOT_CHAT_JOB_VOICE_FILE = 1,
    ROBOT_CHAT_JOB_VOICE_RECORD = 2,
} robot_chat_job_kind_t;

typedef struct {
    bool busy;
    bool waiting;
    robot_chat_job_kind_t kind;
    char message[ROBOT_CHAT_MESSAGE_MAX];
    char path[CHAT_VOICE_PATH_MAX];
    robot_chat_response_t response;
    esp_err_t err;
} robot_chat_job_t;

static robot_chat_job_t s_job;

typedef struct {
    char *buf;
    int len;
    int cap;
    bool overflow;
} response_buf_t;

static int64_t chat_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t robot_chat_execute_text(const char *message, robot_chat_response_t *out);
static esp_err_t robot_chat_execute_voice_file(const char *path, robot_chat_response_t *out,
                                               voice_request_timing_t *timing,
                                               uint32_t *audio_download_ms_out);
static esp_err_t robot_chat_execute_voice_record(robot_chat_response_t *out);
static void robot_chat_worker_task(void *arg);

static void strip_trailing_slash(char *text)
{
    if (!text) {
        return;
    }
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == '/') {
        text[--len] = '\0';
    }
}

static esp_err_t robot_chat_build_backend_urls(const char *backend_base_url)
{
    if (!backend_base_url || !backend_base_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_backend_base_url, backend_base_url, sizeof(s_backend_base_url));
    strip_trailing_slash(s_backend_base_url);
    if (!s_backend_base_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_backend_url, sizeof(s_backend_url), "%s%s",
             s_backend_base_url, CONFIG_ROBOT_CHAT_BACKEND_PATH);
    snprintf(s_voice_start_url, sizeof(s_voice_start_url), "%s%s",
             s_backend_base_url, CHAT_VOICE_UPLOAD_START_PATH);
    snprintf(s_voice_chunk_url, sizeof(s_voice_chunk_url), "%s%s",
             s_backend_base_url, CHAT_VOICE_UPLOAD_CHUNK_PATH);
    snprintf(s_voice_finish_url, sizeof(s_voice_finish_url), "%s%s",
             s_backend_base_url, CHAT_VOICE_UPLOAD_FINISH_PATH);
    snprintf(s_voice_cancel_url, sizeof(s_voice_cancel_url), "%s%s",
             s_backend_base_url, CHAT_VOICE_UPLOAD_CANCEL_PATH);
    ESP_LOGI(TAG, "backend_base_url=%s", s_backend_base_url);
    ESP_LOGI(TAG, "backend URL = %s", s_backend_url);
    ESP_LOGI(TAG, "voice upload start URL = %s", s_voice_start_url);
    return ESP_OK;
}

static const char *fallback_text(void)
{
    return "Most nem érem el az internetet, próbáljuk meg később.";
}

static void chat_state_snapshot(char *conversation_id, size_t conversation_len,
                                char *profile_id, size_t profile_len,
                                char *active_mode, size_t active_len)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    if (conversation_id && conversation_len) {
        strlcpy(conversation_id, s_conversation_id, conversation_len);
    }
    if (profile_id && profile_len) {
        strlcpy(profile_id, s_profile_id, profile_len);
    }
    if (active_mode && active_len) {
        strlcpy(active_mode, s_active_mode[0] ? s_active_mode : "kids_chat", active_len);
    }
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

static void chat_state_update(const robot_chat_response_t *resp)
{
    if (!resp) {
        return;
    }
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    if (resp->conversation_id[0]) {
        strlcpy(s_conversation_id, resp->conversation_id, sizeof(s_conversation_id));
    }
    if (resp->profile_id[0]) {
        strlcpy(s_profile_id, resp->profile_id, sizeof(s_profile_id));
    }
    if (resp->active_mode[0]) {
        strlcpy(s_active_mode, resp->active_mode, sizeof(s_active_mode));
    }
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

static esp_err_t chat_http_event(esp_http_client_event_t *evt)
{
    response_buf_t *rb = (response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGW(TAG, "HTTP client event: error");
    } else if (evt->event_id == HTTP_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "HTTP client event: disconnected");
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len > 0) {
        int copy = evt->data_len;
        if (copy > rb->cap - rb->len - 1) {
            copy = rb->cap - rb->len - 1;
            rb->overflow = true;
        }
        if (copy > 0) {
            memcpy(rb->buf + rb->len, evt->data, copy);
            rb->len += copy;
            rb->buf[rb->len] = '\0';
        } else {
            rb->overflow = true;
        }
    }
    return ESP_OK;
}

static void log_http_failure(esp_err_t err, int status)
{
    if (err == ESP_ERR_HTTP_CONNECT) {
        ESP_LOGE(TAG, "DNS/connection failure for %s", s_backend_url);
    } else if (err == ESP_ERR_HTTP_CONNECTING ||
               err == ESP_ERR_HTTP_EAGAIN ||
               err == ESP_ERR_HTTP_READ_TIMEOUT ||
               err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "timeout while calling %s (%s)", s_backend_url, esp_err_to_name(err));
    } else if (err == ESP_ERR_HTTP_CONNECTION_CLOSED) {
        ESP_LOGE(TAG, "connection closed by backend");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP client failure: %s", esp_err_to_name(err));
    }

    if (status > 0 && (status < 200 || status >= 300)) {
        ESP_LOGE(TAG, "HTTP status code: %d", status);
    }
}

static void log_sd_open_failure(const char *path, int saved_errno)
{
    uint32_t sd_mb = Get_SD_Size();
    ESP_LOGE(TAG, "audio download: failed to open %s: errno=%d (%s)",
             path ? path : "(null)", saved_errno, strerror(saved_errno));
    ESP_LOGE(TAG, "audio download: SD size=%lu MB, heap=%u, psram=%u",
             (unsigned long)sd_mb,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    FILE *test = fopen("/sdcard/TEST.TXT", "wb");
    if (!test) {
        int test_errno = errno;
        ESP_LOGE(TAG, "SD self-test: failed to open /sdcard/TEST.TXT: errno=%d (%s)",
                 test_errno, strerror(test_errno));
        return;
    }

    const char probe[] = "robot_chat_sd_test\n";
    size_t written = fwrite(probe, 1, sizeof(probe) - 1, test);
    int close_rc = fclose(test);
    int close_errno = errno;
    struct stat st = {0};
    int stat_rc = stat("/sdcard/TEST.TXT", &st);
    int stat_errno = errno;
    ESP_LOGI(TAG, "SD self-test: written=%u close=%d stat=%d size=%ld",
             (unsigned)written, close_rc, stat_rc, stat_rc == 0 ? (long)st.st_size : -1L);
    if (close_rc != 0) {
        ESP_LOGW(TAG, "SD self-test: close errno=%d (%s)", close_errno, strerror(close_errno));
    }
    if (stat_rc != 0) {
        ESP_LOGW(TAG, "SD self-test: stat errno=%d (%s)", stat_errno, strerror(stat_errno));
    }
    int unlink_rc = unlink("/sdcard/TEST.TXT");
    int unlink_errno = errno;
    ESP_LOGI(TAG, "SD self-test: remove /sdcard/TEST.TXT rc=%d", unlink_rc);
    if (unlink_rc != 0) {
        ESP_LOGW(TAG, "SD self-test: remove errno=%d (%s)", unlink_errno, strerror(unlink_errno));
    }
}

static esp_err_t robot_chat_download_audio(const char *url, const char *path, size_t *bytes_out)
{
    if (bytes_out) {
        *bytes_out = 0;
    }
    if (!url || !url[0] || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "downloading audio to %s", path);
    ESP_LOGI(TAG, "audio download heap: internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CHAT_AUDIO_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "audio download: http client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio download open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "audio download HTTP status=%d content_length=%lld", status, (long long)content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int remove_rc = unlink(path);
    if (remove_rc == 0) {
        ESP_LOGI(TAG, "removed previous audio file: %s", path);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (errno == ENOENT) {
        ESP_LOGI(TAG, "no previous audio file to remove: %s", path);
    } else {
        int saved_errno = errno;
        ESP_LOGW(TAG, "failed to remove previous audio file %s: errno=%d (%s)",
                 path, saved_errno, strerror(saved_errno));
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        int saved_errno = errno;
        log_sd_open_failure(path, saved_errno);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc(CHAT_AUDIO_CHUNK,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "audio download: failed to allocate DMA buffer size=%u internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
                 (unsigned)CHAT_AUDIO_CHUNK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        fclose(f);
        unlink(path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (true) {
        int r = esp_http_client_read(client, (char *)buf, CHAT_AUDIO_CHUNK);
        if (r < 0) {
            ESP_LOGE(TAG, "audio download read failed");
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        size_t written = fwrite(buf, 1, (size_t)r, f);
        if (written != (size_t)r) {
            ESP_LOGE(TAG, "MP3 download failed because SD write failed");
            ESP_LOGE(TAG, "audio download write failed: offset=%u chunk=%d wrote=%u errno=%d (%s) internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
                     (unsigned)total,
                     r,
                     (unsigned)written,
                     errno,
                     strerror(errno),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            err = ESP_FAIL;
            break;
        }
        total += written;
    }

    free(buf);
    if (fclose(f) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "audio download fclose failed: errno=%d", errno);
        err = ESP_FAIL;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        unlink(path);
        return err;
    }
    if (total == 0) {
        ESP_LOGE(TAG, "audio download produced zero byte file");
        unlink(path);
        return ESP_FAIL;
    }

    if (bytes_out) {
        *bytes_out = total;
    }
    struct stat st = {0};
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "audio download complete, size=%ld", (long)st.st_size);
        if (st.st_size <= 0) {
            ESP_LOGE(TAG, "audio download stat shows empty file");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "audio download complete, size=%u", (unsigned)total);
    }
    return ESP_OK;
}

static const char *robot_chat_audio_url(const char *reply_audio_url,
                                        char *buf,
                                        size_t buf_len)
{
    if (!reply_audio_url || !reply_audio_url[0]) {
        return "";
    }
    if (strncmp(reply_audio_url, "http://", 7) == 0 ||
        strncmp(reply_audio_url, "https://", 8) == 0 ||
        strncmp(reply_audio_url, "file://", 7) == 0) {
        return reply_audio_url;
    }
    if (!buf || buf_len == 0 || !s_backend_base_url[0]) {
        return reply_audio_url;
    }
    if (reply_audio_url[0] == '/') {
        snprintf(buf, buf_len, "%s%s", s_backend_base_url, reply_audio_url);
    } else {
        snprintf(buf, buf_len, "%s/%s", s_backend_base_url, reply_audio_url);
    }
    return buf;
}

static bool robot_chat_play_downloaded_audio(const robot_chat_response_t *out,
                                             uint32_t *download_ms_out)
{
    if (download_ms_out) {
        *download_ms_out = 0;
    }
    if (!out || !out->reply_audio_url[0]) {
        ESP_LOGI(TAG, "audio URL missing, text-only response");
        return false;
    }

    if (robot_is_speaking() || Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING) {
        ESP_LOGW(TAG, "audio already playing, stopping current playback before robot reply");
        Audio_Stop_Play();
        robot_set_idle();
        vTaskDelay(pdMS_TO_TICKS(100));
        robot_set_thinking();
    }

    size_t bytes = 0;
    char audio_url[ROBOT_CHAT_AUDIO_URL_MAX + 160];
    const char *download_url = robot_chat_audio_url(out->reply_audio_url,
                                                    audio_url,
                                                    sizeof(audio_url));
    int64_t download_start_ms = chat_now_ms();
    esp_err_t err = robot_chat_download_audio(download_url, ROBOT_REPLY_AUDIO_PATH, &bytes);
    if (download_ms_out) {
        *download_ms_out = (uint32_t)(chat_now_ms() - download_start_ms);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio download failed: %s; using text-only response", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "audio download verified, bytes=%u", (unsigned)bytes);

    struct stat st = {0};
    if (stat(ROBOT_REPLY_AUDIO_PATH, &st) == 0) {
        ESP_LOGI(TAG, "audio file ready: %s size=%ld", ROBOT_REPLY_AUDIO_PATH, (long)st.st_size);
        if (st.st_size <= 0) {
            ESP_LOGE(TAG, "audio file is empty, not playing");
            return false;
        }
    } else {
        ESP_LOGE(TAG, "audio file stat failed after download: %s errno=%d (%s)",
                 ROBOT_REPLY_AUDIO_PATH, errno, strerror(errno));
        return false;
    }
    face_status_set_last_audio(ROBOT_REPLY_AUDIO_PATH);
    ESP_LOGI(TAG, "playing downloaded audio: %s", ROBOT_REPLY_AUDIO_PATH);
    robot_say_file(out->reply_text, ROBOT_REPLY_AUDIO_URL);
    return true;
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    size_t pos = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (const char *s = src; *s && pos + 1 < dst_len; s++) {
        char c = *s;
        if ((c == '"' || c == '\\') && pos + 2 < dst_len) {
            dst[pos++] = '\\';
            dst[pos++] = c;
        } else if (c == '\n' && pos + 2 < dst_len) {
            dst[pos++] = '\\';
            dst[pos++] = 'n';
        } else if (c != '\r') {
            dst[pos++] = c;
        }
    }
    dst[pos] = '\0';
}

static bool json_bool_value(const char *json, const char *key, bool fallback)
{
    char search[40];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        return fallback;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        return false;
    }
    return fallback;
}

static int json_int_value(const char *json, const char *key, int fallback)
{
    char search[40];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        return fallback;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return (int)strtol(p, NULL, 10);
}

static void json_string_value(const char *json, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!json) {
        return;
    }

    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        return;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (strncmp(p, "null", 4) == 0) {
        return;
    }
    if (*p != '"') {
        return;
    }
    p++;

    size_t pos = 0;
    while (*p && *p != '"' && pos + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') {
                out[pos++] = ' ';
            } else {
                out[pos++] = *p;
            }
            p++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}

static void parse_chat_response_json(const char *resp, robot_chat_response_t *out)
{
    if (!resp || !out) {
        return;
    }
    out->ok = json_bool_value(resp, "ok", false);
    out->safe = json_bool_value(resp, "safe", false);
    json_string_value(resp, "transcript", out->transcript, sizeof(out->transcript));
    json_string_value(resp, "conversation_id", out->conversation_id, sizeof(out->conversation_id));
    json_string_value(resp, "profile_id", out->profile_id, sizeof(out->profile_id));
    json_string_value(resp, "reply_text", out->reply_text, sizeof(out->reply_text));
    json_string_value(resp, "reply_audio_url", out->reply_audio_url, sizeof(out->reply_audio_url));
    json_string_value(resp, "robot_mood", out->robot_mood, sizeof(out->robot_mood));
    json_string_value(resp, "robot_action", out->robot_action, sizeof(out->robot_action));
    json_string_value(resp, "blocked_reason", out->blocked_reason, sizeof(out->blocked_reason));
    json_string_value(resp, "active_mode", out->active_mode, sizeof(out->active_mode));
    chat_state_update(out);
}

static void log_chat_response_summary(const robot_chat_response_t *out)
{
    if (!out) {
        return;
    }
    if (out->transcript[0]) {
        ESP_LOGI(TAG, "transcript=%s", out->transcript);
    }
    ESP_LOGI(TAG, "conversation_id received = %s",
             out->conversation_id[0] ? out->conversation_id : "(missing)");
    ESP_LOGI(TAG, "profile_id = %s", out->profile_id[0] ? out->profile_id : "(missing)");
    ESP_LOGI(TAG, "active_mode = %s", out->active_mode[0] ? out->active_mode : "(missing)");
    ESP_LOGI(TAG, "%s", out->safe ? "safe response" : "blocked response");
    ESP_LOGI(TAG, "reply length=%u", (unsigned)strlen(out->reply_text));
    if (out->reply_audio_url[0]) {
        ESP_LOGI(TAG, "audio URL received = %s", out->reply_audio_url);
    } else {
        ESP_LOGI(TAG, "audio URL missing, text-only response");
    }
}

static esp_err_t make_request_body(const char *message,
                                   const char *conversation_id,
                                   const char *profile_id,
                                   char *body,
                                   size_t body_len)
{
    char escaped[640];
    json_escape(message, escaped, sizeof(escaped));

    char conv_part[140] = {0};
    char profile_part[80] = {0};
    if (conversation_id && conversation_id[0]) {
        char escaped_conv[ROBOT_CHAT_CONVERSATION_ID_MAX + 8];
        json_escape(conversation_id, escaped_conv, sizeof(escaped_conv));
        snprintf(conv_part, sizeof(conv_part), "\"conversation_id\":\"%s\",", escaped_conv);
    }
    if (profile_id && profile_id[0]) {
        char escaped_profile[ROBOT_CHAT_PROFILE_ID_MAX + 8];
        json_escape(profile_id, escaped_profile, sizeof(escaped_profile));
        snprintf(profile_part, sizeof(profile_part), "\"profile_id\":\"%s\",", escaped_profile);
    }

    int n = snprintf(body, body_len,
        "{%s%s"
        "\"device_id\":\"%s\","
        "\"locale\":\"%s\","
        "\"mode\":\"kids_chat\","
        "\"message\":\"%s\","
        "\"max_answer_seconds\":30,"
        "\"allowed_topics\":["
        "\"short_story\",\"weather\",\"basic_math\",\"colors\","
        "\"animals\",\"nature\",\"kindness\",\"daily_routine\"]}",
        conv_part,
        profile_part,
        CONFIG_ROBOT_CHAT_DEVICE_ID,
        CONFIG_ROBOT_CHAT_LOCALE,
        escaped);

    if (n < 0 || n >= (int)body_len) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t perform_request_once(const char *body, robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!body || !body[0]) {
        strlcpy(out->error, "empty request body", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_backend_url[0]) {
        strlcpy(out->error, "backend URL missing", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    char *resp = heap_caps_calloc(1, CHAT_RESP_MAX, MALLOC_CAP_8BIT);
    if (!resp) {
        strlcpy(out->error, "response buffer allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    response_buf_t rb = {
        .buf = resp,
        .len = 0,
        .cap = CHAT_RESP_MAX,
        .overflow = false,
    };

    esp_http_client_config_t cfg = {
        .url = s_backend_url,
        .timeout_ms = CHAT_TIMEOUT_MS,
        .event_handler = chat_http_event,
        .user_data = &rb,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        strlcpy(out->error, "http client init failed", sizeof(out->error));
        free(resp);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    out->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "backend response status=%d err=%s",
             out->http_status, esp_err_to_name(err));
    esp_http_client_cleanup(client);

    if (rb.overflow) {
        ESP_LOGE(TAG, "backend response too large for %d byte buffer", CHAT_RESP_MAX);
        strlcpy(out->error, "backend response too large", sizeof(out->error));
        free(resp);
        return ESP_ERR_NO_MEM;
    }

    if (err != ESP_OK || out->http_status < 200 || out->http_status >= 300) {
        log_http_failure(err, out->http_status);
        snprintf(out->error, sizeof(out->error), "backend unavailable status=%d",
                 out->http_status);
        free(resp);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    parse_chat_response_json(resp, out);
    log_chat_response_summary(out);
    free(resp);
    return ESP_OK;
}

static bool url_append_char(char *out, size_t out_len, size_t *pos, char c)
{
    if (!out || !pos || *pos + 1 >= out_len) {
        return false;
    }
    out[(*pos)++] = c;
    out[*pos] = '\0';
    return true;
}

static bool url_append_str(char *out, size_t out_len, size_t *pos, const char *text)
{
    if (!text) {
        return true;
    }
    while (*text) {
        if (!url_append_char(out, out_len, pos, *text++)) {
            return false;
        }
    }
    return true;
}

static bool url_append_encoded(char *out, size_t out_len, size_t *pos, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!value) {
        return true;
    }
    while (*value) {
        unsigned char c = (unsigned char)*value++;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (safe) {
            if (!url_append_char(out, out_len, pos, (char)c)) {
                return false;
            }
        } else {
            if (*pos + 3 >= out_len) {
                return false;
            }
            out[(*pos)++] = '%';
            out[(*pos)++] = hex[(c >> 4) & 0x0F];
            out[(*pos)++] = hex[c & 0x0F];
            out[*pos] = '\0';
        }
    }
    return true;
}

static bool url_append_param(char *out, size_t out_len, size_t *pos,
                             bool *first, const char *key, const char *value)
{
    if (!value || !value[0]) {
        return true;
    }
    if (!url_append_char(out, out_len, pos, *first ? '?' : '&')) {
        return false;
    }
    *first = false;
    return url_append_str(out, out_len, pos, key) &&
           url_append_char(out, out_len, pos, '=') &&
           url_append_encoded(out, out_len, pos, value);
}

static esp_err_t http_post_json(const char *url, const char *body,
                                int timeout_ms,
                                char *resp, size_t resp_len, int *status_out)
{
    if (!url || !url[0] || !body || !resp || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    resp[0] = '\0';
    if (status_out) {
        *status_out = 0;
    }

    response_buf_t rb = {.buf = resp, .len = 0, .cap = (int)resp_len, .overflow = false};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .event_handler = chat_http_event,
        .user_data = &rb,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (status_out) {
        *status_out = esp_http_client_get_status_code(client);
    }
    if (rb.overflow && err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t make_voice_chunk_url(char *out, size_t out_len,
                                      const char *upload_id,
                                      size_t offset,
                                      size_t size)
{
    if (!out || out_len == 0 || !upload_id || !upload_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char offset_text[24];
    char size_text[16];
    snprintf(offset_text, sizeof(offset_text), "%u", (unsigned)offset);
    snprintf(size_text, sizeof(size_text), "%u", (unsigned)size);
    size_t pos = 0;
    bool first = true;
    out[0] = '\0';
    if (!url_append_str(out, out_len, &pos, s_voice_chunk_url) ||
        !url_append_param(out, out_len, &pos, &first, "upload_id", upload_id) ||
        !url_append_param(out, out_len, &pos, &first, "offset", offset_text) ||
        !url_append_param(out, out_len, &pos, &first, "size", size_text)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t upload_voice_chunk_once(const char *upload_id,
                                         size_t offset,
                                         const uint8_t *data,
                                         size_t size,
                                         int *received_out)
{
    if (received_out) {
        *received_out = -1;
    }
    char url[384];
    esp_err_t err = make_voice_chunk_url(url, sizeof(url), upload_id, offset, size);
    if (err != ESP_OK) {
        return err;
    }

    char resp[256];
    response_buf_t rb = {.buf = resp, .len = 0, .cap = sizeof(resp), .overflow = false};
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_VOICE_UPLOAD_CHUNK_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    char content_length_header[16];
    snprintf(content_length_header, sizeof(content_length_header), "%u", (unsigned)size);
    esp_http_client_set_header(client, "Content-Length", content_length_header);

    err = esp_http_client_open(client, (int)size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: chunk open failed offset=%u size=%u err=%s",
                 (unsigned)offset, (unsigned)size, esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, (const char *)data, (int)size);
    if (written != (int)size) {
        ESP_LOGE(TAG, "voice: chunk write failed offset=%u wrote=%d expected=%u",
                 (unsigned)offset, written, (unsigned)size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    while (true) {
        int r = esp_http_client_read(client, resp + rb.len, rb.cap - rb.len - 1);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        rb.len += r;
        resp[rb.len] = '\0';
        if (rb.len >= rb.cap - 1) {
            rb.overflow = true;
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (rb.overflow) {
        return ESP_ERR_NO_MEM;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "voice: chunk HTTP status=%d body=%s", status, resp);
        return ESP_FAIL;
    }
    if (received_out) {
        *received_out = json_int_value(resp, "received", -1);
    }
    return ESP_OK;
}

static void cancel_voice_upload(const char *upload_id)
{
    if (!upload_id || !upload_id[0]) {
        return;
    }
    char upload_json[128];
    char escaped[96];
    json_escape(upload_id, escaped, sizeof(escaped));
    int n = snprintf(upload_json, sizeof(upload_json), "{\"upload_id\":\"%s\"}", escaped);
    if (n < 0 || n >= (int)sizeof(upload_json)) {
        return;
    }
    char resp[128];
    int status = 0;
    (void)http_post_json(s_voice_cancel_url, upload_json, CHAT_TIMEOUT_MS, resp, sizeof(resp), &status);
}

static esp_err_t perform_voice_request_once(const char *wav_path, robot_chat_response_t *out,
                                            voice_request_timing_t *timing)
{
    if (!out || !wav_path || !wav_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timing) {
        memset(timing, 0, sizeof(*timing));
    }
    if (!s_voice_start_url[0] || !s_voice_chunk_url[0] ||
        !s_voice_finish_url[0] || !s_voice_cancel_url[0]) {
        strlcpy(out->error, "voice upload URL missing", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st = {0};
    if (stat(wav_path, &st) != 0 || st.st_size <= 44) {
        snprintf(out->error, sizeof(out->error), "voice wav unavailable");
        ESP_LOGE(TAG, "voice upload: stat failed path=%s errno=%d (%s)", wav_path, errno, strerror(errno));
        return ESP_FAIL;
    }

    char conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
    char profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
    char active_mode[ROBOT_CHAT_ACTIVE_MODE_MAX];
    chat_state_snapshot(conversation_id, sizeof(conversation_id),
                        profile_id, sizeof(profile_id),
                        active_mode, sizeof(active_mode));

    ESP_LOGI(TAG, "conversation_id sent = %s", conversation_id[0] ? conversation_id : "(none)");
    ESP_LOGI(TAG, "profile_id = %s", profile_id[0] ? profile_id : "(none)");
    ESP_LOGI(TAG, "active_mode = %s", active_mode[0] ? active_mode : "kids_chat");

    if (st.st_size > INT32_MAX) {
        strlcpy(out->error, "voice wav too large", sizeof(out->error));
        return ESP_ERR_INVALID_SIZE;
    }

    char start_body[640];
    char escaped_conv[ROBOT_CHAT_CONVERSATION_ID_MAX + 8] = {0};
    char escaped_profile[ROBOT_CHAT_PROFILE_ID_MAX + 8] = {0};
    char conv_part[128] = {0};
    char profile_part[80] = {0};
    if (conversation_id[0]) {
        json_escape(conversation_id, escaped_conv, sizeof(escaped_conv));
        snprintf(conv_part, sizeof(conv_part), "\"conversation_id\":\"%s\",", escaped_conv);
    }
    if (profile_id[0]) {
        json_escape(profile_id, escaped_profile, sizeof(escaped_profile));
        snprintf(profile_part, sizeof(profile_part), "\"profile_id\":\"%s\",", escaped_profile);
    }
    int body_len = snprintf(start_body, sizeof(start_body),
        "{"
        "\"device_id\":\"%s\","
        "\"locale\":\"%s\","
        "\"mode\":\"%s\","
        "%s%s"
        "\"tts\":true,"
        "\"file_size\":%ld"
        "}",
        CONFIG_ROBOT_CHAT_DEVICE_ID,
        CONFIG_ROBOT_CHAT_LOCALE,
        active_mode[0] ? active_mode : "kids_chat",
        conv_part,
        profile_part,
        (long)st.st_size);
    if (body_len < 0 || body_len >= (int)sizeof(start_body)) {
        strlcpy(out->error, "voice upload start body too large", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    char start_resp[256];
    int start_status = 0;
    ESP_LOGI(TAG, "voice: upload start file_size=%ld", (long)st.st_size);
    int64_t upload_start_ms = chat_now_ms();
    esp_err_t err = http_post_json(s_voice_start_url, start_body, CHAT_TIMEOUT_MS,
                                   start_resp, sizeof(start_resp), &start_status);
    if (err != ESP_OK || start_status < 200 || start_status >= 300) {
        ESP_LOGE(TAG, "voice: upload start failed status=%d err=%s body=%s",
                 start_status, esp_err_to_name(err), start_resp);
        strlcpy(out->error, "Voice upload start failed", sizeof(out->error));
        return err == ESP_OK ? ESP_FAIL : err;
    }

    char upload_id[96];
    json_string_value(start_resp, "upload_id", upload_id, sizeof(upload_id));
    if (!upload_id[0]) {
        ESP_LOGE(TAG, "voice: upload start missing upload_id body=%s", start_resp);
        strlcpy(out->error, "Voice upload start missing upload_id", sizeof(out->error));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "voice: upload_id=%s", upload_id);

    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        int saved_errno = errno;
        ESP_LOGE(TAG, "voice upload: failed to open %s errno=%d (%s)", wav_path, saved_errno, strerror(saved_errno));
        cancel_voice_upload(upload_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "voice: upload chunk_size=%u timeout_ms=%u max_attempts=%u",
             (unsigned)CHAT_VOICE_UPLOAD_CHUNK_SIZE,
             (unsigned)CHAT_VOICE_UPLOAD_CHUNK_TIMEOUT_MS,
             (unsigned)CHAT_VOICE_UPLOAD_CHUNK_MAX_ATTEMPTS);
    uint8_t *chunk = heap_caps_malloc(CHAT_VOICE_UPLOAD_CHUNK_SIZE, MALLOC_CAP_8BIT);
    if (!chunk) {
        strlcpy(out->error, "voice upload buffer allocation failed", sizeof(out->error));
        fclose(f);
        cancel_voice_upload(upload_id);
        return ESP_ERR_NO_MEM;
    }

    size_t uploaded = 0;
    while (uploaded < (size_t)st.st_size) {
        size_t wanted = (size_t)st.st_size - uploaded;
        if (wanted > CHAT_VOICE_UPLOAD_CHUNK_SIZE) {
            wanted = CHAT_VOICE_UPLOAD_CHUNK_SIZE;
        }
        size_t read_len = fread(chunk, 1, wanted, f);
        if (read_len == 0) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "voice upload: file read failed after %u/%u bytes",
                         (unsigned)uploaded, (unsigned)st.st_size);
            } else {
                ESP_LOGE(TAG, "voice upload: unexpected EOF after %u/%u bytes",
                         (unsigned)uploaded, (unsigned)st.st_size);
            }
            err = ESP_FAIL;
            break;
        }

        ESP_LOGI(TAG, "voice: uploading chunk offset=%u size=%u",
                 (unsigned)uploaded, (unsigned)read_len);
        int received = -1;
        for (int attempt = 1; attempt <= CHAT_VOICE_UPLOAD_CHUNK_MAX_ATTEMPTS; attempt++) {
            err = upload_voice_chunk_once(upload_id, uploaded, chunk, read_len, &received);
            if (err == ESP_OK) {
                if (attempt > 1) {
                    ESP_LOGI(TAG, "voice: chunk retry succeeded offset=%u attempt=%d",
                             (unsigned)uploaded, attempt);
                }
                break;
            }
            ESP_LOGW(TAG, "voice: chunk attempt failed offset=%u attempt=%d/%u err=%s",
                     (unsigned)uploaded,
                     attempt,
                     (unsigned)CHAT_VOICE_UPLOAD_CHUNK_MAX_ATTEMPTS,
                     esp_err_to_name(err));
            if (attempt < CHAT_VOICE_UPLOAD_CHUNK_MAX_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "voice: chunk upload failed offset=%u size=%u err=%s",
                     (unsigned)uploaded, (unsigned)read_len, esp_err_to_name(err));
            break;
        }

        uploaded += read_len;
        ESP_LOGI(TAG, "voice: chunk ok received=%d", received);
        if (received >= 0 && received < (int)uploaded) {
            ESP_LOGE(TAG, "voice: backend received less than expected received=%d uploaded=%u",
                     received, (unsigned)uploaded);
            err = ESP_FAIL;
            break;
        }
    }
    free(chunk);
    fclose(f);

    if (err != ESP_OK || uploaded != (size_t)st.st_size) {
        ESP_LOGE(TAG, "voice: upload failed uploaded=%u/%u err=%s",
                 (unsigned)uploaded, (unsigned)st.st_size, esp_err_to_name(err));
        cancel_voice_upload(upload_id);
        strlcpy(out->error, "Voice upload failed. Backend did not respond.", sizeof(out->error));
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (timing) {
        timing->upload_ms = (uint32_t)(chat_now_ms() - upload_start_ms);
    }

    char finish_body[128];
    char escaped_upload_id[104];
    json_escape(upload_id, escaped_upload_id, sizeof(escaped_upload_id));
    int finish_len = snprintf(finish_body, sizeof(finish_body), "{\"upload_id\":\"%s\"}", escaped_upload_id);
    if (finish_len < 0 || finish_len >= (int)sizeof(finish_body)) {
        cancel_voice_upload(upload_id);
        strlcpy(out->error, "voice upload finish body too large", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    char *resp = heap_caps_calloc(1, CHAT_RESP_MAX, MALLOC_CAP_8BIT);
    if (!resp) {
        cancel_voice_upload(upload_id);
        strlcpy(out->error, "voice finish response allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice: upload finish");
    robot_voice_mark_waiting_backend();
    int64_t backend_start_ms = chat_now_ms();
    err = http_post_json(s_voice_finish_url, finish_body, CHAT_VOICE_TIMEOUT_MS,
                         resp, CHAT_RESP_MAX, &out->http_status);
    if (timing) {
        timing->backend_ms = (uint32_t)(chat_now_ms() - backend_start_ms);
    }
    ESP_LOGI(TAG, "voice: backend response status=%d err=%s", out->http_status, esp_err_to_name(err));
    if (err != ESP_OK || out->http_status < 200 || out->http_status >= 300) {
        ESP_LOGE(TAG, "voice: upload finish failed status=%d body=%s", out->http_status, resp);
        strlcpy(out->error, "Voice upload finish failed", sizeof(out->error));
        free(resp);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    parse_chat_response_json(resp, out);
    log_chat_response_summary(out);
    free(resp);
    return ESP_OK;
}

esp_err_t robot_chat_init(void)
{
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    if (!s_state_lock) {
        s_state_lock = xSemaphoreCreateMutex();
        if (!s_state_lock) {
            ESP_LOGE(TAG, "failed to create state mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_worker_lock) {
        s_worker_lock = xSemaphoreCreateMutex();
        if (!s_worker_lock) {
            ESP_LOGE(TAG, "failed to create worker mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_worker_events) {
        s_worker_events = xEventGroupCreate();
        if (!s_worker_events) {
            ESP_LOGE(TAG, "failed to create worker event group");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_worker_queue) {
        s_worker_queue = xQueueCreate(CHAT_WORKER_QUEUE_LEN, sizeof(uint8_t));
        if (!s_worker_queue) {
            ESP_LOGE(TAG, "failed to create worker queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_worker_task) {
        BaseType_t ok = xTaskCreate(robot_chat_worker_task,
                                    "robot_chat_worker",
                                    CHAT_WORKER_STACK_BYTES,
                                    NULL,
                                    5,
                                    &s_worker_task);
        if (ok != pdPASS) {
            s_worker_task = NULL;
            ESP_LOGE(TAG, "failed to create robot_chat_worker task");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "robot_chat_worker started stack=%u priority=%u",
                 (unsigned)CHAT_WORKER_STACK_BYTES, 5U);
    }
    s_conversation_id[0] = '\0';
    s_profile_id[0] = '\0';
    strlcpy(s_active_mode, "kids_chat", sizeof(s_active_mode));
    char default_base_url[160];
    snprintf(default_base_url, sizeof(default_base_url), "http://%s:%d",
             CONFIG_ROBOT_CHAT_BACKEND_HOST,
             CONFIG_ROBOT_CHAT_BACKEND_PORT);
    return robot_chat_build_backend_urls(default_base_url);
#else
    s_backend_url[0] = '\0';
    s_backend_base_url[0] = '\0';
    s_voice_start_url[0] = '\0';
    s_voice_chunk_url[0] = '\0';
    s_voice_finish_url[0] = '\0';
    s_voice_cancel_url[0] = '\0';
    ESP_LOGI(TAG, "Kids Chat disabled");
    return ESP_OK;
#endif
}

bool robot_chat_enabled(void)
{
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    return true;
#else
    return false;
#endif
}

const char *robot_chat_backend_url(void)
{
    return s_backend_url;
}

esp_err_t robot_chat_set_backend_base_url(const char *backend_base_url)
{
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    return robot_chat_build_backend_urls(backend_base_url);
#else
    (void)backend_base_url;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t robot_chat_execute_text(const char *message, robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "kids chat disabled", sizeof(out->error));
    strlcpy(out->reply_text, "A beszelgetos mod most ki van kapcsolva.",
            sizeof(out->reply_text));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!message || !message[0]) {
        strlcpy(out->error, "missing message", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }

    const char *blocked_reason = NULL;
    if (!kids_safety_message_allowed(message, &blocked_reason)) {
        out->ok = true;
        out->safe = false;
        strlcpy(out->blocked_reason,
                blocked_reason ? blocked_reason : "local_safety",
                sizeof(out->blocked_reason));
        strlcpy(out->reply_text,
                "Erről most nem beszélgetek, de szívesen mondok egy mesét vagy játszhatunk barkóbát.",
                sizeof(out->reply_text));
        chat_state_snapshot(out->conversation_id, sizeof(out->conversation_id),
                            out->profile_id, sizeof(out->profile_id),
                            out->active_mode, sizeof(out->active_mode));
        robot_set_speaking(out->reply_text);
        vTaskDelay(pdMS_TO_TICKS(1200));
        robot_set_idle();
        ESP_LOGI(TAG, "blocked locally: %s", out->blocked_reason);
        return ESP_OK;
    }

    char conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
    char profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
    char active_mode[ROBOT_CHAT_ACTIVE_MODE_MAX];
    chat_state_snapshot(conversation_id, sizeof(conversation_id),
                        profile_id, sizeof(profile_id),
                        active_mode, sizeof(active_mode));

    ESP_LOGI(TAG, "conversation_id sent = %s", conversation_id[0] ? conversation_id : "(none)");
    ESP_LOGI(TAG, "profile_id = %s", profile_id[0] ? profile_id : "(none)");
    ESP_LOGI(TAG, "active_mode = %s", active_mode[0] ? active_mode : "kids_chat");

    char *body = heap_caps_calloc(1, CHAT_REQ_MAX, MALLOC_CAP_8BIT);
    if (!body) {
        strlcpy(out->error, "request allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = make_request_body(message, conversation_id, profile_id, body, CHAT_REQ_MAX);
    if (err != ESP_OK) {
        strlcpy(out->error, "request too large", sizeof(out->error));
        free(body);
        return err;
    }

    robot_set_thinking();
    ESP_LOGI(TAG, "sending request to %s", s_backend_url);
    err = perform_request_once(body, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed once: %s, retrying", esp_err_to_name(err));
        err = perform_request_once(body, out);
    }

    if (err != ESP_OK) {
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
        face_status_set_last_error("kids chat backend unavailable");
        robot_set_speaking(out->reply_text);
        vTaskDelay(pdMS_TO_TICKS(1500));
        robot_set_idle();
        free(body);
        return err;
    }
    free(body);

    face_status_set_last_error(NULL);
    if (!out->reply_text[0]) {
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
    }

    if (robot_chat_play_downloaded_audio(out, NULL)) {
        return ESP_OK;
    }

    robot_set_speaking(out->reply_text);
    vTaskDelay(pdMS_TO_TICKS(1800));
    robot_set_idle();

    return ESP_OK;
#endif
}

static esp_err_t robot_chat_execute_voice_file(const char *path, robot_chat_response_t *out,
                                               voice_request_timing_t *timing,
                                               uint32_t *audio_download_ms_out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "kids chat disabled", sizeof(out->error));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!path || !path[0]) {
        strlcpy(out->error, "missing voice file", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }

    robot_set_thinking();
    esp_err_t err = perform_voice_request_once(path, out, timing);

    if (err != ESP_OK) {
        strlcpy(out->reply_text, "Most nem hallottalak jól. Megpróbálod még egyszer?",
                sizeof(out->reply_text));
        face_status_set_last_error("kids voice backend unavailable");
        robot_set_speaking(out->reply_text);
        vTaskDelay(pdMS_TO_TICKS(1500));
        robot_set_idle();
        return err;
    }

    face_status_set_last_error(NULL);
    if (!out->reply_text[0]) {
        strlcpy(out->reply_text, "Most nem hallottalak jól. Megpróbálod még egyszer?",
                sizeof(out->reply_text));
    }

    if (robot_chat_play_downloaded_audio(out, audio_download_ms_out)) {
        return ESP_OK;
    }

    robot_set_speaking(out->reply_text);
    vTaskDelay(pdMS_TO_TICKS(1800));
    robot_set_idle();
    return ESP_OK;
#endif
}

static void publish_voice_timing(const robot_voice_timing_t *timing)
{
    if (!timing) {
        return;
    }
    robot_voice_set_timing(timing);
    ESP_LOGI(TAG,
             "voice_timing record_ms=%u upload_ms=%u backend_ms=%u audio_download_ms=%u total_ms=%u wav_bytes=%u sample_rate=%u",
             (unsigned)timing->record_ms,
             (unsigned)timing->upload_ms,
             (unsigned)timing->backend_ms,
             (unsigned)timing->audio_download_ms,
             (unsigned)timing->total_ms,
             (unsigned)timing->wav_bytes,
             (unsigned)timing->sample_rate);
}

static esp_err_t robot_chat_execute_voice_record(robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "kids chat disabled", sizeof(out->error));
    robot_voice_mark_error_idle(out->error);
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_LOGI(TAG, "robot_chat_worker: processing voice job");
    int64_t total_start_ms = chat_now_ms();
    robot_voice_timing_t voice_timing = {
        .sample_rate = CHAT_VOICE_UPLOAD_SAMPLE_RATE,
    };
    char voice_error[96] = {0};
    size_t wav_size = 0;
    int64_t record_start_ms = chat_now_ms();
    esp_err_t err = robot_voice_record_wav_file(&wav_size, voice_error, sizeof(voice_error));
    voice_timing.record_ms = (uint32_t)(chat_now_ms() - record_start_ms);
    voice_timing.wav_bytes = wav_size;
    if (err != ESP_OK) {
        strlcpy(out->error, voice_error[0] ? voice_error : "voice recording failed", sizeof(out->error));
        voice_timing.total_ms = (uint32_t)(chat_now_ms() - total_start_ms);
        publish_voice_timing(&voice_timing);
        return err;
    }

    robot_voice_mark_uploading();
    ESP_LOGI(TAG, "voice: uploading to backend");
    voice_request_timing_t request_timing = {0};
    uint32_t audio_download_ms = 0;
    err = robot_chat_execute_voice_file(ROBOT_VOICE_WAV_PATH, out,
                                        &request_timing,
                                        &audio_download_ms);
    voice_timing.upload_ms = request_timing.upload_ms;
    voice_timing.backend_ms = request_timing.backend_ms;
    voice_timing.audio_download_ms = audio_download_ms;
    ESP_LOGI(TAG, "voice: upload status=%d", out->http_status);
    if (out->transcript[0]) {
        ESP_LOGI(TAG, "voice: transcript=%s", out->transcript);
    }
    ESP_LOGI(TAG, "voice: reply_audio_url=%s", out->reply_audio_url[0] ? out->reply_audio_url : "(missing)");

    if (err != ESP_OK) {
        const char *detail = out->error[0] ? out->error : "";
        ESP_LOGE(TAG, "voice chat failed: %s detail=%s", esp_err_to_name(err), detail);
        char msg[192];
        snprintf(msg, sizeof(msg), "voice chat failed: %s %.120s", esp_err_to_name(err), detail);
        voice_timing.total_ms = (uint32_t)(chat_now_ms() - total_start_ms);
        publish_voice_timing(&voice_timing);
        robot_voice_mark_error_idle(msg);
        return err;
    }

    if (robot_is_speaking() || Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING) {
        robot_voice_mark_playing_reply();
        int waited_ms = 0;
        while ((robot_is_speaking() || Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING) && waited_ms < 120000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited_ms += 200;
        }
    }
    voice_timing.total_ms = (uint32_t)(chat_now_ms() - total_start_ms);
    publish_voice_timing(&voice_timing);
    robot_voice_mark_idle();
    ESP_LOGI(TAG, "voice: state -> idle");
    return ESP_OK;
#endif
}

static void robot_chat_worker_task(void *arg)
{
    (void)arg;
    uint8_t token = 0;

    for (;;) {
        if (xQueueReceive(s_worker_queue, &token, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "robot_chat_worker: processing request");
        ESP_LOGI(TAG, "heap before request: free=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        char *message = heap_caps_calloc(1, ROBOT_CHAT_MESSAGE_MAX, MALLOC_CAP_8BIT);
        char *path = heap_caps_calloc(1, CHAT_VOICE_PATH_MAX, MALLOC_CAP_8BIT);
        robot_chat_job_kind_t kind = ROBOT_CHAT_JOB_TEXT;
        robot_chat_response_t resp = {0};
        esp_err_t err = ESP_ERR_NO_MEM;

        if (message && path) {
            xSemaphoreTake(s_worker_lock, portMAX_DELAY);
            kind = s_job.kind;
            strlcpy(message, s_job.message, ROBOT_CHAT_MESSAGE_MAX);
            strlcpy(path, s_job.path, CHAT_VOICE_PATH_MAX);
            xSemaphoreGive(s_worker_lock);
            if (kind == ROBOT_CHAT_JOB_VOICE_RECORD) {
                err = robot_chat_execute_voice_record(&resp);
            } else if (kind == ROBOT_CHAT_JOB_VOICE_FILE) {
                err = robot_chat_execute_voice_file(path, &resp, NULL, NULL);
            } else {
                err = robot_chat_execute_text(message, &resp);
            }
        } else {
            xSemaphoreTake(s_worker_lock, portMAX_DELAY);
            kind = s_job.kind;
            xSemaphoreGive(s_worker_lock);
            strlcpy(resp.error, "worker allocation failed", sizeof(resp.error));
            if (kind == ROBOT_CHAT_JOB_VOICE_RECORD) {
                robot_voice_mark_error_idle(resp.error);
            }
        }
        free(message);
        free(path);

        ESP_LOGI(TAG, "heap after request: free=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        if (s_job.waiting) {
            s_job.response = resp;
            s_job.err = err;
            xEventGroupSetBits(s_worker_events, CHAT_EVENT_DONE);
        } else {
            ESP_LOGI(TAG, "robot_chat_worker: async request finished err=%s", esp_err_to_name(err));
            s_job.busy = false;
        }
        xSemaphoreGive(s_worker_lock);
    }
}

static esp_err_t robot_chat_queue_request(robot_chat_job_kind_t kind,
                                          const char *message,
                                          const char *path,
                                          robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "kids chat disabled", sizeof(out->error));
    strlcpy(out->reply_text, "A beszelgetos mod most ki van kapcsolva.",
            sizeof(out->reply_text));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (kind == ROBOT_CHAT_JOB_TEXT && (!message || !message[0])) {
        strlcpy(out->error, "missing message", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }
    if (kind == ROBOT_CHAT_JOB_VOICE_FILE && (!path || !path[0])) {
        strlcpy(out->error, "missing voice file", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_worker_lock || !s_worker_queue || !s_worker_events || !s_worker_task) {
        strlcpy(out->error, "robot_chat worker not initialized", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);
    if (s_job.busy) {
        robot_chat_job_kind_t busy_kind = s_job.kind;
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "robot_chat busy", sizeof(out->error));
        if (busy_kind == ROBOT_CHAT_JOB_VOICE_RECORD || busy_kind == ROBOT_CHAT_JOB_VOICE_FILE) {
            strlcpy(out->reply_text, "A robot most a hangfelvetelt dolgozza fel. Varj par masodpercet.",
                    sizeof(out->reply_text));
        } else {
            strlcpy(out->reply_text, "Most meg egy valaszon dolgozom, kerlek varj egy kicsit.",
                    sizeof(out->reply_text));
        }
        ESP_LOGW(TAG, "robot_chat request rejected: worker busy");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_job, 0, sizeof(s_job));
    s_job.kind = kind;
    if (message) {
        strlcpy(s_job.message, message, sizeof(s_job.message));
    }
    if (path) {
        strlcpy(s_job.path, path, sizeof(s_job.path));
    }
    s_job.busy = true;
    s_job.waiting = true;
    xEventGroupClearBits(s_worker_events, CHAT_EVENT_DONE);
    xSemaphoreGive(s_worker_lock);

    uint8_t token = 1;
    if (xQueueSend(s_worker_queue, &token, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.busy = false;
        s_job.waiting = false;
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "robot_chat queue full", sizeof(out->error));
        ESP_LOGE(TAG, "failed to queue robot_chat request");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "queued %s request",
             kind == ROBOT_CHAT_JOB_VOICE_RECORD ? "voice record" :
             (kind == ROBOT_CHAT_JOB_VOICE_FILE ? "voice chat" : "chat"));
    EventBits_t bits = xEventGroupWaitBits(s_worker_events,
                                           CHAT_EVENT_DONE,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CHAT_WAIT_TIMEOUT_MS));
    if ((bits & CHAT_EVENT_DONE) == 0) {
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.waiting = false;
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "robot_chat timeout", sizeof(out->error));
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
        robot_set_idle();
        ESP_LOGE(TAG, "robot_chat request timed out after %u ms",
                 (unsigned)CHAT_WAIT_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);
    *out = s_job.response;
    esp_err_t err = s_job.err;
    s_job.busy = false;
    s_job.waiting = false;
    xSemaphoreGive(s_worker_lock);
    return err;
#endif
}

esp_err_t robot_chat_send_text(const char *message, robot_chat_response_t *out)
{
    return robot_chat_queue_request(ROBOT_CHAT_JOB_TEXT, message, NULL, out);
}

esp_err_t robot_chat_send_voice_file(const char *wav_path, robot_chat_response_t *out)
{
    return robot_chat_queue_request(ROBOT_CHAT_JOB_VOICE_FILE, NULL, wav_path, out);
}

bool robot_chat_worker_available(void)
{
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    return s_worker_lock && s_worker_queue && s_worker_events && s_worker_task;
#else
    return false;
#endif
}

esp_err_t robot_chat_start_voice_record(char *error, size_t error_len)
{
#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    if (error && error_len) {
        strlcpy(error, "kids chat disabled", error_len);
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!robot_chat_worker_available()) {
        if (error && error_len) {
            strlcpy(error, "Voice worker not started", error_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);
    if (s_job.busy) {
        xSemaphoreGive(s_worker_lock);
        if (error && error_len) {
            strlcpy(error, "robot_chat worker busy", error_len);
        }
        ESP_LOGW(TAG, "voice record request rejected: worker busy");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_job, 0, sizeof(s_job));
    s_job.kind = ROBOT_CHAT_JOB_VOICE_RECORD;
    s_job.busy = true;
    s_job.waiting = false;
    xEventGroupClearBits(s_worker_events, CHAT_EVENT_DONE);
    xSemaphoreGive(s_worker_lock);

    uint8_t token = 1;
    if (xQueueSend(s_worker_queue, &token, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.busy = false;
        s_job.waiting = false;
        xSemaphoreGive(s_worker_lock);
        if (error && error_len) {
            strlcpy(error, "Voice worker queue unavailable", error_len);
        }
        ESP_LOGE(TAG, "failed to queue voice record request");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "voice: queued voice job to robot_chat_worker");
    if (error && error_len) {
        strlcpy(error, "Recording started", error_len);
    }
    return ESP_OK;
#endif
}
