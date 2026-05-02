#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_IDLE,
    ROBOT_SPEAKING,
    ROBOT_THINKING,
    ROBOT_LISTENING,
    ROBOT_SLEEPING,
} robot_state_t;

robot_state_t robot_get_state(void);

void robot_set_idle(void);
void robot_set_speaking(const char *text);
void robot_set_thinking(void);
void robot_set_listening(void);
void robot_set_sleeping(void);

/* Set speaking state, show text, and play an MP3 URL from the SD card. */
void robot_say_file(const char *text, const char *url);

#ifdef __cplusplus
}
#endif
