#include "camera_web_server.h"
#include "face_enroll.h"
#include "face_status.h"
#include "robot_state.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "cam_server";

static httpd_handle_t    s_server  = NULL;
static SemaphoreHandle_t s_cam_mux = NULL;

/* Last successfully encoded JPEG — served as a stale frame while audio is playing
   so that frame2jpg() never competes with SDMMC DMA during MP3 playback.
   Owned by the httpd task; no mutex needed (single httpd task processes requests). */
static uint8_t *s_cached_jpg     = NULL;
static size_t   s_cached_jpg_len = 0;

/* Print "skipped while speaking" at most once per speaking episode. */
static bool s_last_was_speaking = false;

/* ------------------------------------------------------------------ */
/* Shared camera capture API (used by web server and face_detect task) */
/* ------------------------------------------------------------------ */

esp_err_t camera_capture_frame(camera_fb_t **fb)
{
    if (!s_cam_mux) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cam_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *fb = esp_camera_fb_get();
    if (!*fb) {
        xSemaphoreGive(s_cam_mux);
        return ESP_FAIL;
    }
    return ESP_OK;
    /* Caller MUST call camera_release_frame() to release the mutex. */
}

void camera_release_frame(camera_fb_t *fb)
{
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_cam_mux);
}

/* ------------------------------------------------------------------ */
/* JSON response helpers                                               */
/* ------------------------------------------------------------------ */

static void send_json_ok(httpd_req_t *req, const char *extra_fields)
{
    char buf[256];
    if (extra_fields && extra_fields[0]) {
        snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extra_fields);
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void send_json_error(httpd_req_t *req, const char *error)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}",
             error ? error : "unknown error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Read up to bufsize-1 bytes of request body; null-terminates buf. */
