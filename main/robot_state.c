#include "robot_state.h"

#include "audio_driver.h"
#include "robot_face.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "robot_state";

static robot_state_t s_state = ROBOT_IDLE;

robot_state_t robot_get_state(void)
{
    return s_state;
}

const char *robot_state_name(robot_state_t state)
{
    switch (state) {
        case ROBOT_IDLE: return "IDLE";
        case ROBOT_THINKING: return "THINKING";
        case ROBOT_SPEAKING: return "SPEAKING";
        case ROBOT_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

bool robot_is_speaking(void)
{
    return s_state == ROBOT_SPEAKING ||
           Audio_Get_Current_State() == ESP_ASP_STATE_RUNNING;
}

static void set_display_text(const char *text, bool speaking)
{
    lvgl_port_lock(0);
    robot_face_set_sleep(false);
    robot_face_set_text(text ? text : "");
    robot_face_set_speaking(speaking);
    lvgl_port_unlock();
}

void robot_set_idle(void)
{
    ESP_LOGI(TAG, "-> IDLE");
    s_state = ROBOT_IDLE;
    set_display_text("Varok...", false);
}

void robot_set_thinking(void)
{
    ESP_LOGI(TAG, "-> THINKING");
    s_state = ROBOT_THINKING;
    set_display_text("Gondolkodom...", false);
}

void robot_set_speaking(const char *text)
{
    ESP_LOGI(TAG, "-> SPEAKING: %s", text ? text : "");
    s_state = ROBOT_SPEAKING;
    set_display_text(text ? text : "", true);
}

void robot_set_error(const char *text)
{
    ESP_LOGW(TAG, "-> ERROR: %s", text ? text : "");
    s_state = ROBOT_ERROR;
    set_display_text(text && text[0] ? text : "Hiba tortent", false);
}

esp_gmf_err_t robot_say_file(const char *text, const char *url)
{
    if (s_state == ROBOT_SPEAKING) {
        ESP_LOGW(TAG, "Already speaking, ignoring '%s'", url ? url : "");
        return ESP_GMF_ERR_INVALID_STATE;
    }
    robot_set_speaking(text);
    esp_gmf_err_t err = Audio_Play_Music(url);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Audio_Play_Music failed: %d", (int)err);
        robot_set_idle();
    }
    return err;
}
