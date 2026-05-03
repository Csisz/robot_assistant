/*
 * face_detect.cpp — C++ wrapper around esp-dl HumanFaceDetect
 *
 * Component: espressif/esp-dl  (see idf_component.yml)
 * Model:     HumanFaceDetect MSR01 (two-stage cascade)
 *
 * If the build fails with "No such file or directory" on the include below,
 * check the actual header name inside:
 *   managed_components/espressif__esp-dl/models/human_face_detect/include/
 * and update the include accordingly.
 */

#include "face_detect.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "face_detect";

/* ------------------------------------------------------------------ */
/* esp-dl include — guarded so the file compiles even before the      */
/* component is downloaded (gives a clear log warning instead of a    */
/* cryptic linker error).                                             */
/* ------------------------------------------------------------------ */
#if __has_include("human_face_detect_msr01.hpp")
    #include "human_face_detect_msr01.hpp"
    #define FACE_DL_AVAILABLE 1
#elif __has_include("dl_detect_human_face.hpp")
    #include "dl_detect_human_face.hpp"
    #define FACE_DL_AVAILABLE 1
#else
    #warning "esp-dl face detection header not found. Run 'idf.py build' once to download managed components, then rebuild."
    #define FACE_DL_AVAILABLE 0
#endif

/* ------------------------------------------------------------------ */

#if FACE_DL_AVAILABLE
static HumanFaceDetect *s_detector = nullptr;
#endif

extern "C" esp_err_t face_detect_init(void)
{
#if !FACE_DL_AVAILABLE
    ESP_LOGW(TAG, "esp-dl not available — face detection disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_detector) return ESP_OK;

    ESP_LOGI(TAG, "Allocating face detector — SPIRAM free before: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_detector = new HumanFaceDetect;
    if (!s_detector) {
        ESP_LOGE(TAG, "new HumanFaceDetect failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Face detector ready — SPIRAM free after: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
#endif
}

extern "C" bool face_detect_run(const uint16_t *rgb565, int width, int height)
{
#if !FACE_DL_AVAILABLE
    return false;
#else
    if (!s_detector) return false;

    /* esp-dl HumanFaceDetect takes RGB565 as uint16_t*, shape {H, W, 3}.
     * The model unpacks the 5-6-5 channels internally. */
    auto results = s_detector->infer(
        const_cast<uint16_t *>(rgb565),
        {height, width, 3}
    );

    bool found = !results.empty();
    if (found) {
        ESP_LOGI(TAG, "Face detected (%zu candidate(s))", results.size());
    }
    return found;
#endif
}