static int read_body(httpd_req_t *req, char *buf, size_t bufsize)
{
    if (!req->content_len) { buf[0] = '\0'; return 0; }
    size_t to_read = req->content_len < bufsize - 1 ? req->content_len : bufsize - 1;
    int n = httpd_req_recv(req, buf, to_read);
    if (n < 0) return n;
    buf[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* HTML page                                                           */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><title>Robot Camera</title>"
    "<style>"
    "body{background:#111;color:#eee;font-family:sans-serif;"
    "display:flex;flex-direction:column;align-items:center;padding:20px;margin:0}"
    "h2{margin-bottom:12px}"
    "img{max-width:100%;border:2px solid #555;border-radius:4px}"
    "#status{margin-top:10px;font-size:.9em;color:#aaa}"
    ".card{background:#1e1e1e;border:1px solid #444;border-radius:8px;"
    "padding:16px;margin-top:20px;width:100%;max-width:420px;box-sizing:border-box}"
    ".card h3{margin:0 0 10px;font-size:.95em;color:#bbb}"
    "label{font-size:.8em;color:#999;display:block;margin-top:6px}"
    "input{background:#252525;border:1px solid #555;color:#eee;padding:5px 8px;"
    "border-radius:4px;width:100%;box-sizing:border-box;margin:2px 0 4px;font-size:.9em}"
    ".row{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}"
    "button{padding:7px 13px;border:none;border-radius:4px;cursor:pointer;"
    "font-size:.82em;font-weight:600}"
    ".bs{background:#2563eb;color:#fff}"
    ".bc{background:#16a34a;color:#fff}"
    ".bf{background:#7c3aed;color:#fff}"
    ".bx{background:#dc2626;color:#fff}"
    "button:disabled{opacity:.35;cursor:not-allowed}"
    "#estat{margin-top:10px;padding:8px;background:#222;border-radius:4px;"
    "font-size:.82em;color:#999;min-height:2em}"
    ".ok{color:#4ade80}.er{color:#f87171}.warn{color:#fbbf24}"
    "</style></head><body>"
    "<h2>ESP32 Robot Camera</h2>"
    "<img id='cam' src='/capture.jpg'>"
    "<div id='status'>Connecting...</div>"
    "<div class='card'>"
    "<h3>Detection Status</h3>"
    "<div id='rdet'>No face detected</div>"
    "<div id='rinfo' style='font-size:.78em;color:#666;margin-top:4px'></div>"
    "</div>"
    "<div class='card'>"
    "<h3>Robot State</h3>"
    "<div id='rst' style='font-size:.9em'></div>"
    "<div id='rled' style='font-size:.78em;color:#888;margin-top:3px'></div>"
    "<div id='rperson' style='font-size:.78em;color:#888;margin-top:3px'></div>"
    "<div id='raudio' style='font-size:.78em;color:#888;margin-top:3px'></div>"
    "</div>"
    "<div class='card'>"
    "<h3>Face Enrollment</h3>"
    "<label>Person ID</label>"
    "<input id='eid' placeholder='apa'>"
    "<label>Display Name</label>"
    "<input id='ename' placeholder='Apa'>"
    "<label>Audio File</label>"
    "<input id='eaudio' placeholder='/sdcard/APA.MP3'>"
    "<div class='row'>"
    "<button class='bs' id='bst' onclick='eStart()'>Start</button>"
    "<button class='bc' id='bca' onclick='eCap()' disabled>Capture</button>"
    "<button class='bf' id='bfi' onclick='eFin()' disabled>Finish</button>"
    "<button class='bx' id='bcn' onclick='eCancel()' disabled>Cancel</button>"
    "</div>"
    "<div id='estat'>Idle</div>"
    "</div>"
    "<script>"
    "let ok=0,fail=0;"
    "setInterval(()=>{"
    "  const i=new Image(),t=Date.now();"
    "  i.onload=()=>{document.getElementById('cam').src=i.src;"
    "    document.getElementById('status').textContent='OK '+ok++;};"
    "  i.onerror=()=>{document.getElementById('status').textContent='ERR '+fail++;};"
    "  i.src='/capture.jpg?t='+t;"
    "},1000);"
    "function enrBtn(on){"
    "  document.getElementById('bst').disabled=on;"
    "  ['bca','bfi','bcn'].forEach(id=>{"
    "    document.getElementById(id).disabled=!on;});"
    "}"
    "function stat(msg,cls){"
    "  const e=document.getElementById('estat');"
    "  e.textContent=msg;e.className=cls||'';}"
    "async function jpost(u,b){"
    "  const opts={method:'POST'};"
    "  if(b){opts.headers={'Content-Type':'application/json'};"
    "    opts.body=JSON.stringify(b);}"
    "  const r=await fetch(u,opts);"
    "  return r.json();}"
    "async function eStart(){"
    "  const id=document.getElementById('eid').value.trim();"
    "  const nm=document.getElementById('ename').value.trim();"
    "  const au=document.getElementById('eaudio').value.trim();"
    "  if(!id||!nm){stat('ID and Name are required','er');return;}"
    "  const r=await jpost('/enroll/start',{id:id,display_name:nm,audio_file:au});"
    "  if(r.ok){enrBtn(true);stat('Enrolling '+nm+' — capture at least 3 samples','ok');}"
    "  else stat('Error: '+r.error,'er');}"
    "async function eCap(){"
    "  stat('Capturing...','');"
    "  const r=await jpost('/enroll/capture');"
    "  if(r.ok)stat('Sample '+r.sample_count+' captured'"
    "    +(r.sample_count<3?' — need '+(3-r.sample_count)+' more':''),'ok');"
    "  else stat('Error: '+r.error,'er');}"
    "async function eFin(){"
    "  const r=await jpost('/enroll/finish');"
    "  if(r.ok){enrBtn(false);stat('Enrollment saved: '+r.id+', samples: '+r.samples,'ok');}"
    "  else stat('Error: '+r.error,'er');}"
    "async function eCancel(){"
    "  await jpost('/enroll/cancel');"
    "  enrBtn(false);stat('Cancelled','');}"
    "setInterval(async()=>{"
    "  try{"
    "    const s=await(await fetch('/enroll/status')).json();"
    "    if(s.active){enrBtn(true);"
    "      stat('Enrolling: '+s.display_name+' | Samples: '+s.sample_count"
    "        +(s.sample_count<3?' ('+(3-s.sample_count)+' more needed)':''),'ok');}"
    "  }catch(e){}},4000);"
    "setInterval(async()=>{"
    "  try{"
    "    const d=await(await fetch('/status')).json();"
    "    const el=document.getElementById('rdet');"
    "    const il=document.getElementById('rinfo');"
    "    if(!d.face_present){"
    "      el.textContent='No face detected';el.className='';il.textContent='';}"
    "    else if(!d.recognition_available){"
    "      el.textContent='Face detected — recognition not available';"
    "      el.className='warn';il.textContent='';}"
    "    else if(!d.recognized){"
    "      el.textContent='Face detected — unknown person';"
    "      el.className='warn';il.textContent='';}"
    "    else{"
    "      el.textContent='Recognized: '+d.display_name;"
    "      el.className='ok';"
    "      il.textContent='Confidence: '+Math.round(d.confidence*100)+'%  Audio: '+d.audio_file;}"
    "    const sc={'IDLE':'','SPEAKING':'ok','THINKING':'warn','LISTENING':'ok','SLEEPING':''};"
    "    const rst=document.getElementById('rst');"
    "    rst.textContent='State: '+d.robot_state;"
    "    rst.className=sc[d.robot_state]||'';"
    "    document.getElementById('rled').textContent='LED: '+d.led_state;"
    "    if(d.display_name)document.getElementById('rperson').textContent='Last person: '+d.display_name+(d.person_id?'  ('+d.person_id+')':'');"
    "    if(d.last_audio)document.getElementById('raudio').textContent='Last audio: '+d.last_audio;"
    "  }catch(e2){}},750);"
    "</script></body></html>";

/* ------------------------------------------------------------------ */
/* Route: GET /                                                        */
/* ------------------------------------------------------------------ */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Route: GET /capture.jpg                                            */
/* ------------------------------------------------------------------ */
static esp_err_t capture_handler(httpd_req_t *req)
{
    /* While audio is playing: skip camera/JPEG work entirely to avoid
       competing with SDMMC DMA.  Serve the last cached frame instead. */
    if (robot_is_speaking()) {
        if (!s_last_was_speaking) {
            ESP_LOGI(TAG, "capture skipped while speaking — serving cached frame");
            s_last_was_speaking = true;
        }
        if (s_cached_jpg && s_cached_jpg_len > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, (const char *)s_cached_jpg,
                                   (ssize_t)s_cached_jpg_len);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "robot speaking, no cached frame yet");
        return ESP_FAIL;
    }

    s_last_was_speaking = false;

    camera_fb_t *fb = NULL;
    if (camera_capture_frame(&fb) != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    /* Second guard: audio may have started while we were grabbing the frame. */
    if (robot_is_speaking()) {
        camera_release_frame(fb);
        s_last_was_speaking = true;
        ESP_LOGI(TAG, "capture skipped while speaking (post-grab) — serving cached frame");
        if (s_cached_jpg && s_cached_jpg_len > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, (const char *)s_cached_jpg,
                                   (ssize_t)s_cached_jpg_len);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "speaking, no cached frame yet");
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);

    ESP_LOGI(TAG, "Frame %dx%d len=%zu → JPEG %zu B  ok=%d",
             fb->width, fb->height, fb->len, jpg_len, ok);

    camera_release_frame(fb);

    if (!ok || !jpg_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg conv failed");
        free(jpg_buf);
        return ESP_FAIL;
    }

    free(s_cached_jpg);
    s_cached_jpg     = jpg_buf;
    s_cached_jpg_len = jpg_len;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)s_cached_jpg,
                           (ssize_t)s_cached_jpg_len);
}

