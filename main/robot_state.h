#pragma once

#include <stdbool.h>
#include "esp_gmf_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROBOT_IDLE = 0,
    ROBOT_THINKING,
    ROBOT_SPEAKING,
    ROBOT_ERROR,
} robot_state_t;

robot_state_t robot_get_state(void);
const char   *robot_state_name(robot_state_t state);
bool          robot_is_speaking(void);

void robot_set_idle(void);
void robot_set_thinking(void);
void robot_set_speaking(const char *text);
void robot_set_error(const char *text);

esp_gmf_err_t robot_say_file(const char *text, const char *url);

#ifdef __cplusplus
}
#endif
