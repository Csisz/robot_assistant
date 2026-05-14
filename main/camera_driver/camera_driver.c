#include "camera_driver.h"
#include "tca9555_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "camera_driver";

static bool s_initialized = false;
static volatile bool s_paused_for_voice = false;
static SemaphoreHandle_t s_cam_mux = NULL;
static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_health_t s_health = {0};

static camera_config_t camera_config = {
    .pin_pwdn      = CAM_PIN_PWDN,
    .pin_reset     = CAM_PIN_RESET,
    .pin_xclk      = CAM_PIN_XCLK,
    .pin_sccb_sda  = CAM_PIN_SIOD,
    .pin_sccb_scl  = CAM_PIN_SIOC,

    .pin_d7        = CAM_PIN_D7,
    .pin_d6        = CAM_PIN_D6,
    .pin_d5        = CAM_PIN_D5,
    .pin_d4        = CAM_PIN_D4,
    .pin_d3        = CAM_PIN_D3,
    .pin_d2        = CAM_PIN_D2,
    .pin_d1        = CAM_PIN_D1,
    .pin_d0        = CAM_PIN_D0,
    .pin_vsync     = CAM_PIN_VSYNC,
    .pin_href      = CAM_PIN_HREF,
    .pin_pclk      = CAM_PIN_PCLK,

    .xclk_freq_hz  = 20000000,
    .ledc_timer    = CAM_LEDC_TIMER,
    .ledc_channel  = CAM_LEDC_CHANNEL,

    .pixel_format  = PIXFORMAT_RGB565,   /* raw frames for face detection; web server converts to JPEG */
    .frame_size    = FRAMESIZE_QQVGA,   /* RGB565 + Wi-Fi is most stable at one small raw buffer */
    .jpeg_quality  = 12,
    .fb_count      = 1,
    .fb_location   = CAMERA_FB_IN_PSRAM,
    .grab_mode     = CAMERA_GRAB_LATEST,
};

static const char *grab_mode_name(camera_grab_mode_t mode)
{
    return mode == CAMERA_GRAB_LATEST ? "LATEST" : "WHEN_EMPTY";
}

static void health_set_config(void)
{
    portENTER_CRITICAL(&s_health_lock);
    s_health.xclk_freq_hz = camera_config.xclk_freq_hz;
    s_health.pixel_format = (int)camera_config.pixel_format;
    s_health.frame_size = (int)camera_config.frame_size;
    s_health.jpeg_quality = camera_config.jpeg_quality;
    s_health.fb_count = (int)camera_config.fb_count;
    s_health.fb_location = (int)camera_config.fb_location;
    s_health.grab_mode = (int)camera_config.grab_mode;
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_set_error(esp_err_t err, const char *msg, bool timeout)
{
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_health_lock);
    s_health.capture_fail_count++;
    if (timeout) {
        s_health.capture_timeout_count++;
    }
    s_health.last_fail_us = now_us;
    s_health.init_err = s_health.initialized ? s_health.init_err : err;
    strlcpy(s_health.last_error, msg ? msg : esp_err_to_name(err), sizeof(s_health.last_error));
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_set_capture_ok(const camera_fb_t *fb)
{
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_health_lock);
    s_health.capture_ok_count++;
    s_health.frames_in_flight++;
    s_health.last_success_us = now_us;
    s_health.last_width = fb ? (int)fb->width : 0;
    s_health.last_height = fb ? (int)fb->height : 0;
    s_health.last_format = fb ? (int)fb->format : -1;
    s_health.last_len = fb ? fb->len : 0;
    s_health.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_health_lock);
}

static void health_set_release(void)
{
    portENTER_CRITICAL(&s_health_lock);
    if (s_health.frames_in_flight > 0) {
        s_health.frames_in_flight--;
    }
    portEXIT_CRITICAL(&s_health_lock);
}

