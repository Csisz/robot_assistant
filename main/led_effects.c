/*
 * led_effects.c — WS2812 LED strip animations
 *
 * Hardware: GPIO 38, 7 LEDs (LED_STRIP_GPIO_PIN / LED_STRIP_LED_COUNT from bsp_board.h)
 *
 * Continuous states are derived every tick from robot_get_state():
 *   ROBOT_SPEAKING  → LED_SPEAKING  (rainbow chase)
 *   ROBOT_LISTENING → LED_LISTENING (cyan pulse)
 *   anything else   → LED_IDLE      (soft blue breathing)
 *
 * One-shot states (FACE_DETECTED, RECOGNIZED, ERROR) are triggered via
 * led_set_state() and play for a fixed number of ticks before reverting
 * to the derived continuous state — even if robot state changed mid-flight.
 *
 * Update rate: 50 ms per tick (20 Hz).  Task priority 1 so it never
 * blocks audio, camera, or face detection.
 */

#include "led_effects.h"
#include "bsp_board.h"      /* LED_STRIP_GPIO_PIN, LED_STRIP_LED_COUNT */
#include "robot_state.h"    /* robot_get_state() */

#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "led_fx"

/* ---------- timing (ticks at 50 ms each) --------------------------------- */
#define TICKS_IDLE_PERIOD      40   /* 2 s breathing period */
#define TICKS_LISTEN_PERIOD    20   /* 1 s breathing period */
#define TICKS_FACE_DETECTED     6   /* 300 ms one-shot */
#define TICKS_RECOGNIZED       10   /* 500 ms one-shot */
#define TICKS_ERROR            16   /* 800 ms one-shot */

/* ---------- hardware ------------------------------------------------------ */
static led_strip_handle_t s_strip = NULL;

/* ---------- shared state -------------------------------------------------- */
/* Written by any task via led_set_state(); read + cleared by LED task. */
static volatile led_state_t s_requested = LED_IDLE;
/* Written exclusively by the LED task; read by led_get_state(). */
static volatile led_state_t s_active    = LED_IDLE;

/* ---------- colour helpers ------------------------------------------------ */

/* Triangle-wave brightness: 0 → max_val → 0 over one 'period' cycle. */
static uint8_t triangle(uint32_t tick, uint32_t period, uint8_t max_val)
{
    if (period == 0) return max_val;
    uint32_t phase = tick % period;
    uint32_t half  = period / 2;
    if (phase < half) {
        return (uint8_t)((uint32_t)max_val * 2 * phase / period);
    }
    return (uint8_t)((uint32_t)max_val * 2 * (period - phase) / period);
}

/* Full-saturation HSV → RGB.  h: 0-359, v: 0-255. */
static void hsv_to_rgb(uint16_t h, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (v == 0) { *r = *g = *b = 0; return; }
    uint16_t region = h / 60;
    uint8_t  rem    = (uint8_t)((h % 60) * 255u / 60);   /* 0..255 */
    uint8_t  q      = (uint8_t)((uint32_t)v * (255u - rem) / 255u);
    uint8_t  t      = (uint8_t)((uint32_t)v * rem / 255u);
    switch (region) {
        case 0:  *r = v; *g = t; *b = 0; break;
        case 1:  *r = q; *g = v; *b = 0; break;
        case 2:  *r = 0; *g = v; *b = t; break;
        case 3:  *r = 0; *g = q; *b = v; break;
        case 4:  *r = t; *g = 0; *b = v; break;
        default: *r = v; *g = 0; *b = q; break;
    }
}

/* ---------- renderer ------------------------------------------------------ */
static void render(led_state_t state, uint32_t tick)
{
    uint8_t r, g, b;

    switch (state) {

        case LED_IDLE: {
            uint8_t bri = triangle(tick, TICKS_IDLE_PERIOD, 50);
            /* soft blue — R=0, G=bri/4, B=bri */
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, 0, bri >> 2, bri);
            }
            break;
        }

        case LED_LISTENING: {
            uint8_t bri = triangle(tick, TICKS_LISTEN_PERIOD, 70);
            /* cyan — R=0, G=bri, B=bri */
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, 0, bri, bri);
            }
            break;
        }

        case LED_SPEAKING: {
            /* rainbow chase: each LED offset by 360/LED_COUNT degrees,
               base hue rotates 9 degrees per tick → 1 revolution in ~2 s */
            uint16_t base_hue = (uint16_t)((tick * 9u) % 360u);
            uint16_t step     = 360u / LED_STRIP_LED_COUNT;
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                uint16_t h = (base_hue + (uint16_t)i * step) % 360u;
                hsv_to_rgb(h, 70, &r, &g, &b);
                led_strip_set_pixel(s_strip, i, r, g, b);
            }
            break;
        }

        case LED_FACE_DETECTED:
            /* warm yellow flash */
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, 100, 80, 0);
            }
            break;

        case LED_RECOGNIZED:
            /* bright green */
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, 0, 100, 10);
            }
            break;

        case LED_ERROR: {
            /* blink red: on for 2 ticks, off for 2 ticks (200 ms each) */
            uint8_t on = (tick % 4 < 2) ? 100 : 0;
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, on, 0, 0);
            }
            break;
        }
    }

    led_strip_refresh(s_strip);
}

/* ---------- LED task ------------------------------------------------------ */
static void led_task(void *arg)
{
    uint32_t tick     = 0;
    int      one_shot = 0;  /* remaining ticks for current one-shot */

    while (1) {
        led_state_t req = s_requested;

        if (one_shot > 0) {
            /* mid-flight one-shot: keep current s_active, decrement counter */
            one_shot--;
        } else if (req == LED_FACE_DETECTED || req == LED_RECOGNIZED || req == LED_ERROR) {
            /* new one-shot request */
            s_active    = req;
            s_requested = LED_IDLE;   /* consume the request */
            switch (req) {
                case LED_FACE_DETECTED: one_shot = TICKS_FACE_DETECTED - 1; break;
                case LED_RECOGNIZED:    one_shot = TICKS_RECOGNIZED    - 1; break;
                case LED_ERROR:         one_shot = TICKS_ERROR         - 1; break;
                default: break;
            }
        } else {
            /* continuous state: derive from robot state machine */
            robot_state_t rs = robot_get_state();
            if      (rs == ROBOT_SPEAKING)  s_active = LED_SPEAKING;
            else if (rs == ROBOT_LISTENING) s_active = LED_LISTENING;
            else                            s_active = LED_IDLE;
        }

        render(s_active, tick);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- public API ---------------------------------------------------- */

esp_err_t led_effects_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num        = LED_STRIP_GPIO_PIN,
        .max_leds              = LED_STRIP_LED_COUNT,
        .led_model             = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags                 = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz */
        .flags         = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }

    led_strip_clear(s_strip);

    BaseType_t ok = xTaskCreate(led_task, "led_fx", 2048, NULL, 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "led_task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED strip ready: GPIO %d, %d LEDs",
             LED_STRIP_GPIO_PIN, LED_STRIP_LED_COUNT);
    return ESP_OK;
}

void led_set_state(led_state_t state)
{
    s_requested = state;
}

led_state_t led_get_state(void)
{
    return s_active;
}

const char *led_state_name(led_state_t state)
{
    switch (state) {
        case LED_IDLE:          return "IDLE";
        case LED_LISTENING:     return "LISTENING";
        case LED_SPEAKING:      return "SPEAKING";
        case LED_FACE_DETECTED: return "FACE_DETECTED";
        case LED_RECOGNIZED:    return "RECOGNIZED";
        case LED_ERROR:         return "ERROR";
        default:                return "UNKNOWN";
    }
}
