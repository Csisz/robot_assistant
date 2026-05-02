#pragma once

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEY_ID_9  = 9,
    KEY_ID_10 = 10,
    KEY_ID_11 = 11,
} key_id_t;

typedef enum {
    KEY_EVENT_SHORT_PRESS,
    KEY_EVENT_LONG_PRESS,
} key_event_t;

typedef void (*key_callback_t)(key_id_t key_id, key_event_t event, void *user_data);

esp_err_t key_module_init(void *user_data);
esp_err_t key_register_callback(key_callback_t callback);
esp_err_t key_module_deinit(void);

#ifdef __cplusplus
}
#endif
