#include "web_server.h"

#include "audio_driver.h"
#include "led_effects.h"
#include "robot_chat.h"
#include "robot_state.h"
#include "wifi_manager.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ROBOT_ENABLE_VOICE_INPUT
#define CONFIG_ROBOT_ENABLE_VOICE_INPUT 0
#endif

#ifndef CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
#define CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE 0
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server;
static char s_last_audio[96];
static char s_last_error[128];
static char s_last_transcript[320];

void web_server_set_last_audio(const char *path)
{
    strlcpy(s_last_audio, path ? path : "", sizeof(s_last_audio));
}

void web_server_set_last_error(const char *error)
{
    strlcpy(s_last_error, error ? error : "", sizeof(s_last_error));
}

void web_server_set_last_transcript(const char *transcript)
{
    strlcpy(s_last_transcript, transcript ? transcript : "", sizeof(s_last_transcript));
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

static int read_body(httpd_req_t *req, char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) {
        return -1;
    }
    if (req->content_len == 0) {
        buf[0] = '\0';
        return 0;
    }
    size_t to_read = req->content_len < bufsize - 1 ? req->content_len : bufsize - 1;
    int n = httpd_req_recv(req, buf, to_read);
    if (n < 0) {
        return n;
    }
    buf[n] = '\0';
    return n;
}

