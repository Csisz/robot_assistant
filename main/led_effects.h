#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_IDLE = 0,       /* slow breathing, soft blue   */
    LED_LISTENING,      /* blue/cyan pulse, 1 s period */
    LED_SPEAKING,       /* colorful rainbow chase      */
    LED_FACE_DETECTED,  /* brief yellow pulse (one-shot, ~300 ms) */
    LED_RECOGNIZED,     /* brief green flash (one-shot, ~500 ms)  */
    LED_ERROR,          /* red blink (one-shot, ~800 ms)          */
} led_state_t;

/* Create the RMT-backed LED strip and start the LED effect task. */
esp_err_t   led_effects_init(void);

/* Trigger a one-shot effect (FACE_DETECTED / RECOGNIZED / ERROR).
   For continuous states the LED task derives them from robot_get_state(). */
void        led_set_state(led_state_t state);

/* Effective LED state currently showing (set by the LED task). */
led_state_t led_get_state(void);

/* Human-readable name of a LED state. */
const char *led_state_name(led_state_t state);

/* Force a specific LED pattern for approximately ticks × 50 ms.
   Takes priority over both one-shots and continuous state — useful
   for hardware testing via the web UI without changing robot state. */
void led_test_force(led_state_t state, int ticks);

#ifdef __cplusplus
}
#endif
