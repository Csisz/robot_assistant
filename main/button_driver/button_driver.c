#include "button_driver.h"
#include "tca9555_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button_driver";

#define SCAN_INTERVAL_MS   100
#define LONG_PRESS_MS     1000

typedef struct {
    bool is_pressed;
    bool last_pressed;
    uint64_t press_start;
    bool long_triggered;
} key_state_t;

static key_state_t  s_keys[3]    = {0};
static key_callback_t s_callback = NULL;
static void         *s_user_data = NULL;

static bool get_key_state(uint32_t mask, key_id_t id)
{
    /* Keys are active-low: bit clear means pressed */
    return !(mask & (1ULL << id));
}

static void button_scan_task(void *arg)
{
    while (1) {
        uint32_t mask = 0;
        esp_err_t ret = esp_io_expander_get_level(
            io_expander,
            (1ULL << KEY_ID_9) | (1ULL << KEY_ID_10) | (1ULL << KEY_ID_11),
            &mask
        );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "io_expander read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
            continue;
        }

        for (int i = 0; i < 3; i++) {
            key_id_t   id  = (key_id_t)(KEY_ID_9 + i);
            key_state_t *k = &s_keys[i];

            k->is_pressed = get_key_state(mask, id);
            uint64_t now  = esp_timer_get_time() / 1000;

            if (k->is_pressed && !k->last_pressed) {
                k->press_start    = now;
                k->long_triggered = false;
            } else if (!k->is_pressed && k->last_pressed) {
                uint32_t dur = (uint32_t)(now - k->press_start);
                if (dur < LONG_PRESS_MS && !k->long_triggered && s_callback) {
                    s_callback(id, KEY_EVENT_SHORT_PRESS, s_user_data);
                }
            } else if (k->is_pressed && !k->long_triggered) {
                if ((now - k->press_start) >= LONG_PRESS_MS && s_callback) {
                    s_callback(id, KEY_EVENT_LONG_PRESS, s_user_data);
                    k->long_triggered = true;
                }
            }

            k->last_pressed = k->is_pressed;
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

esp_err_t key_module_init(void *user_data)
{
    s_user_data = user_data;
    for (int i = 0; i < 3; i++) {
        s_keys[i] = (key_state_t){0};
    }
    xTaskCreatePinnedToCore(button_scan_task, "btn_scan", 4096, NULL, 3, NULL, 0);
    return ESP_OK;
}

esp_err_t key_register_callback(key_callback_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    s_callback = callback;
    return ESP_OK;
}

esp_err_t key_module_deinit(void)
{
    s_callback = NULL;
    return ESP_OK;
}
