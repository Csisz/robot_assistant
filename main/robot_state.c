#include "robot_state.h"
#include "robot_face.h"
#include "audio_driver.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "robot_state";

static robot_state_t s_state = ROBOT_IDLE;

robot_state_t robot_get_state(void)
{
    return s_state;
}

bool robot_is_speaking(void)
{
    return s_state == ROBOT_SPEAKING ||
           Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING;
}

void robot_set_idle(void)
{
    ESP_LOGI(TAG, "-> IDLE");
    s_state = ROBOT_IDLE;
    lvgl_port_lock(0);
    robot_face_set_speaking(false);
    robot_face_set_sleep(false);
    robot_face_set_text("Várok...");
    lvgl_port_unlock();
}

void robot_set_speaking(const char *text)
{
    ESP_LOGI(TAG, "-> SPEAKING: %s", text ? text : "");
    s_state = ROBOT_SPEAKING;
    lvgl_port_lock(0);
    robot_face_set_sleep(false);
    robot_face_set_text(text ? text : "");
    robot_face_set_speaking(true);
    lvgl_port_unlock();
}

void robot_set_thinking(void)
{
    ESP_LOGI(TAG, "-> THINKING");
    s_state = ROBOT_THINKING;
    lvgl_port_lock(0);
    robot_face_set_speaking(false);
    robot_face_set_sleep(false);
    robot_face_set_text("Gondolkodom...");
    lvgl_port_unlock();
}

void robot_set_listening(void)
{
    ESP_LOGI(TAG, "-> LISTENING");
    s_state = ROBOT_LISTENING;
    lvgl_port_lock(0);
    robot_face_set_speaking(false);
    robot_face_set_sleep(false);
    robot_face_set_text("Hallgatom...");
    lvgl_port_unlock();
}

void robot_set_sleeping(void)
{
    ESP_LOGI(TAG, "-> SLEEPING");
    s_state = ROBOT_SLEEPING;
    lvgl_port_lock(0);
    robot_face_set_speaking(false);
    robot_face_set_text("Zzz...");
    robot_face_set_sleep(true);
    lvgl_port_unlock();
}

void robot_say_file(const char *text, const char *url)
{
    if (s_state == ROBOT_SPEAKING) {
        ESP_LOGW(TAG, "Already speaking — ignoring '%s'", url ? url : "");
        return;
    }
    ESP_LOGI(TAG, "-> SPEAKING (file): %s | %s", text ? text : "", url ? url : "");
    s_state = ROBOT_SPEAKING;
    lvgl_port_lock(0);
    robot_face_set_sleep(false);
    robot_face_set_text(text ? text : "");
    robot_face_set_speaking(true);
    lvgl_port_unlock();
    esp_gmf_err_t err = Audio_Play_Music(url);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Audio_Play_Music failed: %d", (int)err);
        robot_set_idle();
    }
}
