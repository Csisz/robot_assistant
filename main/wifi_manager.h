#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_start(void);
bool      wifi_manager_is_connected(void);
const char *wifi_manager_ssid(void);
int       wifi_manager_rssi(void);
const char *wifi_manager_backend_base_url(void);
const char *wifi_manager_last_disconnect_reason(void);

#ifdef __cplusplus
}
#endif
