#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOT_CHAT_REPLY_TEXT_MAX   512
#define ROBOT_CHAT_MESSAGE_MAX      512
#define ROBOT_CHAT_TRANSCRIPT_MAX   512
#define ROBOT_CHAT_AUDIO_URL_MAX    320
#define ROBOT_CHAT_REASON_MAX        48
#define ROBOT_CHAT_MOOD_MAX          24
#define ROBOT_CHAT_ACTION_MAX        32
#define ROBOT_CHAT_CONVERSATION_ID_MAX 96
#define ROBOT_CHAT_PROFILE_ID_MAX    40
#define ROBOT_CHAT_ACTIVE_MODE_MAX   24

typedef struct {
    bool ok;
    bool safe;
    int http_status;
    char transcript[ROBOT_CHAT_TRANSCRIPT_MAX];
    char conversation_id[ROBOT_CHAT_CONVERSATION_ID_MAX];
    char profile_id[ROBOT_CHAT_PROFILE_ID_MAX];
    char reply_text[ROBOT_CHAT_REPLY_TEXT_MAX];
    char reply_audio_url[ROBOT_CHAT_AUDIO_URL_MAX];
    char robot_mood[ROBOT_CHAT_MOOD_MAX];
    char robot_action[ROBOT_CHAT_ACTION_MAX];
    char blocked_reason[ROBOT_CHAT_REASON_MAX];
    char active_mode[ROBOT_CHAT_ACTIVE_MODE_MAX];
    char error[96];
} robot_chat_response_t;

esp_err_t robot_chat_init(void);
bool robot_chat_enabled(void);
esp_err_t robot_chat_send_text(const char *message, robot_chat_response_t *out);
esp_err_t robot_chat_send_voice_file(const char *wav_path, robot_chat_response_t *out);
esp_err_t robot_chat_start_voice_record(char *error, size_t error_len);
bool robot_chat_worker_available(void);
const char *robot_chat_backend_url(void);
esp_err_t robot_chat_set_backend_base_url(const char *backend_base_url);

#ifdef __cplusplus
}
#endif
