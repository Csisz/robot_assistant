#include "wifi_manager.h"
#include "wifi_profiles.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1
#define WIFI_RETRY_DELAY_MS      10000
#define WIFI_CONNECT_TIMEOUT_MS  8000

static const char *TAG = "wifi_manager";

typedef struct {
    int profile_index;
    int rssi;
    uint8_t channel;
    bool found_in_scan;
} wifi_candidate_t;

static EventGroupHandle_t s_event_group = NULL;
static bool s_connected = false;
static volatile bool s_connecting_profile = false;
static int s_current_profile = -1;
static int s_current_rssi = -127;
static uint8_t s_current_channel = 0;
static volatile bool s_ignore_disconnect = false;
static char s_selected_ssid[33];
static char s_backend_base_url[192];
static char s_last_disconnect_reason_text[48] = "none";

static bool is_placeholder_profile(const wifi_profile_t *profile)
{
    return !profile || !profile->ssid || !profile->ssid[0] ||
           strncmp(profile->ssid, "YOUR_WIFI_SSID_", 15) == 0;
}

static const char *authmode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "UNKNOWN";
    }
}

static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case 0: return "none";
        case WIFI_REASON_NO_AP_FOUND: return "WIFI_REASON_NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "WIFI_REASON_AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "WIFI_REASON_ASSOC_FAIL";
        default:
            snprintf(s_last_disconnect_reason_text, sizeof(s_last_disconnect_reason_text),
                     "WIFI_REASON_%u", (unsigned)reason);
            return s_last_disconnect_reason_text;
    }
}

