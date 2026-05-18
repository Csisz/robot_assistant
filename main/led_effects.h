#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_IDLE = 0,
    LED_THINKING,
    LED_SPEAKING,
    LED_ERROR,
} led_state_t;

esp_err_t   led_effects_init(void);
void        led_set_state(led_state_t state);
led_state_t led_get_state(void);
const char *led_state_name(led_state_t state);
void        led_test_force(led_state_t state, int ticks);

#ifdef __cplusplus
}
#endif
