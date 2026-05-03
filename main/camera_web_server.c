#include "camera_web_server.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "cam_server";

static httpd_handle_t    s_server  = NULL;
static SemaphoreHandle_t s_cam_mux = NULL;

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
    /* Caller MUST call camera_release_frame() to unlock the mutex. */
}

void camera_release_frame(camera_fb_t *fb)
{
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_cam_mux);
}

/* ------------------------------------------------------------------ */
/* HTML page                                                           */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='utf-8'>"
    "<title>Robot Camera</title>"
    "<style>"
    "body{background:#111;color:#eee;font-family:sans-serif;"
    "     display:flex;flex-direction:column;align-items:center;"
    "     padding:20px;margin:0}"
    "h2{margin-bottom:12px}"
    "img{max-width:100%;border:2px solid #555;border-radius:4px}"
    "#status{margin-top:10px;font-size:0.9em;color:#aaa}"
    "</style>"
    "</head><body>"
    "<h2>ESP32 Robot Camera</h2>"
    "<img id='cam' src='/capture.jpg'>"
    "<div id='status'>Connecting...</div>"
    "<script>"
    "let ok=0,fail=0;"
    "setInterval(()=>{"
    "  const img=new Image();"
    "  const t=Date.now();"
    "  img.onload=()=>{"
    "    document.getElementById('cam').src=img.src;"
    "    document.getElementById('status').textContent='OK '+ok++;"
    "  };"
    "  img.onerror=()=>{"
    "    document.getElementById('status').textContent='ERR '+fail++;"
    "  };"
    "  img.src='/capture.jpg?t='+t;"
    "},1000);"
    "</script>"
    "</body></html>";

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
    camera_fb_t *fb = NULL;
    if (camera_capture_frame(&fb) != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    /* Convert RGB565 frame to JPEG; frame buffer returned before HTTP send. */
    uint8_t *jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);

    ESP_LOGI(TAG, "Frame %dx%d len=%zu → JPEG %zu B  ok=%d",
             fb->width, fb->height, fb->len, jpg_len, ok);

    camera_release_frame(fb);   /* release ASAP, before the slow HTTP send */

    if (!ok || !jpg_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg conv failed");
        free(jpg_buf);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, (ssize_t)jpg_len);
    free(jpg_buf);
    return res;
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

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", cfg.server_port);
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t index_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = index_handler,
    };
    static const httpd_uri_t capture_uri = {
        .uri     = "/capture.jpg",
        .method  = HTTP_GET,
        .handler = capture_handler,
    };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &capture_uri);

    ESP_LOGI(TAG, "Routes registered: GET /  GET /capture.jpg");
    return ESP_OK;
}
