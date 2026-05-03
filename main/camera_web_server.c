#include "camera_web_server.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "cam_server";

static httpd_handle_t  s_server    = NULL;
static SemaphoreHandle_t s_cam_mux = NULL;

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
    "</style>"
    "</head><body>"
    "<h2>ESP32 Robot Camera</h2>"
    "<img id='cam' src='/capture.jpg'>"
    "<script>"
    "setInterval(()=>{"
    "  document.getElementById('cam').src='/capture.jpg?t='+Date.now();"
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
    if (xSemaphoreTake(s_cam_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Camera mutex timeout");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
        return ESP_FAIL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(s_cam_mux);
        ESP_LOGE(TAG, "esp_camera_fb_get() returned NULL");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serving frame: %dx%d  len=%zu", fb->width, fb->height, fb->len);

    esp_err_t res = ESP_OK;
    if (fb->format == PIXFORMAT_JPEG) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        res = httpd_resp_send(req, (const char *)fb->buf, (ssize_t)fb->len);
    } else {
        ESP_LOGE(TAG, "Frame is not JPEG (format=%d)", (int)fb->format);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not jpeg");
        res = ESP_FAIL;
    }

    esp_camera_fb_return(fb);
    xSemaphoreGive(s_cam_mux);
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
