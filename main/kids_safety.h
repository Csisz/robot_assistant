#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool kids_safety_message_allowed(const char *message, const char **blocked_reason);

#ifdef __cplusplus
}
#endif