/* ------------------------------------------------------------------ */
/* Route: GET /enroll/status                                          */
/* ------------------------------------------------------------------ */
static esp_err_t enroll_status_handler(httpd_req_t *req)
{
    enroll_status_t st = face_enroll_get_status();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"id\":\"%s\",\"display_name\":\"%s\",\"sample_count\":%d}",
             st.active ? "true" : "false",
             st.id, st.display_name, st.sample_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Route: POST /enroll/start                                          */
/* Body: {"id":"apa","display_name":"Apa","audio_file":"/sdcard/..."}*/
/* ------------------------------------------------------------------ */
static esp_err_t enroll_start_handler(httpd_req_t *req)
{
    char body[512];
    int n = read_body(req, body, sizeof(body));
    if (n < 0) {
        send_json_error(req, "body recv failed");
        return ESP_FAIL;
    }

    esp_err_t err = face_enroll_start_json(body);
    if (err == ESP_ERR_INVALID_ARG) {
        send_json_error(req, "invalid JSON or missing id/display_name");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        send_json_error(req, "start failed");
        return ESP_FAIL;
    }
    send_json_ok(req, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: POST /enroll/capture                                        */
/* ------------------------------------------------------------------ */
static esp_err_t enroll_capture_handler(httpd_req_t *req)
{
    int count = 0;
    esp_err_t err = face_enroll_capture(&count);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        send_json_error(req, "Maximum samples reached");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        send_json_error(req, "no face detected");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        send_json_error(req, "multiple faces, audio playing, or not in enrollment mode");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_NO_MEM) {
        send_json_error(req, "out of memory");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        send_json_error(req, "capture or SD write failed");
        return ESP_FAIL;
    }

    char extra[48];
    snprintf(extra, sizeof(extra), "\"sample_count\":%d", count);
    send_json_ok(req, extra);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: POST /enroll/finish                                         */
/* ------------------------------------------------------------------ */
static esp_err_t enroll_finish_handler(httpd_req_t *req)
{
    int  samples = 0;
    char id[32]  = {0};

    esp_err_t err = face_enroll_finish(&samples, id, sizeof(id));
    if (err == ESP_ERR_INVALID_STATE) {
        send_json_error(req, "not enrolling or need at least 3 samples");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        send_json_error(req, "failed to save DB.TXT");
        return ESP_FAIL;
    }

    char extra[80];
    snprintf(extra, sizeof(extra), "\"samples\":%d,\"id\":\"%s\"", samples, id);
    send_json_ok(req, extra);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: POST /enroll/cancel                                         */
/* ------------------------------------------------------------------ */
static esp_err_t enroll_cancel_handler(httpd_req_t *req)
{
    face_enroll_cancel();
    send_json_ok(req, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: GET /status                                                  */
/* ------------------------------------------------------------------ */
static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[512];
    face_status_get_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t camera_web_server_start(void)
{
    s_cam_mux = xSemaphoreCreateMutex();
    if (!s_cam_mux) {
        ESP_LOGE(TAG, "Failed to create camera mutex");
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", cfg.server_port);
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler,
    };
    static const httpd_uri_t capture_uri = {
        .uri = "/capture.jpg", .method = HTTP_GET, .handler = capture_handler,
    };
    static const httpd_uri_t enroll_status_uri = {
        .uri = "/enroll/status", .method = HTTP_GET, .handler = enroll_status_handler,
    };
    static const httpd_uri_t enroll_start_uri = {
        .uri = "/enroll/start", .method = HTTP_POST, .handler = enroll_start_handler,
    };
    static const httpd_uri_t enroll_capture_uri = {
        .uri = "/enroll/capture", .method = HTTP_POST, .handler = enroll_capture_handler,
    };
    static const httpd_uri_t enroll_finish_uri = {
        .uri = "/enroll/finish", .method = HTTP_POST, .handler = enroll_finish_handler,
    };
    static const httpd_uri_t enroll_cancel_uri = {
        .uri = "/enroll/cancel", .method = HTTP_POST, .handler = enroll_cancel_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_handler,
    };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &capture_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &enroll_status_uri);
    httpd_register_uri_handler(s_server, &enroll_start_uri);
    httpd_register_uri_handler(s_server, &enroll_capture_uri);
    httpd_register_uri_handler(s_server, &enroll_finish_uri);
    httpd_register_uri_handler(s_server, &enroll_cancel_uri);

    ESP_LOGI(TAG, "Routes: GET /  GET /capture.jpg  GET /status  GET+POST /enroll/*");
    return ESP_OK;
}