static void json_str(const char *body, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!body || !key) {
        return;
    }

    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) {
        return;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
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

static void send_json_ok(httpd_req_t *req, const char *extra)
{
    char buf[256];
    if (extra && extra[0]) {
        snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extra);
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void send_json_error(httpd_req_t *req, const char *error)
{
    char escaped[160];
    char buf[220];
    json_escape(error ? error : "unknown error", escaped, sizeof(escaped));
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static const char INDEX_HTML[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Robot Assistant</title>"
    "<style>"
    "body{background:#101114;color:#eceff3;font-family:Arial,sans-serif;margin:0;padding:14px;max-width:560px;margin-inline:auto}"
    "h2{font-size:1.2rem;text-align:center;margin:8px 0 14px}"
    ".card{background:#1b1e24;border:1px solid #343944;border-radius:8px;padding:12px;margin:10px 0}"
    "textarea,input{width:100%;box-sizing:border-box;background:#12141a;color:#fff;border:1px solid #444b58;border-radius:5px;padding:8px;font-size:.95rem}"
    "textarea{min-height:92px;resize:vertical}"
    "button{border:0;border-radius:5px;padding:8px 12px;font-weight:700;cursor:pointer;margin:6px 6px 0 0}"
    ".primary{background:#2f6fed;color:#fff}.muted{background:#394150;color:#fff}.danger{background:#c24141;color:#fff}.think{background:#b7791f;color:#fff}"
    ".msg{white-space:pre-wrap;background:#12141a;border-radius:5px;padding:8px;margin-top:8px;min-height:1.4em;color:#c9d1dc}"
    ".ok{color:#6ee7a8}.err{color:#ff8585}table{width:100%;border-collapse:collapse;font-size:.9rem}td{border-bottom:1px solid #2b3039;padding:5px}"
    "td:first-child{color:#9ca3af;width:36%}"
    "</style></head><body>"
    "<h2>Robot Assistant</h2>"
    "<div class='card'><textarea id='msg'>Szia, Zita vagyok</textarea>"
    "<button class='primary' onclick='chatSend()'>Send</button>"
    "<div id='meta' class='msg'>Conversation: none | Mode: companion</div>"
    "<div id='out' class='msg'>ESP32 proxy: /api/chat</div></div>"
    "<div class='card'><table>"
    "<tr><td>State</td><td id='st'></td></tr>"
    "<tr><td>LED</td><td id='led'></td></tr>"
    "<tr><td>WiFi</td><td id='wifi'></td></tr>"
    "<tr><td>Backend</td><td id='be'></td></tr>"
    "<tr><td>Last audio</td><td id='aud'></td></tr>"
    "<tr><td>Last error</td><td id='err'></td></tr>"
    "<tr><td>Last heard</td><td id='tr' style='color:#f0c040;font-style:italic'>-</td></tr>"
    "</table></div>"
    "<div class='card'><input id='afile' placeholder='/sdcard/REPLY.MP3'>"
    "<button class='muted' onclick='playAudio()'>Play MP3</button><div id='amsg' class='msg'></div></div>"
    "<div class='card'>"
    "<button class='muted' onclick='led(\"idle\")'>Idle</button>"
    "<button class='think' onclick='led(\"thinking\")'>Thinking</button>"
    "<button class='primary' onclick='led(\"speaking\")'>Speaking</button>"
    "<button class='danger' onclick='led(\"error\")'>Error</button>"
    "<div id='lmsg' class='msg'></div></div>"
    "<script>"
    "let cid='';let mode='companion';"
    "async function jpost(u,b){const o={method:'POST'};if(b){o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(b)}return(await fetch(u,o)).json()}"
    "async function chatSend(){const m=document.getElementById('msg').value.trim();const out=document.getElementById('out');if(!m){out.textContent='Enter a message';out.className='msg err';return}out.textContent='Thinking...';out.className='msg';try{const b={message:m,tts:true};if(cid)b.conversation_id=cid;const r=await jpost('/api/chat',b);if(r.conversation_id)cid=r.conversation_id;if(r.active_mode)mode=r.active_mode;document.getElementById('meta').textContent='Conversation: '+(cid||'none')+' | Mode: '+mode;out.textContent=(r.reply_text||r.error||'No reply')+(r.reply_audio_url?'\\nAudio: '+r.reply_audio_url:'')+'\\nconversation_id='+(r.conversation_id||'');out.className='msg '+(r.ok?'ok':'err')}catch(e){out.textContent='Request failed';out.className='msg err'}}"
    "async function status(){try{const d=await(await fetch('/status')).json();st.textContent=d.robot_state||'';led.textContent=d.led_state||'';wifi.textContent=(d.wifi_ssid||'')+' '+(d.wifi_rssi||0)+' dBm';be.textContent=d.backend_base_url||'';aud.textContent=d.last_audio||'-';err.textContent=d.last_error||'-';const t=d.last_transcript||'';tr.textContent=t||'-';tr.style.color=t?'#f0c040':'#666'}catch(e){}}"
    "async function playAudio(){const f=afile.value.trim();const r=await jpost('/api/play?file='+encodeURIComponent(f),{});amsg.textContent=r.ok?'Playing '+f:(r.error||'Error');amsg.className='msg '+(r.ok?'ok':'err')}"
    "async function led(s){const r=await jpost('/api/led/'+s,{});lmsg.textContent=r.ok?'LED '+s:(r.error||'Error');lmsg.className='msg '+(r.ok?'ok':'err')}"
    "setInterval(status,750);status();"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ssid[96];
    char backend[220];
    char audio[160];
    char error[220];
    char transcript[400];
    json_escape(wifi_manager_ssid(), ssid, sizeof(ssid));
    json_escape(wifi_manager_backend_base_url(), backend, sizeof(backend));
    json_escape(s_last_audio, audio, sizeof(audio));
    json_escape(s_last_error, error, sizeof(error));
    json_escape(s_last_transcript, transcript, sizeof(transcript));

    char resp[1400];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"robot_state\":\"%s\",\"led_state\":\"%s\","
             "\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,\"backend_base_url\":\"%s\","
             "\"last_audio\":\"%s\",\"last_error\":\"%s\",\"last_transcript\":\"%s\"}",
             robot_state_name(robot_get_state()),
             led_state_name(led_get_state()),
             ssid,
             wifi_manager_rssi(),
             backend,
             audio,
             error,
             transcript);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static bool is_valid_audio_path(const char *p)
{
    if (!p || strncmp(p, "/sdcard/", 8) != 0 || strstr(p, "..")) {
        return false;
    }
    size_t len = strlen(p);
    if (len < 12) {
        return false;
    }
    const char *e = p + len - 4;
    return e[0] == '.' &&
           (e[1] == 'm' || e[1] == 'M') &&
           (e[2] == 'p' || e[2] == 'P') &&
           e[3] == '3';
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    char *d = dst;
    char *end = dst + dst_len - 1;
    while (src && *src && d < end) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *d++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *d++ = ' ';
            src++;
        } else {
            *d++ = *src++;
        }
    }
    *d = '\0';
}

static esp_err_t api_play_handler(httpd_req_t *req)
{
    char raw[128] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(raw)) {
        char query[160];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "file", raw, sizeof(raw));
        }
    }
    if (!raw[0]) {
        char body[160];
        (void)read_body(req, body, sizeof(body));
        httpd_query_key_value(body, "file", raw, sizeof(raw));
    }

    char file[128];
    url_decode(file, sizeof(file), raw);
    if (!is_valid_audio_path(file)) {
        send_json_error(req, "invalid path: must be /sdcard/*.mp3");
        return ESP_OK;
    }

    char url[140];
    snprintf(url, sizeof(url), "file://sdcard/%s", file + 8);
    web_server_set_last_audio(file);
    web_server_set_last_error(NULL);
    if (robot_say_file("Audio test", url) != ESP_GMF_ERR_OK) {
        web_server_set_last_error("audio playback failed");
        send_json_error(req, "audio playback failed");
        return ESP_OK;
    }
    send_json_ok(req, NULL);
    return ESP_OK;
}

