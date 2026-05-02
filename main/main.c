#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "bsp_board.h"
#include "tca9555_driver.h"
#include "audio_driver.h"
#include "lcd_driver.h"
#include "lvgl_driver.h"
#include "robot_face.h"
#include "esp_lvgl_port.h"

static const char *TAG = "app_main";

static void robot_demo_task(void *arg)
{
    /* Short delay to let the LVGL task settle */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Announce and start speaking animation */
    lvgl_port_lock(0);
    robot_face_set_text("Szia Zita!");
    robot_face_set_speaking(true);
    lvgl_port_unlock();

    /* Play the greeting from SD card */
    Audio_Play_Music("file://sdcard/ZITA.MP3");

    /* Keep speaking animation while audio plays (~5 s) */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Return to idle */
    lvgl_port_lock(0);
    robot_face_set_speaking(false);
    robot_face_set_text("Varok...");
    lvgl_port_unlock();

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Robot Assistant starting...");

    /* 1. Board init (I2C, I2S, codecs) */
    ESP_ERROR_CHECK(esp_board_init(48000, 2, 16));

    /* 2. IO expander (TCA9555) – required for LCD reset and PA enable */
    tca9555_driver_init();

    /* 3. Mount SD card */
    ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));

    /* 4. Audio pipeline */
    Audio_Play_Init();
    Volume_Adjustment(60);

    /* 5. LCD hardware init */
    ESP_ERROR_CHECK(lcd_driver_init());

    /* 6. LVGL port init */
    ESP_ERROR_CHECK(lvgl_driver_init());

    /* 7. Create robot face UI under LVGL lock */
    lvgl_port_lock(0);
    robot_face_create();
    robot_face_set_text("Szia Zita!");
    lvgl_port_unlock();

    /* 8. Start demo task */
    xTaskCreate(robot_demo_task, "robot_demo", 4096, NULL, 5, NULL);
}
