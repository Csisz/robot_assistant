#pragma once

#include <stddef.h>

typedef struct {
    const char *ssid;
    const char *password;
    const char *backend_base_url;
} wifi_profile_t;

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
static const wifi_profile_t WIFI_PROFILES[] = {
    {"YOUR_WIFI_SSID_1", "YOUR_WIFI_PASSWORD_1", "http://192.168.1.234:8000"},
    {"YOUR_WIFI_SSID_2", "YOUR_WIFI_PASSWORD_2", "http://192.168.1.234:8000"},
};
#endif

static const size_t WIFI_PROFILE_COUNT = sizeof(WIFI_PROFILES) / sizeof(WIFI_PROFILES[0]);
