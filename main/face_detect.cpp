/*
 * face_detect.cpp — wrapper around espressif/human_face_detect (v0.4.x)
 *
 * The model weights are bundled inside the espressif/human_face_detect managed
 * component as flash rodata — no separate partition or SD card file needed.
 *
 * Input: RGB565 big-endian frames (matches OV2640 PIXFORMAT_RGB565 output).
 * Returns true when at least one face is found.
 */

#include "face_detect.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "face_detect";

/* ------------------------------------------------------------------ */
/* Guard so the translation unit still compiles before the managed    */
/* component is downloaded (gives a clear warning instead of a        */
/* cryptic error).                                                     */
/* ------------------------------------------------------------------ */
#if __has_include("human_face_detect.hpp")
    #include "human_face_detect.hpp"
    #include "dl_image_define.hpp"
    #define FACE_DL_AVAILABLE 1
#else
    #warning "human_face_detect.hpp not found. Run 'idf.py build' once to download managed components, then rebuild."
    #define FACE_DL_AVAILABLE 0
#endif

/* ------------------------------------------------------------------ */

#if FACE_DL_AVAILABLE
static HumanFaceDetect   *s_detector  = nullptr;
static SemaphoreHandle_t  s_infer_mux = NULL;
#endif

extern "C" esp_err_t face_detect_init(void)
{
#if !FACE_DL_AVAILABLE
    ESP_LOGW(TAG, "human_face_detect component not available — face detection disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_detector) return ESP_OK;

    s_infer_mux = xSemaphoreCreateMutex();
    if (!s_infer_mux) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Initializing HumanFaceDetect — SPIRAM free before: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_detector = new HumanFaceDetect();
    if (!s_detector) {
        ESP_LOGE(TAG, "HumanFaceDetect allocation failed (out of memory)");
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
    if (!s_detector || !s_infer_mux) return false;
    if (xSemaphoreTake(s_infer_mux, pdMS_TO_TICKS(5000)) != pdTRUE) return false;

    dl::image::img_t img = {
        .data     = const_cast<uint16_t *>(rgb565),
        .width    = static_cast<uint16_t>(width),
        .height   = static_cast<uint16_t>(height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };

    auto &results = s_detector->run(img);
    bool found = !results.empty();
    if (found) {
        ESP_LOGI(TAG, "Face detected (%zu result(s))", results.size());
    }
    xSemaphoreGive(s_infer_mux);
    return found;
#endif
}

#if FACE_DL_AVAILABLE
static int detect_impl(const uint16_t *rgb565, int width, int height,
                        int bbox[4], int keypoints[10])
{
    if (!s_detector || !s_infer_mux) return 0;
    if (xSemaphoreTake(s_infer_mux, pdMS_TO_TICKS(5000)) != pdTRUE) return 0;

    dl::image::img_t img = {
        .data     = const_cast<uint16_t *>(rgb565),
        .width    = static_cast<uint16_t>(width),
        .height   = static_cast<uint16_t>(height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };

    auto &results = s_detector->run(img);
    int count = static_cast<int>(results.size());
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

    if (count > 0) {
        const auto &r = results.front();
        if (bbox) {
            bbox[0] = clamp(r.box[0], 0, width  - 1);
            bbox[1] = clamp(r.box[1], 0, height - 1);
            bbox[2] = clamp(r.box[2], 0, width  - 1);
            bbox[3] = clamp(r.box[3], 0, height - 1);
        }
        if (keypoints) {
            int kp_n = static_cast<int>(r.keypoint.size());
            for (int i = 0; i < 10; i++) {
                if (i < kp_n) {
                    int lim = (i % 2 == 0) ? width - 1 : height - 1;
                    keypoints[i] = clamp(r.keypoint[i], 0, lim);
                } else {
                    keypoints[i] = 0;
                }
            }
        }
    } else {
        if (bbox)      { bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0; }
        if (keypoints) { for (int i = 0; i < 10; i++) keypoints[i] = 0; }
    }

    xSemaphoreGive(s_infer_mux);
    return count;
}
#endif

extern "C" int face_detect_run_ex(const uint16_t *rgb565, int width, int height, int bbox[4])
{
#if !FACE_DL_AVAILABLE
    return 0;
#else
    return detect_impl(rgb565, width, height, bbox, nullptr);
#endif
}

extern "C" int face_detect_run_full(const uint16_t *rgb565, int width, int height,
                                     int bbox[4], int keypoints[10])
{
#if !FACE_DL_AVAILABLE
    return 0;
#else
    return detect_impl(rgb565, width, height, bbox, keypoints);
#endif
}
