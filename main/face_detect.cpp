/*
 * face_detect.cpp — wrapper around espressif/human_face_detect
 *
 * Key design decisions:
 *   - EAGER loading (lazy_load=false): model loads from flash rodata during
 *     face_detect_init().  Any OOM becomes visible at init time rather than
 *     silently at the first inference call inside another module's init path.
 *   - Global s_byte_swap selects RGB565BE vs RGB565LE for the regular pipeline.
 *   - face_detect_run_ex2() accepts an explicit pix_type (used by /debug/detect
 *     to probe all four RGB/BGR 565 variants without mutating global state).
 *   - face_detect_is_ready() lets callers check init state without calling into
 *     detect_impl and getting a silent 0-return.
 */

#include "face_detect.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "face_detect";

#if __has_include("human_face_detect.hpp")
    #include "human_face_detect.hpp"
    #include "dl_image_define.hpp"
    #define FACE_DL_AVAILABLE 1
#else
    #warning "human_face_detect.hpp not found. Run 'idf.py build' once to download managed components."
    #define FACE_DL_AVAILABLE 0
#endif

/* ------------------------------------------------------------------ */

#if FACE_DL_AVAILABLE
static HumanFaceDetect   *s_detector  = nullptr;
static SemaphoreHandle_t  s_infer_mux = NULL;
#endif

/* Global pixel-type selector for the regular pipeline.
   Written by face_detect_set_byte_swap(); read in detect_impl.          */
static volatile bool s_byte_swap = false;

extern "C" void face_detect_set_byte_swap(bool swap) { s_byte_swap = swap; }
extern "C" bool face_detect_get_byte_swap(void)       { return s_byte_swap; }

extern "C" bool face_detect_is_ready(void)
{
#if !FACE_DL_AVAILABLE
    return false;
#else
    return (s_detector != nullptr && s_infer_mux != NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* face_detect_init                                                    */
/* ------------------------------------------------------------------ */

extern "C" esp_err_t face_detect_init(void)
{
#if !FACE_DL_AVAILABLE
    ESP_LOGW(TAG, "human_face_detect component not available — detection disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_detector) {
        ESP_LOGI(TAG, "face_detect_init: already initialised");
        return ESP_OK;
    }

    s_infer_mux = xSemaphoreCreateMutex();
    if (!s_infer_mux) {
        ESP_LOGE(TAG, "face_detect_init: mutex creation failed (OOM internal RAM)");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "face_detect_init: loading MSRMNP model from flash rodata...");
    ESP_LOGI(TAG, "  PSRAM free before: %u B (%.1f MB)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);

    /* EAGER load (lazy_load=false): forces the MSR+MNP model weights to be
       loaded from flash into PSRAM tensors RIGHT NOW, so any OOM is caught
       here rather than silently inside face_recog_init()'s enroll loop.    */
    s_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);

    if (!s_detector) {
        ESP_LOGE(TAG, "face_detect_init: HumanFaceDetect allocation failed (OOM)");
        vSemaphoreDelete(s_infer_mux);
        s_infer_mux = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "face_detect_init: model loaded OK");
    ESP_LOGI(TAG, "  PSRAM free after:  %u B (%.1f MB)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Internal: run inference with an explicit pixel type.               */
/* Caller must pass one of the DL_IMAGE_PIX_TYPE_* integer values.   */
/* ------------------------------------------------------------------ */

#if FACE_DL_AVAILABLE
static int run_with_pix_type(const uint16_t *data, int width, int height,
                              dl::image::pix_type_t pix_type,
                              int bbox[4], int keypoints[10])
{
    if (!s_detector || !s_infer_mux) {
        ESP_LOGE(TAG, "run_with_pix_type: detector=%p mutex=%p — NOT ready",
                 (void *)s_detector, (void *)s_infer_mux);
        return 0;
    }
    if (xSemaphoreTake(s_infer_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "run_with_pix_type: mutex timeout (5 s)");
        return 0;
    }

    dl::image::img_t img = {
        .data     = const_cast<uint16_t *>(data),
        .width    = static_cast<uint16_t>(width),
        .height   = static_cast<uint16_t>(height),
        .pix_type = pix_type,
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
#endif /* FACE_DL_AVAILABLE */

/* ------------------------------------------------------------------ */
/* Public detection API                                                */
/* ------------------------------------------------------------------ */

extern "C" bool face_detect_run(const uint16_t *rgb565, int width, int height)
{
#if !FACE_DL_AVAILABLE
    return false;
#else
    if (!s_detector || !s_infer_mux) {
        ESP_LOGE(TAG, "face_detect_run: detector not ready");
        return false;
    }
    dl::image::pix_type_t ptype = s_byte_swap
        ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
        : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
    int count = run_with_pix_type(rgb565, width, height, ptype, nullptr, nullptr);
    if (count > 0) ESP_LOGI(TAG, "Face detected (%d result(s))", count);
    return count > 0;
#endif
}

extern "C" int face_detect_run_ex(const uint16_t *rgb565, int width, int height, int bbox[4])
{
#if !FACE_DL_AVAILABLE
    return 0;
#else
    if (!s_detector || !s_infer_mux) {
        ESP_LOGE(TAG, "face_detect_run_ex: detector not ready");
        return 0;
    }
    dl::image::pix_type_t ptype = s_byte_swap
        ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
        : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
    return run_with_pix_type(rgb565, width, height, ptype, bbox, nullptr);
#endif
}

extern "C" int face_detect_run_full(const uint16_t *rgb565, int width, int height,
                                     int bbox[4], int keypoints[10])
{
#if !FACE_DL_AVAILABLE
    return 0;
#else
    if (!s_detector || !s_infer_mux) {
        ESP_LOGE(TAG, "face_detect_run_full: detector not ready");
        return 0;
    }
    dl::image::pix_type_t ptype = s_byte_swap
        ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
        : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
    return run_with_pix_type(rgb565, width, height, ptype, bbox, keypoints);
#endif
}

/* pix_type_int maps directly to dl::image::pix_type_t enum values:
   9=RGB565LE  10=RGB565BE  11=BGR565LE  12=BGR565BE
   Used by /debug/detect to probe all variants without touching s_byte_swap. */
extern "C" int face_detect_run_ex2(const uint16_t *data, int width, int height,
                                    int pix_type_int, int bbox[4])
{
#if !FACE_DL_AVAILABLE
    return 0;
#else
    if (!s_detector || !s_infer_mux) {
        ESP_LOGE(TAG, "face_detect_run_ex2: detector not ready");
        return 0;
    }
    dl::image::pix_type_t ptype = static_cast<dl::image::pix_type_t>(pix_type_int);
    return run_with_pix_type(data, width, height, ptype, bbox, nullptr);
#endif
}
