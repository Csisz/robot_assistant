#include "robot_chat.h"

#include "audio_driver.h"
#include "robot_state.h"
#include "web_server.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "robot_chat";

#ifndef CONFIG_ROBOT_VOICE_BACKEND_PATH
#define CONFIG_ROBOT_VOICE_BACKEND_PATH "/api/robot/voice-chat"
#endif

#define CHAT_TIMEOUT_MS 60000
#define CHAT_AUDIO_TIMEOUT_MS 60000
#define CHAT_REQ_MAX 1400
#define CHAT_RESP_MAX 4096
#define CHAT_WORKER_STACK_BYTES (18 * 1024)
#define CHAT_WORKER_QUEUE_LEN 1
#define CHAT_WAIT_TIMEOUT_MS 90000
#define CHAT_EVENT_DONE BIT0
#define CHAT_AUDIO_CHUNK 512
#define CHAT_AUDIO_MAX_BYTES (1024 * 1024)
#define CHAT_AUDIO_INITIAL_CAPACITY (64 * 1024)
#define ROBOT_REPLY_AUDIO_PATH "/sdcard/REPLY.MP3"
#define ROBOT_REPLY_AUDIO_TMP_PATH "/sdcard/REPLY.TMP"
#define ROBOT_REPLY_AUDIO_URL "file://sdcard/REPLY.MP3"

/* Multipart voice upload */
#define VOICE_BOUNDARY      "----RobotVoiceBoundary0x1A2B"
#define VOICE_TRAILER       "\r\n------RobotVoiceBoundary0x1A2B--\r\n"
#define VOICE_PREAMBLE_MAX  900
#define VOICE_RESP_MAX      4096
#define VOICE_STREAM_CHUNK  4096
#define VOICE_TIMEOUT_MS    90000

static char s_backend_url[192];
static char s_backend_base_url[160];
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_worker_lock;
static QueueHandle_t s_worker_queue;
static EventGroupHandle_t s_worker_events;
static TaskHandle_t s_worker_task;
static char s_conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
static char s_profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
static char s_active_mode[ROBOT_CHAT_ACTIVE_MODE_MAX] = "companion";

typedef struct {
    bool busy;
    char message[ROBOT_CHAT_MESSAGE_MAX];
    robot_chat_response_t response;
    esp_err_t err;
} robot_chat_job_t;

typedef struct {
    char *buf;
    int len;
    int cap;
    bool overflow;
} response_buf_t;

static robot_chat_job_t s_job;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

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
    ESP_LOGI(TAG, "backend URL = %s", s_backend_url);
    return ESP_OK;
}

static const char *fallback_text(void)
{
    return "Most nem sikerult valaszolnom. Probald meg ujra par masodperc mulva.";
}