static void remember_disconnect_reason(uint8_t reason)
{
    strlcpy(s_last_disconnect_reason_text,
            disconnect_reason_name(reason),
            sizeof(s_last_disconnect_reason_text));
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        return;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = ev ? ev->reason : 0;
        if (s_ignore_disconnect) {
            s_ignore_disconnect = false;
            return;
        }
        s_connected = false;
        remember_disconnect_reason(reason);
        const char *ssid = s_current_profile >= 0 ? WIFI_PROFILES[s_current_profile].ssid : "(unknown)";
        ESP_LOGW(TAG, "disconnected ssid='%s' reason=%s(%u)",
                 ssid,
                 disconnect_reason_name(reason),
                 (unsigned)reason);
        if (s_connecting_profile) {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        } else {
            ESP_LOGW(TAG, "WiFi disconnected after connection; retrying current profile ssid='%s'", ssid);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        const wifi_profile_t *profile = s_current_profile >= 0 ? &WIFI_PROFILES[s_current_profile] : NULL;
        ESP_LOGI(TAG, "connected ssid='%s' ip=" IPSTR,
                 profile && profile->ssid ? profile->ssid : "(unknown)",
                 IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Open browser: http://" IPSTR "/", IP2STR(&ev->ip_info.ip));
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err == ESP_OK) {
            ESP_LOGI(TAG, "wifi power save disabled for low-latency voice mode");
        } else {
            ESP_LOGW(TAG, "failed to disable wifi power save: %s", esp_err_to_name(ps_err));
        }
        if (profile) {
            strlcpy(s_selected_ssid, profile->ssid, sizeof(s_selected_ssid));
            strlcpy(s_backend_base_url,
                    profile->backend_base_url ? profile->backend_base_url : "",
                    sizeof(s_backend_base_url));
        }
        ESP_LOGI(TAG, "selected_wifi_ssid=%s", s_selected_ssid[0] ? s_selected_ssid : "(unknown)");
        ESP_LOGI(TAG, "selected_wifi_rssi=%d", s_current_rssi);
        ESP_LOGI(TAG, "selected_wifi_channel=%u", (unsigned)s_current_channel);
        ESP_LOGI(TAG, "backend_base_url=%s", s_backend_base_url[0] ? s_backend_base_url : "(empty)");
        s_connected = true;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool ssid_matches(const uint8_t scanned_ssid[33], const char *known_ssid)
{
    return known_ssid && known_ssid[0] &&
           strncmp((const char *)scanned_ssid, known_ssid, 32) == 0;
}

static int compare_candidates_by_rssi(const void *a, const void *b)
{
    const wifi_candidate_t *ca = (const wifi_candidate_t *)a;
    const wifi_candidate_t *cb = (const wifi_candidate_t *)b;
    return cb->rssi - ca->rssi;
}

static void log_configured_profile_scan_matches(const int *best_rssi, const uint8_t *best_channel)
{
    for (size_t p = 0; p < WIFI_PROFILE_COUNT; p++) {
        const wifi_profile_t *profile = &WIFI_PROFILES[p];
        if (is_placeholder_profile(profile)) {
            ESP_LOGI(TAG, "configured profile[%u] ssid='%s' found=no placeholder=yes",
                     (unsigned)p,
                     profile && profile->ssid ? profile->ssid : "");
            continue;
        }
        bool found = best_rssi[p] != INT_MIN;
        if (found) {
            ESP_LOGI(TAG, "configured profile[%u] ssid='%s' found=yes rssi=%d channel=%u",
                     (unsigned)p,
                     profile->ssid,
                     best_rssi[p],
                     (unsigned)best_channel[p]);
        } else {
            ESP_LOGI(TAG, "configured profile[%u] ssid='%s' found=no",
                     (unsigned)p,
                     profile->ssid);
        }
    }
}

static size_t build_scanned_candidates(wifi_candidate_t *candidates, size_t max_candidates)
{
    if (!candidates || max_candidates == 0 || WIFI_PROFILE_COUNT == 0) {
        return 0;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count == 0) {
        ESP_LOGW(TAG, "WiFi scan found no APs");
        int best_rssi[WIFI_PROFILE_COUNT];
        uint8_t best_channel[WIFI_PROFILE_COUNT];
        for (size_t i = 0; i < WIFI_PROFILE_COUNT; i++) {
            best_rssi[i] = INT_MIN;
            best_channel[i] = 0;
        }
        log_configured_profile_scan_matches(best_rssi, best_channel);
        return 0;
    }
    ESP_LOGI(TAG, "WiFi scan found %u APs", (unsigned)ap_count);

    wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aps) {
        ESP_LOGE(TAG, "WiFi scan allocation failed count=%u", (unsigned)ap_count);
        return 0;
    }
    uint16_t read_count = ap_count;
    err = esp_wifi_scan_get_ap_records(&read_count, aps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan read failed: %s", esp_err_to_name(err));
        free(aps);
        return 0;
    }

    int best_rssi[WIFI_PROFILE_COUNT];
    uint8_t best_channel[WIFI_PROFILE_COUNT];
    for (size_t i = 0; i < WIFI_PROFILE_COUNT; i++) {
        best_rssi[i] = INT_MIN;
        best_channel[i] = 0;
    }

    for (uint16_t ap = 0; ap < read_count; ap++) {
        ESP_LOGI(TAG, "AP[%u] ssid='%s' rssi=%d channel=%u auth=%s",
                 (unsigned)ap,
                 (const char *)aps[ap].ssid,
                 aps[ap].rssi,
                 (unsigned)aps[ap].primary,
                 authmode_name(aps[ap].authmode));
        for (size_t p = 0; p < WIFI_PROFILE_COUNT; p++) {
            if (is_placeholder_profile(&WIFI_PROFILES[p])) {
                continue;
            }
            if (ssid_matches(aps[ap].ssid, WIFI_PROFILES[p].ssid) && aps[ap].rssi > best_rssi[p]) {
                best_rssi[p] = aps[ap].rssi;
                best_channel[p] = aps[ap].primary;
            }
        }
    }
    log_configured_profile_scan_matches(best_rssi, best_channel);
    free(aps);

    size_t count = 0;
    for (size_t p = 0; p < WIFI_PROFILE_COUNT && count < max_candidates; p++) {
        if (best_rssi[p] == INT_MIN) {
            continue;
        }
        candidates[count++] = (wifi_candidate_t){
            .profile_index = (int)p,
            .rssi = best_rssi[p],
            .channel = best_channel[p],
            .found_in_scan = true,
        };
    }
    qsort(candidates, count, sizeof(candidates[0]), compare_candidates_by_rssi);
    return count;
}

static size_t build_fallback_candidates(wifi_candidate_t *candidates, size_t max_candidates)
{
    size_t count = 0;
    for (size_t p = 0; p < WIFI_PROFILE_COUNT && count < max_candidates; p++) {
        if (is_placeholder_profile(&WIFI_PROFILES[p])) {
            continue;
        }
        candidates[count++] = (wifi_candidate_t){
            .profile_index = (int)p,
            .rssi = -127,
            .channel = 0,
            .found_in_scan = false,
        };
    }
    return count;
}

static esp_err_t connect_profile(const wifi_candidate_t *candidate)
{
    if (!candidate || candidate->profile_index < 0 ||
        candidate->profile_index >= (int)WIFI_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_profile_t *profile = &WIFI_PROFILES[candidate->profile_index];
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, profile->ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, profile->password ? profile->password : "",
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = wifi_cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_connected = false;
    s_current_profile = candidate->profile_index;
    s_current_rssi = candidate->rssi;
    s_current_channel = candidate->channel;
    strlcpy(s_last_disconnect_reason_text, "none", sizeof(s_last_disconnect_reason_text));
    ESP_LOGI(TAG, "trying profile[%d] ssid='%s'%s rssi=%d channel=%u timeout_ms=%u",
             candidate->profile_index,
             profile->ssid,
             candidate->found_in_scan ? " found_in_scan=yes" : " found_in_scan=no",
             candidate->rssi,
             (unsigned)candidate->channel,
             (unsigned)WIFI_CONNECT_TIMEOUT_MS);
    s_ignore_disconnect = true;
    esp_err_t disc_err = esp_wifi_disconnect();
    if (disc_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_ignore_disconnect = false;
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    s_connecting_profile = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connecting_profile = false;
        ESP_LOGW(TAG, "connect failed ssid='%s' reason=%s",
                 profile->ssid, esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        s_connecting_profile = false;
        ESP_LOGI(TAG, "connected ssid='%s'", profile->ssid);
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        s_connecting_profile = false;
        ESP_LOGW(TAG, "connect failed ssid='%s' reason=%s", profile->ssid,
                 s_last_disconnect_reason_text[0] ? s_last_disconnect_reason_text : "unknown");
        s_ignore_disconnect = true;
        disc_err = esp_wifi_disconnect();
        if (disc_err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_ignore_disconnect = false;
        return ESP_FAIL;
    }
    s_connecting_profile = false;
    ESP_LOGW(TAG, "connect failed ssid='%s' reason=timeout", profile->ssid);
    s_ignore_disconnect = true;
    disc_err = esp_wifi_disconnect();
    if (disc_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_ignore_disconnect = false;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi profiles configured=%u", (unsigned)WIFI_PROFILE_COUNT);
    for (;;) {
        wifi_candidate_t candidates[WIFI_PROFILE_COUNT];
#if CONFIG_ROBOT_WIFI_TRY_HIDDEN_PROFILES
        ESP_LOGI(TAG, "CONFIG_ROBOT_WIFI_TRY_HIDDEN_PROFILES enabled; skipping scan matching");
        size_t candidate_count = build_fallback_candidates(candidates, WIFI_PROFILE_COUNT);
#else
        size_t candidate_count = build_scanned_candidates(candidates, WIFI_PROFILE_COUNT);
        if (candidate_count == 0) {
            ESP_LOGW(TAG, "No known scanned WiFi found; trying profiles in configured order");
            candidate_count = build_fallback_candidates(candidates, WIFI_PROFILE_COUNT);
        }
#endif

        for (size_t i = 0; i < candidate_count; i++) {
            if (connect_profile(&candidates[i]) == ESP_OK) {
                return ESP_OK;
            }
        }

        ESP_LOGE(TAG, "All WiFi profiles failed; retrying in %u ms", (unsigned)WIFI_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
    }
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_ssid(void)
{
    return s_selected_ssid;
}

int wifi_manager_rssi(void)
{
    return s_current_rssi;
}

const char *wifi_manager_backend_base_url(void)
{
    return s_backend_base_url;
}

const char *wifi_manager_last_disconnect_reason(void)
{
    return s_last_disconnect_reason_text;
}
