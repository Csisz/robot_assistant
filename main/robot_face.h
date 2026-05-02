#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void robot_face_create(void);
void robot_face_set_text(const char *text);
void robot_face_set_speaking(bool speaking);
void robot_face_set_sleep(bool sleep_mode);

#ifdef __cplusplus
}
#endif
