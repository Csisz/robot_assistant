#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);
void web_server_set_last_audio(const char *path);
void web_server_set_last_error(const char *error);
void web_server_set_last_transcript(const char *transcript);

#ifdef __cplusplus
}
#endif
