#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Camera data bus pins */
#define CAM_PIN_PWDN  -1   /* no hardware power-down */
#define CAM_PIN_RESET -1   /* software reset */
#define CAM_PIN_XCLK  43

/* SCCB / I2C: -1 = reuse the already-initialised I2C_NUM_0 bus */
#define CAM_PIN_SIOD  -1
#define CAM_PIN_SIOC  -1

#define CAM_PIN_D7    48
#define CAM_PIN_D6    47
#define CAM_PIN_D5    46
#define CAM_PIN_D4    45
#define CAM_PIN_D3    39
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0     2
#define CAM_PIN_VSYNC 21
#define CAM_PIN_HREF   1
#define CAM_PIN_PCLK  44

/*
 * Use LEDC_TIMER_1 / LEDC_CHANNEL_1 for camera XCLK.
 * LEDC_TIMER_0 / LEDC_CHANNEL_0 is already taken by the LCD backlight.
 */
#define CAM_LEDC_TIMER   LEDC_TIMER_1
#define CAM_LEDC_CHANNEL LEDC_CHANNEL_1

esp_err_t Camera_Driver_Init(void);

typedef struct {
    bool initialized;
    bool psram_found;
    esp_err_t init_err;
    uint32_t init_attempts;
    uint32_t capture_ok_count;
    uint32_t capture_fail_count;
    uint32_t capture_timeout_count;
    uint32_t frames_in_flight;
    int last_width;
    int last_height;
    int last_format;
    size_t last_len;
    int64_t last_success_us;
    int64_t last_fail_us;
    char last_error[80];
    size_t psram_free;
    size_t psram_largest_free;
    size_t internal_free;
    size_t internal_largest_free;
    int xclk_freq_hz;
    int pixel_format;
    int frame_size;
    int jpeg_quality;
    int fb_count;
    int fb_location;
    int grab_mode;
} camera_health_t;

/* Shared camera access.
 * camera_capture_frame() acquires the camera mutex; camera_release_frame()
 * returns the frame and releases it. Every successful capture must be paired
 * with exactly one release. */
esp_err_t camera_capture_frame(camera_fb_t **fb);
void camera_release_frame(camera_fb_t *fb);
void camera_get_health(camera_health_t *out);
void camera_pause_for_voice(void);
void camera_resume_after_voice(void);

#ifdef __cplusplus
}
#endif