static esp_err_t api_chat_handler(httpd_req_t *req)
{
    typedef struct {
        char body[768];
        char message[512];
        char reply[768];
        char audio[360];
        char error[160];
        char conversation[128];
        char active[48];
        char resp[1900];
    } chat_buf_t;

    chat_buf_t *buf = heap_caps_calloc(1, sizeof(*buf), MALLOC_CAP_8BIT);
    if (!buf) {
        send_json_error(req, "chat buffer allocation failed");
        return ESP_OK;
    }
    if (read_body(req, buf->body, sizeof(buf->body)) < 0) {
        free(buf);
        send_json_error(req, "body recv failed");
        return ESP_OK;
    }
    json_str(buf->body, "message", buf->message, sizeof(buf->message));
    if (!buf->message[0]) {
        free(buf);
        send_json_error(req, "missing message");
        return ESP_OK;
    }

    robot_chat_response_t chat = {0};
    esp_err_t err = robot_chat_send_text(buf->message, &chat);
    if (chat.reply_audio_url[0]) {
        web_server_set_last_audio("/sdcard/REPLY.MP3");
    }
    web_server_set_last_error(err == ESP_OK ? NULL : chat.error);

    json_escape(chat.reply_text, buf->reply, sizeof(buf->reply));
    json_escape(chat.reply_audio_url, buf->audio, sizeof(buf->audio));
    json_escape(chat.error, buf->error, sizeof(buf->error));
    json_escape(chat.conversation_id, buf->conversation, sizeof(buf->conversation));
    json_escape(chat.active_mode, buf->active, sizeof(buf->active));
    snprintf(buf->resp, sizeof(buf->resp),
             "{\"ok\":%s,\"safe\":%s,\"http_status\":%d,"
             "\"conversation_id\":\"%s\",\"reply_text\":\"%s\","
             "\"reply_audio_url\":\"%s\",\"active_mode\":\"%s\",\"error\":\"%s\"}",
             err == ESP_OK ? "true" : "false",
             chat.safe ? "true" : "false",
             chat.http_status,
             buf->conversation,
             buf->reply,
             buf->audio,
             buf->active,
             buf->error);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf->resp, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t api_led_idle_handler(httpd_req_t *req)
{
    led_test_force(LED_IDLE, 60);
    send_json_ok(req, NULL);
    return ESP_OK;
}

static esp_err_t api_led_thinking_handler(httpd_req_t *req)
{
    led_test_force(LED_THINKING, 60);
    send_json_ok(req, NULL);
    return ESP_OK;
}

static esp_err_t api_led_speaking_handler(httpd_req_t *req)
{
    led_test_force(LED_SPEAKING, 60);
    send_json_ok(req, NULL);
    return ESP_OK;
}

static esp_err_t api_led_error_handler(httpd_req_t *req)
{
    led_test_force(LED_ERROR, 60);
    send_json_ok(req, NULL);
    return ESP_OK;
}

#if CONFIG_ROBOT_ENABLE_VOICE_INPUT && CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
static esp_err_t api_voice_disabled_handler(httpd_req_t *req)
{
    send_json_error(req, "voice input is not implemented in this MVP");
    return ESP_OK;
}
#endif

esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 12288;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

#define REG(uri_str, meth, fn) do { \
        static const httpd_uri_t u = {.uri = uri_str, .method = meth, .handler = fn}; \
        esp_err_t reg_err = httpd_register_uri_handler(s_server, &u); \
        if (reg_err != ESP_OK) { \
            ESP_LOGE(TAG, "failed to register %s: %s", uri_str, esp_err_to_name(reg_err)); \
        } \
    } while (0)

    REG("/", HTTP_GET, index_handler);
    REG("/status", HTTP_GET, status_handler);
    REG("/api/chat", HTTP_POST, api_chat_handler);
    REG("/api/robot/chat", HTTP_POST, api_chat_handler);
    REG("/api/play", HTTP_POST, api_play_handler);
    REG("/api/led/idle", HTTP_POST, api_led_idle_handler);
    REG("/api/led/thinking", HTTP_POST, api_led_thinking_handler);
    REG("/api/led/speaking", HTTP_POST, api_led_speaking_handler);
    REG("/api/led/error", HTTP_POST, api_led_error_handler);
#if CONFIG_ROBOT_ENABLE_VOICE_INPUT && CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
    REG("/api/voice/record", HTTP_POST, api_voice_disabled_handler);
#endif

#undef REG

    ESP_LOGI(TAG, "HTTP routes: GET / /status");
    ESP_LOGI(TAG, "HTTP routes: POST /api/chat /api/robot/chat /api/play /api/led/{idle,thinking,speaking,error}");
#if !(CONFIG_ROBOT_ENABLE_VOICE_INPUT && CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE)
    ESP_LOGI(TAG, "voice web routes disabled");
#endif
    return ESP_OK;
}
