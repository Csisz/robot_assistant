#include "camera_driver.h"
#include "tca9555_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "camera_driver";

static bool s_initialized = false;

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
    .frame_size    = FRAMESIZE_QQVGA,   /* 160x120 x 2 B = 38,400 B in PSRAM */
    .jpeg_quality  = 12,
    .fb_count      = 1,
    .fb_location   = CAMERA_FB_IN_PSRAM,
    .grab_mode     = CAMERA_GRAB_WHEN_EMPTY,
};

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

    camera_select_gpio_a();
    camera_power_on();

    ESP_LOGI(TAG, "SPIRAM free:          %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "SPIRAM largest block: %u B", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Internal free:        %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Camera config: frame_size=%d pixel_format=%d fb_count=%d fb_location=%d",
             camera_config.frame_size, (int)camera_config.pixel_format,
             camera_config.fb_count, (int)camera_config.fb_location);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Camera initialised OK");
    return ESP_OK;
}
