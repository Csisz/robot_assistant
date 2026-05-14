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

/* Set a human-readable error string shown in /status (empty string clears). */
void face_status_set_last_error(const char *err);

/* Set how many persons are enrolled in the recognition DB (from face_recog_init). */
void face_status_set_known_people_count(int count);

/* Fill buf with a JSON object for GET /status.
   Includes live robot_state, led_state, and enrollment_state.
   buf must be at least 2048 bytes. */
void face_status_get_json(char *buf, size_t buflen);

/* Inject a mock detection scenario for UI testing (only meaningful when
   ROBOT_MOCK_MODE is defined and the /api/mock/state endpoint is hit).
   scenario: "no_face" | "face" | "recognized" | "speaking" */
#ifdef ROBOT_MOCK_MODE
void face_status_set_mock(const char *scenario);
#endif

#ifdef __cplusplus
}
#endif
