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
#include "robot_state.h"
#include "esp_lvgl_port.h"
#include "button_driver.h"

static const char *TAG = "app_main";

static void robot_audio_state_cb(esp_asp_state_t state)
{
    if (state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_ERROR) {
        lvgl_port_lock(0);
        robot_face_set_speaking(false);
        robot_face_set_text("Varok...");
        lvgl_port_unlock();
    }
}

static void on_key_press(key_id_t key_id, key_event_t event, void *user_data)
{
    if (event != KEY_EVENT_SHORT_PRESS) return;

    switch (key_id) {
        case KEY_ID_9:
            robot_say_file("Szia Zita!", "file://sdcard/ZITA.MP3");
            break;
        case KEY_ID_10:
            robot_say_file("Szia Ida!", "file://sdcard/IDA.MP3");
            break;
        case KEY_ID_11:
            robot_say_file("Szia Zsoli!", "file://sdcard/ZSOLI.MP3");
            break;
        default:
            break;
    }
}

static void robot_demo_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    robot_say_file("Szia Zita!", "file://sdcard/ZITA.MP3");
    /* Fallback: ensure idle state even if callback fires late */
    vTaskDelay(pdMS_TO_TICKS(10000));
    robot_set_idle();

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
    key_register_callback(on_key_press);
    key_module_init(NULL);

    /* 3. Mount SD card */
    ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));

    /* 4. Audio pipeline */
    Audio_Play_Init();
    Audio_Set_State_Callback(robot_audio_state_cb);
    Volume_Adjustment(60);

    /* 5. LCD hardware init */
    ESP_ERROR_CHECK(lcd_driver_init());

    /* 6. LVGL port init */
    ESP_ERROR_CHECK(lvgl_driver_init());

    /* 7. Create robot face UI */
    lvgl_port_lock(0);
    robot_face_create();
    lvgl_port_unlock();

    /* 8. Start demo task */
    xTaskCreate(robot_demo_task, "robot_demo", 4096, NULL, 5, NULL);
}
