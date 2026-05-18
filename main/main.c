#include "audio_driver.h"
#include "bsp_board.h"
#include "button_driver.h"
#include "lcd_driver.h"
#include "led_effects.h"
#include "lvgl_driver.h"
#include "robot_chat.h"
#include "robot_face.h"
#include "robot_state.h"
#include "robot_voice.h"
#include "tca9555_driver.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_ROBOT_ENABLE_VOICE_INPUT
#define CONFIG_ROBOT_ENABLE_VOICE_INPUT 0
#endif

static const char *TAG = "app_main";

static void robot_audio_state_cb(esp_asp_state_t state)
{
    robot_chat_audio_state_changed(state);
    if (state == ESP_ASP_STATE_FINISHED) {
        robot_set_idle();
    } else if (state == ESP_ASP_STATE_ERROR) {
        Audio_PA_Mute();
        robot_set_error("Nem sikerult lejatszani");
        vTaskDelay(pdMS_TO_TICKS(300));
        robot_set_idle();
    }
}

static void on_key_press(key_id_t key_id, key_event_t event, void *user_data)
{
    (void)user_data;
    if (event != KEY_EVENT_SHORT_PRESS) {
        return;
    }

    if (key_id == KEY_ID_9) {
        uint8_t v = get_audio_volume();
        Volume_Adjustment(v >= 10 ? v - 10 : 0);
        ESP_LOGI(TAG, "volume down: %u -> %u", v, get_audio_volume());
        return;
    }

    if (key_id == KEY_ID_10) {
        uint8_t v = get_audio_volume();
        Volume_Adjustment(v <= 90 ? v + 10 : 100);
        ESP_LOGI(TAG, "volume up: %u -> %u", v, get_audio_volume());
        return;
    }

#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
    if (key_id == KEY_ID_11) {
        char error[128] = {0};
        esp_err_t err = robot_voice_start_push_to_talk_ex(error, sizeof(error));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "voice button ignored: %s", error[0] ? error : esp_err_to_name(err));
        }
        return;
    }
#endif
}

static void startup_ready_task(void *arg)
{
    (void)arg;
    lvgl_port_lock(0);
    robot_face_set_text("Kesz!");
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(900));
    robot_set_idle();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Robot Assistant MVP starting...");

    ESP_ERROR_CHECK(esp_board_init(48000, 2, 16));

    if (led_effects_init() != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed, continuing without LEDs");
    }

    tca9555_driver_init();
    key_register_callback(on_key_press);
    key_module_init(NULL);

    ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));

    Audio_Play_Init();
    Audio_Set_State_Callback(robot_audio_state_cb);
    Volume_Adjustment(65);

    ESP_ERROR_CHECK(lcd_driver_init());
    ESP_ERROR_CHECK(lvgl_driver_init());

    lvgl_port_lock(0);
    robot_face_create();
    robot_face_set_text("Indulok...");
    lvgl_port_unlock();

#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
    ESP_ERROR_CHECK(robot_chat_init());
#endif

#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
    esp_err_t voice_err = robot_voice_init();
    if (voice_err != ESP_OK) {
        ESP_LOGW(TAG, "voice input disabled: %s", esp_err_to_name(voice_err));
    }
#else
    ESP_LOGI(TAG, "voice input disabled by CONFIG_ROBOT_ENABLE_VOICE_INPUT");
#endif

    bool web_server_ready = false;
    if (wifi_manager_start() == ESP_OK) {
#if CONFIG_ROBOT_ENABLE_KIDS_CHAT
        const char *backend_base_url = wifi_manager_backend_base_url();
        if (backend_base_url && backend_base_url[0]) {
            esp_err_t backend_err = robot_chat_set_backend_base_url(backend_base_url);
            if (backend_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to apply WiFi backend URL: %s", esp_err_to_name(backend_err));
            }
        }
#endif
        if (web_server_start() == ESP_OK) {
            web_server_ready = true;
            ESP_LOGI(TAG, "Web server ready");
        } else {
            ESP_LOGE(TAG, "Web server failed to start");
        }
    } else {
        ESP_LOGW(TAG, "WiFi not connected, web server skipped");
    }

    xTaskCreate(startup_ready_task, "startup_ready", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "=== Startup complete ===");
    ESP_LOGI(TAG, "  SD card    : /sdcard");
    ESP_LOGI(TAG, "  Audio      : local file playback ready");
    ESP_LOGI(TAG, "  LCD + LVGL : ready");
    ESP_LOGI(TAG, "  Chat       : %s", robot_chat_enabled() ? robot_chat_backend_url() : "disabled");
    ESP_LOGI(TAG, "  Web server : %s", web_server_ready ? "http://<ip>:80" : "not started");
#if CONFIG_ROBOT_ENABLE_VOICE_INPUT
    ESP_LOGI(TAG, "  Voice input: enabled");
#else
    ESP_LOGI(TAG, "  Voice input: disabled");
#endif
    ESP_LOGI(TAG, "========================");
}
