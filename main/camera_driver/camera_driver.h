#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif
