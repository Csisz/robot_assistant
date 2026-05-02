#include "lvgl_driver.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lcd_driver.h"
#include "bsp_board.h"

static const char *TAG = "lvgl driver";

static lv_display_t *lvgl_disp = NULL;

esp_err_t lvgl_driver_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority   = 3,
        .task_stack      = 8196,
        .task_affinity   = -1,
        .task_max_sleep_ms = 1000,
        .timer_period_ms = 10,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /*
     * Keep the LVGL draw buffer small and in internal DMA-capable RAM.
     * The previous buffer was too large and tried to allocate from PSRAM,
     * which caused: Not enough memory for LVGL buffer allocation.
     */
#define LVGL_BUF_PIXELS (EXAMPLE_LCD_H_RES * 20)

    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = lcd_io,
        .panel_handle  = lcd_panel,
        .buffer_size   = LVGL_BUF_PIXELS,
        .double_buffer = false,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,

#if defined(CONFIG_EXAMPLE_DISPLAY_ROTATION_0_DEGREE)
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#elif defined(CONFIG_EXAMPLE_DISPLAY_ROTATION_90_DEGREE)
        .hres = EXAMPLE_LCD_V_RES,
        .vres = EXAMPLE_LCD_H_RES,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
#elif defined(CONFIG_EXAMPLE_DISPLAY_ROTATION_180_DEGREE)
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = true,
            .mirror_y = true,
        },
#elif defined(CONFIG_EXAMPLE_DISPLAY_ROTATION_270_DEGREE)
        .hres = EXAMPLE_LCD_V_RES,
        .vres = EXAMPLE_LCD_H_RES,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = false,
            .mirror_y = true,
        },
#else
        /* fallback: 90 degrees (landscape) */
        .hres = EXAMPLE_LCD_V_RES,
        .vres = EXAMPLE_LCD_H_RES,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
#endif

        .flags = {
            .buff_dma    = true,
            .swap_bytes  = true,
            .buff_spiram = false,
        },
    };
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (!lvgl_disp) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }

    return ESP_OK;
}
