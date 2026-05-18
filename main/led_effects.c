#include "led_effects.h"

#include "bsp_board.h"
#include "robot_state.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define TAG "led_fx"

static led_strip_handle_t s_strip;
static volatile led_state_t s_requested = LED_IDLE;
static volatile led_state_t s_active = LED_IDLE;
static volatile led_state_t s_force_state = LED_IDLE;
static volatile int s_force_ticks;

static uint8_t triangle(uint32_t tick, uint32_t period, uint8_t max_val)
{
    if (period == 0) {
        return max_val;
    }
    uint32_t phase = tick % period;
    uint32_t half = period / 2;
    if (phase < half) {
        return (uint8_t)((uint32_t)max_val * 2U * phase / period);
    }
    return (uint8_t)((uint32_t)max_val * 2U * (period - phase) / period);
}

static void hsv_to_rgb(uint16_t h, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (v == 0) {
        *r = *g = *b = 0;
        return;
    }
    uint16_t region = h / 60U;
    uint8_t rem = (uint8_t)((h % 60U) * 255U / 60U);
    uint8_t q = (uint8_t)((uint32_t)v * (255U - rem) / 255U);
    uint8_t t = (uint8_t)((uint32_t)v * rem / 255U);
    switch (region) {
        case 0: *r = v; *g = t; *b = 0; break;
        case 1: *r = q; *g = v; *b = 0; break;
        case 2: *r = 0; *g = v; *b = t; break;
        case 3: *r = 0; *g = q; *b = v; break;
        case 4: *r = t; *g = 0; *b = v; break;
        default: *r = v; *g = 0; *b = q; break;
    }
}

static led_state_t state_from_robot(void)
{
    switch (robot_get_state()) {
        case ROBOT_THINKING: return LED_THINKING;
        case ROBOT_SPEAKING: return LED_SPEAKING;
        case ROBOT_ERROR: return LED_ERROR;
        case ROBOT_IDLE:
        default: return LED_IDLE;
    }
}

static void render(led_state_t state, uint32_t tick)
{
    switch (state) {
        case LED_IDLE: {
            uint8_t bri = triangle(tick, 40, 45);
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, 0, bri >> 2, bri);
            }
            break;
        }
        case LED_THINKING: {
            uint8_t bri = triangle(tick, 16, 80);
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, bri, 55, 0);
            }
            break;
        }
        case LED_SPEAKING: {
            uint8_t r, g, b;
            uint16_t base_hue = (uint16_t)((tick * 9U) % 360U);
            uint16_t step = 360U / LED_STRIP_LED_COUNT;
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                hsv_to_rgb((base_hue + (uint16_t)i * step) % 360U, 70, &r, &g, &b);
                led_strip_set_pixel(s_strip, i, r, g, b);
            }
            break;
        }
        case LED_ERROR: {
            uint8_t on = (tick % 4U < 2U) ? 100 : 0;
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, on, 0, 0);
            }
            break;
        }
    }
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    while (1) {
        if (s_force_ticks > 0) {
            s_active = s_force_state;
            s_force_ticks--;
        } else if (s_requested != LED_IDLE) {
            s_active = s_requested;
            s_requested = LED_IDLE;
        } else {
            s_active = state_from_robot();
        }
        render(s_active, tick++);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t led_effects_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {.with_dma = false},
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);

    if (xTaskCreate(led_task, "led_fx", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "led_task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LED strip ready: GPIO %d, %d LEDs", LED_STRIP_GPIO_PIN, LED_STRIP_LED_COUNT);
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

void led_test_force(led_state_t state, int ticks)
{
    s_force_state = state;
    s_force_ticks = ticks;
}

const char *led_state_name(led_state_t state)
{
    switch (state) {
        case LED_IDLE: return "IDLE";
        case LED_THINKING: return "THINKING";
        case LED_SPEAKING: return "SPEAKING";
        case LED_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
