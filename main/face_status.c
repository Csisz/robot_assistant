#include "face_status.h"
#include "face_enroll.h"
#include "robot_state.h"
#include "led_effects.h"
#include "camera_driver.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "face_status";

/* Written by face_detect_task (prio 3), read by httpd task (prio 5).
   Updates at most every 2500 ms; reads are display-only.
   A brief inconsistent read is acceptable without a mutex. */
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
    char    last_error[80];
    int     known_people_count;
} s_st;

/* ------------------------------------------------------------------ */

static const char *s_robot_state_name(robot_state_t s)
{
    switch (s) {
        case ROBOT_IDLE:      return "IDLE";
        case ROBOT_SPEAKING:  return "SPEAKING";
        case ROBOT_THINKING:  return "THINKING";
        case ROBOT_LISTENING: return "LISTENING";
        case ROBOT_SLEEPING:  return "SLEEPING";
        default:              return "UNKNOWN";
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

void face_status_set_last_error(const char *err)
{
    if (err) strlcpy(s_st.last_error, err, sizeof(s_st.last_error));
    else     s_st.last_error[0] = '\0';
}

void face_status_set_known_people_count(int count)
{
    s_st.known_people_count = count;
}

void face_status_get_json(char *buf, size_t buflen)
{
    const char *rs  = s_robot_state_name(robot_get_state());
    const char *ls  = led_state_name(led_get_state());
    enroll_status_t enr = face_enroll_get_status();
    const char *es  = enr.active ? "active" : "idle";
    camera_health_t cam = {0};
    camera_get_health(&cam);

    snprintf(buf, buflen,
             "{\"face_present\":%s,\"recognized\":%s,"
             "\"person_id\":\"%s\",\"display_name\":\"%s\","
             "\"audio_file\":\"%s\",\"confidence\":%.2f,"
             "\"recognition_available\":%s,"
             "\"robot_state\":\"%s\",\"led_state\":\"%s\","
             "\"enrollment_state\":\"%s\","
             "\"last_audio\":\"%s\","
             "\"last_error\":\"%s\","
             "\"known_people_count\":%d,"
             "\"camera\":{\"initialized\":%s,\"psram_found\":%s,"
             "\"init_err\":%d,\"init_err_name\":\"%s\","
             "\"capture_ok_count\":%u,\"capture_fail_count\":%u,"
             "\"capture_timeout_count\":%u,\"frames_in_flight\":%u,"
             "\"last_width\":%d,\"last_height\":%d,\"last_format\":%d,"
             "\"last_len\":%u,\"last_error\":\"%s\","
             "\"psram_free\":%u,\"psram_largest_free\":%u,"
             "\"internal_free\":%u,\"internal_largest_free\":%u,"
             "\"xclk_freq_hz\":%d,\"pixel_format\":%d,\"frame_size\":%d,"
             "\"jpeg_quality\":%d,\"fb_count\":%d,\"fb_location\":%d,"
             "\"grab_mode\":%d}}",
             s_st.face_present          ? "true" : "false",
             s_st.recognized            ? "true" : "false",
             s_st.person_id,
             s_st.display_name,
             s_st.audio_file,
             (double)s_st.confidence,
             s_st.recognition_available ? "true" : "false",
             rs,
             ls,
             es,
             s_st.last_audio,
             s_st.last_error,
             s_st.known_people_count,
             cam.initialized ? "true" : "false",
             cam.psram_found ? "true" : "false",
             (int)cam.init_err,
             esp_err_to_name(cam.init_err),
             (unsigned)cam.capture_ok_count,
             (unsigned)cam.capture_fail_count,
             (unsigned)cam.capture_timeout_count,
             (unsigned)cam.frames_in_flight,
             cam.last_width,
             cam.last_height,
             cam.last_format,
             (unsigned)cam.last_len,
             cam.last_error,
             (unsigned)cam.psram_free,
             (unsigned)cam.psram_largest_free,
             (unsigned)cam.internal_free,
             (unsigned)cam.internal_largest_free,
             cam.xclk_freq_hz,
             cam.pixel_format,
             cam.frame_size,
             cam.jpeg_quality,
             cam.fb_count,
             cam.fb_location,
             cam.grab_mode);
}

#ifdef ROBOT_MOCK_MODE
void face_status_set_mock(const char *scenario)
{
    if (!scenario) return;

    if (strcmp(scenario, "no_face") == 0) {
        face_status_set_no_face();
    } else if (strcmp(scenario, "face") == 0) {
        face_status_set_detected();
    } else if (strcmp(scenario, "recognized") == 0) {
        face_status_set_detected();
        face_status_set_recognition_result(true, "apa", "Apa",
            "file://sdcard/APA.MP3", 0.92f);
    } else if (strcmp(scenario, "speaking") == 0) {
        face_status_set_detected();
        face_status_set_recognition_result(true, "apa", "Apa",
            "file://sdcard/APA.MP3", 0.92f);
    }
    /* "enrollment" scenario is handled at the caller level via face_enroll_start_json */
}
#endif
