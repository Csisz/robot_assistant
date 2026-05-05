#include "face_status.h"
#include "robot_state.h"
#include "led_effects.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "face_status";

/* Written by face_detect_task (prio 3), read by httpd task (prio 5).
   Updates happen at most every 2500 ms; reads are display-only.
   A brief inconsistent read is acceptable here without a mutex. */
static struct {
    bool    recognition_available;
    bool    face_present;
    bool    recognized;
    char    person_id[16];
    char    display_name[64];
    char    audio_file[64];
    float   confidence;
    int64_t last_seen_us;
    char    last_audio[64];
} s_st;

/* ------------------------------------------------------------------ */

static const char *s_robot_state_name(robot_state_t s)
{
    switch (s) {
        case ROBOT_IDLE:     return "IDLE";
        case ROBOT_SPEAKING: return "SPEAKING";
        case ROBOT_THINKING: return "THINKING";
        case ROBOT_LISTENING: return "LISTENING";
        case ROBOT_SLEEPING: return "SLEEPING";
        default:             return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */

void face_status_set_recognition_available(bool available)
{
    s_st.recognition_available = available;
    ESP_LOGI(TAG, "Recognition engine: %s", available ? "available" : "not available");
}

void face_status_set_no_face(void)
{
    s_st.face_present    = false;
    s_st.recognized      = false;
    s_st.person_id[0]    = '\0';
    s_st.display_name[0] = '\0';
    s_st.audio_file[0]   = '\0';
    s_st.confidence      = 0.0f;
}

void face_status_set_detected(void)
{
    s_st.face_present    = true;
    s_st.recognized      = false;
    s_st.person_id[0]    = '\0';
    s_st.display_name[0] = '\0';
    s_st.audio_file[0]   = '\0';
    s_st.confidence      = 0.0f;
    s_st.last_seen_us    = esp_timer_get_time();
    ESP_LOGI(TAG, "Face detected");
}

void face_status_set_recognition_result(bool recognized,
                                         const char *person_id,
                                         const char *display_name,
                                         const char *audio_file,
                                         float confidence)
{
    s_st.recognized = recognized;
    s_st.confidence = confidence;

    if (person_id)    strlcpy(s_st.person_id,    person_id,    sizeof(s_st.person_id));
    else              s_st.person_id[0] = '\0';

    if (display_name) strlcpy(s_st.display_name, display_name, sizeof(s_st.display_name));
    else              s_st.display_name[0] = '\0';

    if (audio_file)   strlcpy(s_st.audio_file,   audio_file,   sizeof(s_st.audio_file));
    else              s_st.audio_file[0] = '\0';

    if (recognized) {
        ESP_LOGI(TAG, "Recognized: %s (%s) score=%.2f",
                 s_st.display_name, s_st.person_id, (double)confidence);
    }
}

void face_status_set_last_audio(const char *audio_file)
{
    if (audio_file) strlcpy(s_st.last_audio, audio_file, sizeof(s_st.last_audio));
    else            s_st.last_audio[0] = '\0';
}

void face_status_get_json(char *buf, size_t buflen)
{
    const char *rs = s_robot_state_name(robot_get_state());
    const char *ls = led_state_name(led_get_state());

    snprintf(buf, buflen,
             "{\"face_present\":%s,\"recognized\":%s,"
             "\"person_id\":\"%s\",\"display_name\":\"%s\","
             "\"audio_file\":\"%s\",\"confidence\":%.2f,"
             "\"recognition_available\":%s,"
             "\"robot_state\":\"%s\",\"led_state\":\"%s\","
             "\"last_audio\":\"%s\"}",
             s_st.face_present          ? "true" : "false",
             s_st.recognized            ? "true" : "false",
             s_st.person_id,
             s_st.display_name,
             s_st.audio_file,
             (double)s_st.confidence,
             s_st.recognition_available ? "true" : "false",
             rs,
             ls,
             s_st.last_audio);
}
