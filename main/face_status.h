#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once at startup with the result of face_recog_available(). */
void face_status_set_recognition_available(bool available);

/* Called by face_detect_task on face-present/absent transitions. */
void face_status_set_no_face(void);
void face_status_set_detected(void);

/* Called after every recognition attempt on a face-present frame. */
void face_status_set_recognition_result(bool recognized,
                                         const char *person_id,
                                         const char *display_name,
                                         const char *audio_file,
                                         float confidence);

/* Track the last audio file that was played (button press or recognition). */
void face_status_set_last_audio(const char *audio_file);

/* Fill buf with a JSON object suitable for GET /status.
   Includes live robot_state and led_state derived from robot_get_state()
   and led_get_state().  buf must be at least 512 bytes. */
void face_status_get_json(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
