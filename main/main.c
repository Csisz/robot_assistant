#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>

#include "bsp_board.h"
#include "tca9555_driver.h"
#include "audio_driver.h"
#include "lcd_driver.h"
#include "lvgl_driver.h"
#include "robot_face.h"
#include "robot_state.h"
#include "esp_lvgl_port.h"
#include "button_driver.h"
#include "camera_driver.h"
#include "wifi_manager.h"
#include "camera_web_server.h"
#include "face_detect.h"
#include "face_enroll.h"
#include "face_status.h"
#include "face_recognition.h"
#include "led_effects.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "app_main";

/* Set by face_detect_task once the model is loaded; robot_demo_task waits on
   this before playing the startup greeting to avoid SD-card DMA contention. */
static volatile bool s_face_detect_ready = false;

/* ------------------------------------------------------------------ */
/* Greeting cooldown — 20 s per recognized person                     */
/* ------------------------------------------------------------------ */

#define GREET_COOLDOWN_US  (20LL * 1000000LL)
#define MAX_CD_PERSONS     16

static struct {
    char    person_id[16];
    int64_t last_greet_us;
} s_greet_cd[MAX_CD_PERSONS];
static int s_greet_cd_n = 0;

static bool greet_ok(const char *person_id)
{
    if (!person_id || person_id[0] == '\0') return false;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_greet_cd_n; i++) {
        if (strcmp(s_greet_cd[i].person_id, person_id) == 0) {
            if (now - s_greet_cd[i].last_greet_us < GREET_COOLDOWN_US) {
                return false;   /* still in cooldown */
            }
            s_greet_cd[i].last_greet_us = now;
            return true;
        }
    }
    if (s_greet_cd_n < MAX_CD_PERSONS) {
        strlcpy(s_greet_cd[s_greet_cd_n].person_id, person_id, 16);
        s_greet_cd[s_greet_cd_n].last_greet_us = now;
        s_greet_cd_n++;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Audio state callback                                                */
/* ------------------------------------------------------------------ */

static void robot_audio_state_cb(esp_asp_state_t state)
{
    if (state == ESP_ASP_STATE_FINISHED) {
        robot_set_idle();
    } else if (state == ESP_ASP_STATE_ERROR) {
        /* Kill buzzing immediately — Audio_PA_Mute is safe from callback
           context (only controls the I2C IO expander, does not touch the
           audio pipeline which is already stopped). */
        Audio_PA_Mute();
        robot_set_idle();
    }
}

/* ------------------------------------------------------------------ */
/* Camera health check                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t robot_camera_test(void)
{
    esp_err_t err = Camera_Driver_Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        lvgl_port_lock(0);
        robot_face_set_text("Nem latok kamerat");
        lvgl_port_unlock();
        return err;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        lvgl_port_lock(0);
        robot_face_set_text("Nem latok kamerat");
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera frame: %dx%d fmt=%d len=%zu", fb->width, fb->height, (int)fb->format, fb->len);
    esp_camera_fb_return(fb);

    lvgl_port_lock(0);
    robot_face_set_text("Latlak!");
    lvgl_port_unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Button handler                                                      */
/* ------------------------------------------------------------------ */

static void on_key_press(key_id_t key_id, key_event_t event, void *user_data)
{
    if (event != KEY_EVENT_SHORT_PRESS) return;

    const char *url = NULL;
    const char *text = NULL;

    switch (key_id) {
        case KEY_ID_9:  text = "Szia Zita!";  url = "file://sdcard/ZITA.MP3";  break;
        case KEY_ID_10: text = "Szia Ida!";   url = "file://sdcard/IDA.MP3";   break;
        case KEY_ID_11: text = "Szia Zsoli!"; url = "file://sdcard/ZSOLI.MP3"; break;
        default: break;
    }

    if (url) {
        face_status_set_last_audio(url);
        robot_say_file(text, url);
    }
}

/* ------------------------------------------------------------------ */
/* Face detection task                                                 */
/* ------------------------------------------------------------------ */

static void face_detect_task(void *arg)
{
    /* Allow camera + web server to fully settle before the first inference */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (face_detect_init() != ESP_OK) {
        ESP_LOGE(TAG, "Face detector unavailable — task exiting");
        /* Signal demo task so it does not wait indefinitely */
        s_face_detect_ready = true;
        vTaskDelete(NULL);
        return;
    }

    /* Init recognition after detector (stubs: instant; real impl: loads DB features) */
    face_recog_init();
    face_status_set_recognition_available(face_recog_available());

    /* Unblock the demo task — model is loaded, SD-card reads are now safe */
    s_face_detect_ready = true;

    bool prev_face = false;

    while (1) {
        /* Skip inference while audio is playing (SDMMC DMA contention) or
           while an enrollment capture is in progress (avoid concurrent inference). */
        bool audio_running = (Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING);
        if (robot_get_state() == ROBOT_SPEAKING || audio_running) {
            ESP_LOGI(TAG, "face_detect: skipped while speaking");
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }
        if (face_enroll_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }

        /* --- grab frame ------------------------------------------------ */
        camera_fb_t *fb = NULL;
        if (camera_capture_frame(&fb) != ESP_OK || !fb) {
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }

        /* Copy RGB565 data to PSRAM so the camera buffer is freed quickly */
        size_t    data_len   = fb->len;
        int       frame_w    = fb->width;
        int       frame_h    = fb->height;
        uint16_t *frame_copy = (uint16_t *)heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (frame_copy) {
            memcpy(frame_copy, fb->buf, data_len);
        }
        camera_release_frame(fb);   /* release mutex before slow inference */

        if (!frame_copy) {
            ESP_LOGW(TAG, "PSRAM alloc failed (%zu B) — skipping frame", data_len);
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }

        /* Second enrollment check: enrollment may have started while we held
           the camera mutex.  Skip inference to avoid concurrent detector use. */
        if (face_enroll_is_active()) {
            free(frame_copy);
            vTaskDelay(pdMS_TO_TICKS(2500));
            continue;
        }

        /* --- detect + recognise ---------------------------------------- */
        ESP_LOGD(TAG, "SPIRAM free before detect: %u B  Internal: %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        int bbox[4]  = {0};
        int kpts[10] = {0};
        int face_count = face_detect_run_full(frame_copy, frame_w, frame_h, bbox, kpts);
        bool face = (face_count > 0);

        ESP_LOGD(TAG, "SPIRAM free after  detect: %u B  Internal: %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        /* --- update state transitions first, then run recognition ------ */
        if (face != prev_face) {
            prev_face = face;
            if (face) {
                face_status_set_detected();
                led_set_state(LED_FACE_DETECTED);   /* brief yellow pulse */
                if (robot_get_state() == ROBOT_IDLE) {
                    lvgl_port_lock(0);
                    robot_face_set_text("Latlak!");
                    lvgl_port_unlock();
                }
            } else {
                face_status_set_no_face();
                ESP_LOGI(TAG, "No face");
                if (robot_get_state() == ROBOT_IDLE) {
                    lvgl_port_lock(0);
                    robot_face_set_text("Várok...");
                    lvgl_port_unlock();
                }
            }
        }

        /* --- recognition on every face-present frame ------------------- */
        if (face) {
            face_recog_result_t recog = {0};
            face_recog_run(frame_copy, frame_w, frame_h, bbox, kpts, &recog);
            face_status_set_recognition_result(recog.recognized,
                recog.person_id, recog.display_name, recog.audio_file,
                recog.confidence);

            /* Greeting: only when recognized, not already speaking, cooldown OK */
            if (recog.recognized &&
                robot_get_state() != ROBOT_SPEAKING &&
                recog.audio_file[0] != '\0' &&
                greet_ok(recog.person_id)) {
                ESP_LOGI(TAG, "Greeting: %s → %s", recog.display_name, recog.audio_file);
                led_set_state(LED_RECOGNIZED);
                face_status_set_last_audio(recog.audio_file);
                robot_say_file(recog.display_name, recog.audio_file);
            }
        }

        free(frame_copy);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

/* ------------------------------------------------------------------ */
/* Startup demo task                                                   */
/* ------------------------------------------------------------------ */

static void robot_demo_task(void *arg)
{
    /* Wait for face detector model to load (drives heavy PSRAM/DMA activity).
       No audio plays automatically — only on button press or face recognition. */
    uint32_t waited_ms = 0;
    while (!s_face_detect_ready && waited_ms < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
    }
    lvgl_port_lock(0);
    robot_face_set_text("Kesz!");
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Startup complete — idle, waiting for button or face recognition");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* app_main                                                            */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Robot Assistant starting...");

    /* 1. Board init (I2C, I2S, codecs) */
    ESP_ERROR_CHECK(esp_board_init(48000, 2, 16));

    /* 2. LED strip — early so LEDs breathe during the rest of startup */
    if (led_effects_init() != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed — continuing without LEDs");
    }

    /* 3. IO expander (TCA9555) – required for LCD reset and PA enable */
    tca9555_driver_init();
    key_register_callback(on_key_press);
    key_module_init(NULL);

    /* 4. Mount SD card */
    ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));

    /* 5. Audio pipeline */
    Audio_Play_Init();
    Audio_Set_State_Callback(robot_audio_state_cb);
    Volume_Adjustment(60);

    /* 6. LCD hardware init */
    ESP_ERROR_CHECK(lcd_driver_init());

    /* 7. LVGL port init */
    ESP_ERROR_CHECK(lvgl_driver_init());

    /* 8. Create robot face UI */
    lvgl_port_lock(0);
    robot_face_create();
    lvgl_port_unlock();

    /* 9. Camera init and health check */
    if (robot_camera_test() == ESP_OK) {
        xTaskCreate(face_detect_task, "face_detect", 8192 * 4, NULL, 3, NULL);
    } else {
        led_set_state(LED_ERROR);
    }

    /* 10. Wi-Fi + web server + enrollment */
    face_enroll_init();
    if (wifi_manager_start() == ESP_OK) {
        camera_web_server_start();
        lvgl_port_lock(0);
        robot_face_set_text("Kamera web kesz");
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "WiFi not connected — web server skipped");
    }

    /* 11. Start demo task (waits internally for face detector to be ready) */
    xTaskCreate(robot_demo_task, "robot_demo", 4096, NULL, 5, NULL);
}