static void camera_power_on(void)
{
    /* IO_EXPANDER_PIN_NUM_5 low = camera enabled */
    Set_EXIO(IO_EXPANDER_PIN_NUM_5, false);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void camera_select_gpio_a(void)
{
    /* IO_EXPANDER_PIN_NUM_6 high = route camera through Tx/Rx pins */
    Set_EXIO(IO_EXPANDER_PIN_NUM_6, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t Camera_Driver_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!s_cam_mux) {
        s_cam_mux = xSemaphoreCreateMutex();
        if (!s_cam_mux) {
            ESP_LOGE(TAG, "Failed to create camera mutex");
            health_set_error(ESP_ERR_NO_MEM, "camera mutex create failed", false);
            return ESP_ERR_NO_MEM;
        }
    }

    bool psram_found = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;

    portENTER_CRITICAL(&s_health_lock);
    s_health.init_attempts++;
    s_health.initialized = false;
    s_health.init_err = ESP_OK;
    s_health.psram_found = psram_found;
    portEXIT_CRITICAL(&s_health_lock);
    health_set_config();

    camera_select_gpio_a();
    camera_power_on();

    ESP_LOGI(TAG, "PSRAM detected:       %s",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "YES" : "NO");
    ESP_LOGI(TAG, "PSRAM free/largest:   %u / %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Internal free/largest:%u / %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Camera pins: xclk=%d d0..d7=%d,%d,%d,%d,%d,%d,%d,%d vsync=%d href=%d pclk=%d",
             CAM_PIN_XCLK, CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
             CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
             CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK);
    ESP_LOGI(TAG, "Camera config: xclk=%d frame_size=%d pixel_format=%d jpeg_quality=%d fb_count=%d fb_location=%d grab_mode=%s",
             camera_config.xclk_freq_hz, (int)camera_config.frame_size,
             (int)camera_config.pixel_format, camera_config.jpeg_quality,
             (int)camera_config.fb_count, (int)camera_config.fb_location,
             grab_mode_name(camera_config.grab_mode));

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        portENTER_CRITICAL(&s_health_lock);
        s_health.init_err = err;
        strlcpy(s_health.last_error, esp_err_to_name(err), sizeof(s_health.last_error));
        portEXIT_CRITICAL(&s_health_lock);
        return err;
    }

    s_initialized = true;
    portENTER_CRITICAL(&s_health_lock);
    s_health.initialized = true;
    s_health.init_err = ESP_OK;
    s_health.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_health_lock);
    ESP_LOGI(TAG, "Camera initialised OK");
    return ESP_OK;
}

esp_err_t camera_capture_frame(camera_fb_t **fb)
{
    if (!fb) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb = NULL;

    if (s_paused_for_voice) {
        health_set_error(ESP_ERR_INVALID_STATE, "camera paused for voice", false);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_cam_mux) {
        health_set_error(ESP_ERR_INVALID_STATE, "camera mutex not ready", false);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cam_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        ESP_LOGW(TAG, "capture failed: camera mutex timeout");
        health_set_error(ESP_ERR_TIMEOUT, "camera mutex timeout", true);
        return ESP_ERR_TIMEOUT;
    }

    if (s_paused_for_voice) {
        xSemaphoreGive(s_cam_mux);
        health_set_error(ESP_ERR_INVALID_STATE, "camera paused for voice", false);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_initialized) {
        xSemaphoreGive(s_cam_mux);
        health_set_error(ESP_ERR_INVALID_STATE, "camera not initialized", false);
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        xSemaphoreGive(s_cam_mux);
        ESP_LOGE(TAG, "capture failed: esp_camera_fb_get returned NULL");
        health_set_error(ESP_FAIL, "esp_camera_fb_get failed", true);
        return ESP_FAIL;
    }

    health_set_capture_ok(frame);
    uint32_t ok_count;
    portENTER_CRITICAL(&s_health_lock);
    ok_count = s_health.capture_ok_count;
    portEXIT_CRITICAL(&s_health_lock);
    if (ok_count <= 3 || (ok_count % 30) == 0) {
        ESP_LOGI(TAG, "capture OK #%u: %dx%d fmt=%d len=%zu",
                 (unsigned)ok_count, (int)frame->width, (int)frame->height,
                 (int)frame->format, frame->len);
    }

    *fb = frame;
    return ESP_OK;
}

void camera_release_frame(camera_fb_t *fb)
{
    if (!fb) {
        ESP_LOGW(TAG, "camera_release_frame called with NULL");
        return;
    }
    esp_camera_fb_return(fb);
    health_set_release();
    if (s_cam_mux) {
        xSemaphoreGive(s_cam_mux);
    }
}

void camera_get_health(camera_health_t *out)
{
    if (!out) {
        return;
    }

    portENTER_CRITICAL(&s_health_lock);
    *out = s_health;
    portEXIT_CRITICAL(&s_health_lock);

    out->psram_found = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

void camera_pause_for_voice(void)
{
    s_paused_for_voice = true;
    if (s_cam_mux && xSemaphoreTake(s_cam_mux, pdMS_TO_TICKS(3000)) == pdTRUE) {
        xSemaphoreGive(s_cam_mux);
    }
    ESP_LOGI(TAG, "camera paused for voice");
}

void camera_resume_after_voice(void)
{
    s_paused_for_voice = false;
    ESP_LOGI(TAG, "camera resumed after voice");
}