static void chat_state_snapshot(char *conversation_id, size_t conversation_len,
                                char *profile_id, size_t profile_len)
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
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len > 0) {
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
    while (*src && pos + 1 < dst_len) {
        char c = *src++;
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
    if (strncmp(p, "null", 4) == 0 || *p != '"') {
        return;
    }
    p++;
    size_t pos = 0;
    while (*p && *p != '"' && pos + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            out[pos++] = (*p == 'n') ? ' ' : *p;
            p++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}

static void parse_chat_response_json(const char *resp, robot_chat_response_t *out)
{
    out->ok = json_bool_value(resp, "ok", false);
    out->safe = json_bool_value(resp, "safe", true);
    json_string_value(resp, "conversation_id", out->conversation_id, sizeof(out->conversation_id));
    json_string_value(resp, "profile_id", out->profile_id, sizeof(out->profile_id));
    json_string_value(resp, "transcript", out->transcript, sizeof(out->transcript));
    json_string_value(resp, "reply_text", out->reply_text, sizeof(out->reply_text));
    json_string_value(resp, "reply_audio_url", out->reply_audio_url, sizeof(out->reply_audio_url));
    json_string_value(resp, "robot_mood", out->robot_mood, sizeof(out->robot_mood));
    json_string_value(resp, "robot_action", out->robot_action, sizeof(out->robot_action));
    json_string_value(resp, "blocked_reason", out->blocked_reason, sizeof(out->blocked_reason));
    json_string_value(resp, "active_mode", out->active_mode, sizeof(out->active_mode));
    chat_state_update(out);
}

static esp_err_t make_request_body(const char *message,
                                   const char *conversation_id,
                                   const char *profile_id,
                                   char *body,
                                   size_t body_len)
{
    char escaped[720];
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
                     "{%s%s\"device_id\":\"%s\",\"locale\":\"%s\","
                     "\"mode\":\"companion\",\"message\":\"%s\","
                     "\"max_answer_seconds\":30,\"tts\":true}",
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
    if (!body || !out || !s_backend_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    char *resp = heap_caps_calloc(1, CHAT_RESP_MAX, MALLOC_CAP_8BIT);
    if (!resp) {
        strlcpy(out->error, "response allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    response_buf_t rb = {.buf = resp, .len = 0, .cap = CHAT_RESP_MAX, .overflow = false};
    esp_http_client_config_t cfg = {
        .url = s_backend_url,
        .timeout_ms = CHAT_TIMEOUT_MS,
        .event_handler = chat_http_event,
        .user_data = &rb,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp);
        strlcpy(out->error, "http client init failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    out->http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "backend response status=%d err=%s", out->http_status, esp_err_to_name(err));

    if (rb.overflow) {
        strlcpy(out->error, "backend response too large", sizeof(out->error));
        free(resp);
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK || out->http_status < 200 || out->http_status >= 300) {
        snprintf(out->error, sizeof(out->error), "backend unavailable status=%d", out->http_status);
        free(resp);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    parse_chat_response_json(resp, out);
    ESP_LOGI(TAG, "conversation_id=%s active_mode=%s reply_audio_url=%s",
             out->conversation_id[0] ? out->conversation_id : "(missing)",
             out->active_mode[0] ? out->active_mode : "(missing)",
             out->reply_audio_url[0] ? out->reply_audio_url : "(missing)");
    free(resp);
    return ESP_OK;
}

static const char *absolute_audio_url(const char *reply_audio_url, char *buf, size_t buf_len)
{
    if (!reply_audio_url || !reply_audio_url[0]) {
        return "";
    }
    if (strncmp(reply_audio_url, "http://", 7) == 0 ||
        strncmp(reply_audio_url, "https://", 8) == 0) {
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

static esp_err_t ensure_audio_capacity(uint8_t **buf, size_t *capacity, size_t used_len, size_t needed)
{
    if (needed <= *capacity) {
        return ESP_OK;
    }
    if (needed > CHAT_AUDIO_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t new_capacity = *capacity ? *capacity : CHAT_AUDIO_INITIAL_CAPACITY;
    while (new_capacity < needed) {
        new_capacity = new_capacity > CHAT_AUDIO_MAX_BYTES / 2 ? CHAT_AUDIO_MAX_BYTES : new_capacity * 2;
    }
    uint8_t *new_buf = heap_caps_malloc(new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_buf) {
        return ESP_ERR_NO_MEM;
    }
    if (*buf && used_len > 0) {
        memcpy(new_buf, *buf, used_len);
    }
    free(*buf);
    *buf = new_buf;
    *capacity = new_capacity;
    return ESP_OK;
}

static esp_err_t write_audio_psram_to_sd(const uint8_t *audio, size_t audio_len)
{
    if (!audio || audio_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *scratch = heap_caps_malloc(CHAT_AUDIO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!scratch) {
        return ESP_ERR_NO_MEM;
    }

    unlink(ROBOT_REPLY_AUDIO_TMP_PATH);
    FILE *f = fopen(ROBOT_REPLY_AUDIO_TMP_PATH, "wb");
    if (!f) {
        free(scratch);
        ESP_LOGE(TAG, "open %s failed errno=%d", ROBOT_REPLY_AUDIO_TMP_PATH, errno);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    size_t offset = 0;
    while (offset < audio_len) {
        size_t chunk = audio_len - offset;
        if (chunk > CHAT_AUDIO_CHUNK) {
            chunk = CHAT_AUDIO_CHUNK;
        }
        memcpy(scratch, audio + offset, chunk);
        if (fwrite(scratch, 1, chunk, f) != chunk) {
            err = ESP_FAIL;
            break;
        }
        offset += chunk;
    }
    if (err == ESP_OK && fflush(f) != 0) {
        err = ESP_FAIL;
    }
    if (fclose(f) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(scratch);

    if (err != ESP_OK) {
        unlink(ROBOT_REPLY_AUDIO_TMP_PATH);
        return err;
    }
    if (rename(ROBOT_REPLY_AUDIO_TMP_PATH, ROBOT_REPLY_AUDIO_PATH) != 0) {
        if (unlink(ROBOT_REPLY_AUDIO_PATH) != 0 && errno != ENOENT) {
            unlink(ROBOT_REPLY_AUDIO_TMP_PATH);
            return ESP_FAIL;
        }
        if (rename(ROBOT_REPLY_AUDIO_TMP_PATH, ROBOT_REPLY_AUDIO_PATH) != 0) {
            unlink(ROBOT_REPLY_AUDIO_TMP_PATH);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t download_audio_to_sd(const char *url, size_t *bytes_out)
{
    if (bytes_out) {
        *bytes_out = 0;
    }
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "audio download URL=%s", url);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CHAT_AUDIO_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "audio/mpeg, audio/mp3, */*");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length > CHAT_AUDIO_MAX_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t capacity = content_length > 0 ? (size_t)content_length : CHAT_AUDIO_INITIAL_CAPACITY;
    if (capacity == 0 || capacity > CHAT_AUDIO_MAX_BYTES) {
        capacity = CHAT_AUDIO_INITIAL_CAPACITY;
    }
    uint8_t *audio = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    char temp[1024];
    while (true) {
        int n = esp_http_client_read(client, temp, sizeof(temp));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        err = ensure_audio_capacity(&audio, &capacity, total, total + (size_t)n);
        if (err != ESP_OK) {
            break;
        }
        memcpy(audio + total, temp, (size_t)n);
        total += (size_t)n;
        if (total > CHAT_AUDIO_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && content_length > 0 && total != (size_t)content_length) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && total == 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = write_audio_psram_to_sd(audio, total);
    }
    free(audio);
    if (err == ESP_OK && bytes_out) {
        *bytes_out = total;
    }
    return err;
}

static bool play_downloaded_audio(const robot_chat_response_t *out)
{
    if (!out || !out->reply_audio_url[0]) {
        ESP_LOGW(TAG, "reply_audio_url missing; text-only fallback");
        return false;
    }

    if (robot_is_speaking() || Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING) {
        Audio_Stop_Play();
        robot_set_idle();
        vTaskDelay(pdMS_TO_TICKS(100));
        robot_set_thinking();
    }

    char url_buf[ROBOT_CHAT_AUDIO_URL_MAX + 160];
    const char *url = absolute_audio_url(out->reply_audio_url, url_buf, sizeof(url_buf));
    int64_t start_ms = now_ms();
    size_t bytes = 0;
    esp_err_t err = download_audio_to_sd(url, &bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio download failed: %s; text-only fallback", esp_err_to_name(err));
        web_server_set_last_error("audio download failed");
        return false;
    }
    struct stat st = {0};
    if (stat(ROBOT_REPLY_AUDIO_PATH, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "downloaded audio missing or empty");
        web_server_set_last_error("downloaded audio missing");
        return false;
    }

    web_server_set_last_audio(ROBOT_REPLY_AUDIO_PATH);
    web_server_set_last_error(NULL);
    ESP_LOGI(TAG, "audio ready path=%s bytes=%u download_ms=%u",
             ROBOT_REPLY_AUDIO_PATH,
             (unsigned)bytes,
             (unsigned)(now_ms() - start_ms));
    esp_gmf_err_t play_err = robot_say_file(out->reply_text, ROBOT_REPLY_AUDIO_URL);
    if (play_err != ESP_GMF_ERR_OK) {
        web_server_set_last_error("audio playback failed");
        return false;
    }
    return true;
}

static esp_err_t robot_chat_execute_text(const char *message, robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "chat disabled", sizeof(out->error));
    strlcpy(out->reply_text, "A beszelgetes most ki van kapcsolva.", sizeof(out->reply_text));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!message || !message[0]) {
        strlcpy(out->error, "missing message", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }

    char conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
    char profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
    chat_state_snapshot(conversation_id, sizeof(conversation_id), profile_id, sizeof(profile_id));

    char *body = heap_caps_calloc(1, CHAT_REQ_MAX, MALLOC_CAP_8BIT);
    if (!body) {
        strlcpy(out->error, "request allocation failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = make_request_body(message, conversation_id, profile_id, body, CHAT_REQ_MAX);
    if (err != ESP_OK) {
        free(body);
        strlcpy(out->error, "request too large", sizeof(out->error));
        robot_set_idle();
        return err;
    }

    robot_set_thinking();
    err = perform_request_once(body, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed once: %s, retrying", esp_err_to_name(err));
        err = perform_request_once(body, out);
    }
    free(body);

    if (err != ESP_OK) {
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
        web_server_set_last_error(out->error[0] ? out->error : "backend unavailable");
        robot_set_error("Backend hiba");
        vTaskDelay(pdMS_TO_TICKS(500));
        robot_set_speaking(out->reply_text);
        vTaskDelay(pdMS_TO_TICKS(1500));
        robot_set_idle();
        return err;
    }
    if (!out->reply_text[0]) {
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
    }

    if (play_downloaded_audio(out)) {
        return ESP_OK;
    }

    robot_set_speaking(out->reply_text);
    vTaskDelay(pdMS_TO_TICKS(1800));
    robot_set_idle();
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

        char message[ROBOT_CHAT_MESSAGE_MAX];
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        strlcpy(message, s_job.message, sizeof(message));
        xSemaphoreGive(s_worker_lock);

        robot_chat_response_t resp = {0};
        esp_err_t err = robot_chat_execute_text(message, &resp);

        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.response = resp;
        s_job.err = err;
        s_job.busy = false;
        xEventGroupSetBits(s_worker_events, CHAT_EVENT_DONE);
        xSemaphoreGive(s_worker_lock);
    }
}

esp_err_t robot_chat_init(void)
{
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    if (!s_state_lock) {
        s_state_lock = xSemaphoreCreateMutex();
    }
    if (!s_worker_lock) {
        s_worker_lock = xSemaphoreCreateMutex();
    }
    if (!s_worker_events) {
        s_worker_events = xEventGroupCreate();
    }
    if (!s_worker_queue) {
        s_worker_queue = xQueueCreate(CHAT_WORKER_QUEUE_LEN, sizeof(uint8_t));
    }
    if (!s_state_lock || !s_worker_lock || !s_worker_events || !s_worker_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_worker_task) {
        if (xTaskCreate(robot_chat_worker_task, "robot_chat_worker", CHAT_WORKER_STACK_BYTES,
                        NULL, 5, &s_worker_task) != pdPASS) {
            s_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_conversation_id[0] = '\0';
    s_profile_id[0] = '\0';
    strlcpy(s_active_mode, "companion", sizeof(s_active_mode));
    char default_base_url[160];
    snprintf(default_base_url, sizeof(default_base_url), "http://%s:%d",
             CONFIG_ROBOT_CHAT_BACKEND_HOST,
             CONFIG_ROBOT_CHAT_BACKEND_PORT);
    return robot_chat_build_backend_urls(default_base_url);
#else
    s_backend_url[0] = '\0';
    s_backend_base_url[0] = '\0';
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

esp_err_t robot_chat_send_text(const char *message, robot_chat_response_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "chat disabled", sizeof(out->error));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!message || !message[0]) {
        strlcpy(out->error, "missing message", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_worker_lock || !s_worker_queue || !s_worker_events || !s_worker_task) {
        strlcpy(out->error, "chat worker not initialized", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);
    if (s_job.busy) {
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "chat busy", sizeof(out->error));
        strlcpy(out->reply_text, "Most meg egy valaszon dolgozom, kerlek varj egy kicsit.",
                sizeof(out->reply_text));
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_job, 0, sizeof(s_job));
    strlcpy(s_job.message, message, sizeof(s_job.message));
    s_job.busy = true;
    xEventGroupClearBits(s_worker_events, CHAT_EVENT_DONE);
    xSemaphoreGive(s_worker_lock);

    uint8_t token = 1;
    if (xQueueSend(s_worker_queue, &token, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.busy = false;
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "chat queue full", sizeof(out->error));
        robot_set_idle();
        return ESP_ERR_TIMEOUT;
    }

    EventBits_t bits = xEventGroupWaitBits(s_worker_events,
                                           CHAT_EVENT_DONE,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CHAT_WAIT_TIMEOUT_MS));
    if ((bits & CHAT_EVENT_DONE) == 0) {
        xSemaphoreTake(s_worker_lock, portMAX_DELAY);
        s_job.busy = false;
        xSemaphoreGive(s_worker_lock);
        strlcpy(out->error, "chat timeout", sizeof(out->error));
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
        robot_set_idle();
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_worker_lock, portMAX_DELAY);
    *out = s_job.response;
    esp_err_t err = s_job.err;
    xSemaphoreGive(s_worker_lock);
    return err;
#endif
}

static esp_err_t voice_upload_impl(const char *wav_path, robot_chat_response_t *out)
{
    /* Snapshot conversation state */
    char conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX] = {0};
    char profile_id[ROBOT_CHAT_PROFILE_ID_MAX] = {0};
    chat_state_snapshot(conversation_id, sizeof(conversation_id),
                        profile_id, sizeof(profile_id));

    /* WAV file size */
    struct stat st = {0};
    if (stat(wav_path, &st) != 0 || st.st_size <= 0) {
        strlcpy(out->error, "voice wav missing", sizeof(out->error));
        return ESP_FAIL;
    }
    size_t wav_size = (size_t)st.st_size;

    /* Build multipart preamble: text fields + file-part header */
    char *preamble = heap_caps_calloc(1, VOICE_PREAMBLE_MAX, MALLOC_CAP_8BIT);
    if (!preamble) {
        strlcpy(out->error, "preamble alloc failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    int plen = 0;

#define MP_TEXT(name, value) \
    plen += snprintf(preamble + plen, VOICE_PREAMBLE_MAX - plen, \
        "--%s\r\nContent-Disposition: form-data; name=\"" name "\"\r\n\r\n%s\r\n", \
        VOICE_BOUNDARY, (value))

    MP_TEXT("device_id", CONFIG_ROBOT_CHAT_DEVICE_ID);
    MP_TEXT("locale",    CONFIG_ROBOT_CHAT_LOCALE);
    MP_TEXT("tts",       "true");
    if (conversation_id[0]) {
        MP_TEXT("conversation_id", conversation_id);
    }
#undef MP_TEXT

    /* File-part header (data follows immediately) */
    plen += snprintf(preamble + plen, VOICE_PREAMBLE_MAX - plen,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"VOICE.WAV\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n",
        VOICE_BOUNDARY);

    if (plen >= VOICE_PREAMBLE_MAX) {
        free(preamble);
        strlcpy(out->error, "preamble too large", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }

    static const char trailer[] = VOICE_TRAILER;
    const size_t trailer_len    = sizeof(trailer) - 1;
    const size_t total_len      = (size_t)plen + wav_size + trailer_len;

    /* Build voice URL */
    if (!s_backend_base_url[0]) {
        free(preamble);
        strlcpy(out->error, "backend URL not set", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }
    char voice_url[192];
    snprintf(voice_url, sizeof(voice_url), "%s%s",
             s_backend_base_url, CONFIG_ROBOT_VOICE_BACKEND_PATH);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", VOICE_BOUNDARY);

    esp_http_client_config_t cfg = {
        .url              = voice_url,
        .timeout_ms       = VOICE_TIMEOUT_MS,
        .buffer_size      = 2048,
        .buffer_size_tx   = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(preamble);
        strlcpy(out->error, "voice http init failed", sizeof(out->error));
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Connection",   "close");

    robot_set_thinking();

    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err != ESP_OK) {
        free(preamble);
        esp_http_client_cleanup(client);
        strlcpy(out->error, "voice http open failed", sizeof(out->error));
        robot_set_idle();
        return err;
    }

    /* Write preamble */
    if (esp_http_client_write(client, preamble, plen) != plen) {
        free(preamble);
        esp_http_client_cleanup(client);
        strlcpy(out->error, "preamble write failed", sizeof(out->error));
        robot_set_idle();
        return ESP_FAIL;
    }
    free(preamble);

    /* Stream WAV file */
    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        esp_http_client_cleanup(client);
        strlcpy(out->error, "cannot open wav for upload", sizeof(out->error));
        robot_set_idle();
        return ESP_FAIL;
    }

    uint8_t *chunk = heap_caps_malloc(VOICE_STREAM_CHUNK,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        fclose(f);
        esp_http_client_cleanup(client);
        strlcpy(out->error, "stream chunk alloc failed", sizeof(out->error));
        robot_set_idle();
        return ESP_ERR_NO_MEM;
    }

    size_t sent = 0;
    while (sent < wav_size && err == ESP_OK) {
        size_t to_read = (wav_size - sent < VOICE_STREAM_CHUNK)
                         ? (wav_size - sent) : VOICE_STREAM_CHUNK;
        size_t nr = fread(chunk, 1, to_read, f);
        if (nr == 0) break;
        if (esp_http_client_write(client, (char *)chunk, (int)nr) != (int)nr) {
            err = ESP_FAIL;
        }
        sent += nr;
    }
    fclose(f);
    free(chunk);

    if (err != ESP_OK || sent != wav_size) {
        esp_http_client_cleanup(client);
        strlcpy(out->error, "wav stream failed", sizeof(out->error));
        robot_set_idle();
        return ESP_FAIL;
    }

    /* Write trailer */
    if (esp_http_client_write(client, trailer, (int)trailer_len) != (int)trailer_len) {
        esp_http_client_cleanup(client);
        strlcpy(out->error, "trailer write failed", sizeof(out->error));
        robot_set_idle();
        return ESP_FAIL;
    }

    /* Read response */
    esp_http_client_fetch_headers(client);
    out->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "voice-chat status=%d wav_bytes=%u", out->http_status, (unsigned)wav_size);

    char *resp_buf = heap_caps_calloc(1, VOICE_RESP_MAX, MALLOC_CAP_8BIT);
    if (resp_buf) {
        int total_rd = 0;
        while (total_rd < VOICE_RESP_MAX - 1) {
            int n = esp_http_client_read(client, resp_buf + total_rd,
                                         VOICE_RESP_MAX - 1 - total_rd);
            if (n <= 0) break;
            total_rd += n;
        }
        resp_buf[total_rd] = '\0';
        if (out->http_status >= 200 && out->http_status < 300) {
            parse_chat_response_json(resp_buf, out);
            if (out->transcript[0]) {
                ESP_LOGI(TAG, "voice transcript received: %s", out->transcript);
            } else {
                ESP_LOGW(TAG, "voice transcript missing or empty");
            }
        } else {
            snprintf(out->error, sizeof(out->error),
                     "voice backend status=%d", out->http_status);
        }
        free(resp_buf);
    }
    esp_http_client_cleanup(client);

    if (out->http_status < 200 || out->http_status >= 300) {
        robot_set_idle();
        return ESP_FAIL;
    }

    if (!out->reply_text[0]) {
        strlcpy(out->reply_text, fallback_text(), sizeof(out->reply_text));
    }

    if (play_downloaded_audio(out)) {
        return ESP_OK;
    }
    robot_set_speaking(out->reply_text);
    vTaskDelay(pdMS_TO_TICKS(1800));
    robot_set_idle();
    return ESP_OK;
}

esp_err_t robot_chat_send_voice_file(const char *wav_path, robot_chat_response_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

#if !CONFIG_ROBOT_ENABLE_KIDS_CHAT
    strlcpy(out->error, "chat disabled", sizeof(out->error));
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!wav_path || !wav_path[0]) {
        strlcpy(out->error, "missing wav path", sizeof(out->error));
        return ESP_ERR_INVALID_ARG;
    }
    return voice_upload_impl(wav_path, out);
#endif
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
    if (error && error_len) {
        strlcpy(error, "voice input disabled", error_len);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void robot_chat_audio_state_changed(esp_asp_state_t state)
{
    (void)state;
}
