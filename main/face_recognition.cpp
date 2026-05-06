/*
 * face_recognition.cpp — real face recognition using HumanFaceRecognizer
 *
 * Enrollment (called once from face_recog_init):
 *   Parse /sdcard/FACES/DB.TXT  (CSV: person_id,display_name,audio_file,sample_count)
 *   For each person, load sample JPEGs from /sdcard/FACES/<ID_UPPER>/S01.JPG ...
 *   Detect the face in each sample, then enroll into HumanFaceRecognizer.
 *
 * Recognition (called per frame from face_detect_task):
 *   Wrap the RGB565BE frame in dl::image::img_t, reconstruct dl::detect::result_t
 *   from the already-computed bbox + keypoints, then call recognize().
 *
 * PSRAM guard: the recognizer model can use 2–4 MB of PSRAM.  Combined with
 * the detector model (~3–4 MB), the total may exceed what's left for inference
 * tensor buffers, causing silent detection failures.  We therefore measure PSRAM
 * before and after creating the recognizer; if it drops below MIN_PSRAM_RESERVE
 * we immediately delete the recognizer so the detector can work normally.
 */

#include "face_recognition.h"
#include "face_detect.h"
#include "face_status.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <list>
#include <vector>

#if __has_include("human_face_recognition.hpp")
    #include "human_face_recognition.hpp"
    #include "dl_image_define.hpp"
    #include "dl_image_jpeg.hpp"
    #include "dl_detect_define.hpp"
    #define FACE_RECOG_AVAILABLE 1
#else
    #warning "human_face_recognition.hpp not found — face recognition disabled"
    #define FACE_RECOG_AVAILABLE 0
#endif

static const char *TAG = "face_recog";

/* ------------------------------------------------------------------ */
/* DB path — needed for counting even without the recognition model   */
/* ------------------------------------------------------------------ */
#define DB_TXT_PATH  "/sdcard/FACES/DB.TXT"

/* Always populated from DB.TXT regardless of FACE_RECOG_AVAILABLE.  */
static int s_db_entry_count = 0;

/* Count valid lines in DB.TXT; used for known_people_count in /status. */
static int count_db_entries(void)
{
    FILE *f = fopen(DB_TXT_PATH, "r");
    if (!f) return 0;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (!line[0] || line[0] == '#') continue;
        char pid[16] = {0}, name[64] = {0}, afile[64] = {0};
        int samples = 0;
        if (sscanf(line, "%15[^,],%63[^,],%63[^,],%d",
                   pid, name, afile, &samples) == 4 && pid[0])
            n++;
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------------ */

#if FACE_RECOG_AVAILABLE

#define DB_BIN_PATH   "/sdcard/FACES/RECOG.BIN"
#define MAX_PERSONS   16

/* Minimum PSRAM that must remain free after loading the recognizer.
   If less than this survives, the recognizer is unloaded immediately so
   that the detector's inference buffers can still be allocated.        */
#define MIN_PSRAM_RESERVE  (1500UL * 1024UL)   /* 1.5 MB */

struct PersonEntry {
    char     person_id[16];
    char     display_name[64];
    char     audio_file[64];    /* already in file://sdcard/... form */
    uint16_t db_id_start;
    uint16_t db_id_end;         /* exclusive */
};

static HumanFaceRecognizer *s_recognizer  = nullptr;
static PersonEntry           s_persons[MAX_PERSONS];
static int                   s_num_persons = 0;
static bool                  s_available   = false;

/* /sdcard/FOO.MP3 → file://sdcard/FOO.MP3 */
static void to_audio_url(const char *path, char *url, size_t url_len)
{
    if (strncmp(path, "/sdcard/", 8) == 0) {
        snprintf(url, url_len, "file://sdcard/%s", path + 8);
    } else {
        snprintf(url, url_len, "%s", path);
    }
}

static void str_toupper_n(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && src[i]; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void enroll_person(PersonEntry &p, int sample_count)
{
    char id_upper[16];
    str_toupper_n(id_upper, p.person_id, sizeof(id_upper));

    p.db_id_start = (uint16_t)s_recognizer->get_num_feats();
    int enrolled = 0;

    for (int s = 1; s <= sample_count && s <= 9; s++) {
        char path[80];
        snprintf(path, sizeof(path), "/sdcard/FACES/%s/S%02d.JPG", id_upper, s);

        dl::image::jpeg_img_t jpg = dl::image::read_jpeg(path);
        if (!jpg.data) {
            ESP_LOGW(TAG, "  %s: not found, skipping", path);
            continue;
        }

        dl::image::img_t img = dl::image::sw_decode_jpeg(jpg, dl::image::DL_IMAGE_PIX_TYPE_RGB565BE);
        free(jpg.data);

        if (!img.data) {
            ESP_LOGW(TAG, "  %s: JPEG decode failed", path);
            continue;
        }

        int bbox[4] = {0};
        int kpts[10] = {0};
        int fc = face_detect_run_full(static_cast<const uint16_t *>(img.data),
                                      img.width, img.height, bbox, kpts);
        if (fc < 1) {
            ESP_LOGW(TAG, "  %s: no face detected, skipping", path);
            free(img.data);
            continue;
        }

        dl::detect::result_t det;
        det.category = 0;
        det.score    = 1.0f;
        det.box      = {bbox[0], bbox[1], bbox[2], bbox[3]};
        det.keypoint = {kpts[0], kpts[1], kpts[2], kpts[3], kpts[4],
                        kpts[5], kpts[6], kpts[7], kpts[8], kpts[9]};

        std::list<dl::detect::result_t> det_list;
        det_list.push_back(det);

        esp_err_t err = s_recognizer->enroll(img, det_list);
        free(img.data);

        if (err == ESP_OK) {
            enrolled++;
        } else {
            ESP_LOGW(TAG, "  %s: enroll failed (%s)", path, esp_err_to_name(err));
        }
    }

    p.db_id_end = (uint16_t)s_recognizer->get_num_feats();
    ESP_LOGI(TAG, "Loaded person %s/%s samples=%d enrolled=%d (ids %u..%u)",
             p.person_id, p.display_name, sample_count, enrolled,
             (unsigned)p.db_id_start, (unsigned)(p.db_id_end > 0 ? p.db_id_end - 1 : 0));
}

#endif /* FACE_RECOG_AVAILABLE */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

extern "C" bool face_recog_available(void)
{
#if FACE_RECOG_AVAILABLE
    return s_available;
#else
    return false;
#endif
}

extern "C" int face_recog_get_known_count(void)
{
    /* Always return the DB.TXT entry count, even when recognition is
       disabled — so /status shows the correct "known_people_count". */
    return s_db_entry_count;
}

extern "C" esp_err_t face_recog_init(void)
{
    /* Always count DB.TXT entries so known_people_count is correct. */
    s_db_entry_count = count_db_entries();
    ESP_LOGI(TAG, "DB.TXT: %d person(s) found", s_db_entry_count);

#if !FACE_RECOG_AVAILABLE
    ESP_LOGW(TAG, "human_face_recognition not compiled in — recognition disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* -------------------------------------------------------------- */
    /* PSRAM guard: check free PSRAM before loading the recognizer.   */
    /* The detector model already occupies 3–4 MB.  If the recognizer */
    /* would consume the remaining headroom needed for inference       */
    /* tensor buffers, detection silently returns 0 results.          */
    /* -------------------------------------------------------------- */
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM free before HumanFaceRecognizer: %u B (%.1f MB)",
             (unsigned)psram_before, psram_before / 1048576.0f);

    if (psram_before < 3UL * 1024 * 1024) {
        ESP_LOGW(TAG, "Only %.1f MB PSRAM free — skipping HumanFaceRecognizer "
                 "to protect face detector inference buffers.",
                 psram_before / 1048576.0f);
        return ESP_OK;
    }

    s_recognizer = new HumanFaceRecognizer(DB_BIN_PATH);
    if (!s_recognizer) {
        ESP_LOGE(TAG, "HumanFaceRecognizer allocation failed (OOM)");
        return ESP_ERR_NO_MEM;
    }

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM after HumanFaceRecognizer: %u B (%.1f MB)  used ~%u B",
             (unsigned)psram_after, psram_after / 1048576.0f,
             (unsigned)(psram_before - psram_after));

    if (psram_after < MIN_PSRAM_RESERVE) {
        ESP_LOGE(TAG, "PSRAM critically low after recognizer (%.1f MB < %.1f MB reserve) "
                 "— deleting recognizer to restore detector headroom.",
                 psram_after / 1048576.0f, MIN_PSRAM_RESERVE / 1048576.0f);
        delete s_recognizer;
        s_recognizer = nullptr;
        size_t psram_recovered = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM after deleting recognizer: %u B (%.1f MB)",
                 (unsigned)psram_recovered, psram_recovered / 1048576.0f);
        return ESP_OK;
    }

    /* Recognizer loaded and PSRAM headroom OK — proceed with enrollment. */
    s_recognizer->clear_all_feats();

    FILE *f = fopen(DB_TXT_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "DB.TXT not found — recognition disabled");
        face_status_set_last_error("DB.TXT not found");
        return ESP_OK;
    }

    char line[160];
    while (fgets(line, sizeof(line), f) && s_num_persons < MAX_PERSONS) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (!line[0] || line[0] == '#') continue;

        char pid[16]   = {0};
        char name[64]  = {0};
        char afile[64] = {0};
        int  samples   = 0;

        if (sscanf(line, "%15[^,],%63[^,],%63[^,],%d", pid, name, afile, &samples) != 4) {
            ESP_LOGW(TAG, "DB.TXT: skipping bad line: %.80s", line);
            continue;
        }

        PersonEntry &p = s_persons[s_num_persons];
        strlcpy(p.person_id,    pid,  sizeof(p.person_id));
        strlcpy(p.display_name, name, sizeof(p.display_name));
        to_audio_url(afile, p.audio_file, sizeof(p.audio_file));

        enroll_person(p, samples);
        s_num_persons++;
    }
    fclose(f);

    int total = s_recognizer->get_num_feats();
    if (s_num_persons > 0 && total > 0) {
        s_available = true;
        ESP_LOGI(TAG, "Recognition engine ready — %d persons, %d features total",
                 s_num_persons, total);
    } else {
        ESP_LOGW(TAG, "No faces enrolled — recognition disabled "
                 "(persons=%d features=%d)", s_num_persons, total);
    }

    return ESP_OK;
#endif
}

extern "C" bool face_recog_run(const uint16_t *rgb565, int width, int height,
                                const int bbox[4], const int *keypoints,
                                face_recog_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));

#if !FACE_RECOG_AVAILABLE
    return false;
#else
    if (!s_available || !s_recognizer || !result) return false;

    dl::image::img_t img = {
        .data     = const_cast<uint16_t *>(rgb565),
        .width    = static_cast<uint16_t>(width),
        .height   = static_cast<uint16_t>(height),
        /* Use the same pixel mode the detector selected at startup. */
        .pix_type = face_detect_get_byte_swap()
                        ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                        : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };

    dl::detect::result_t det;
    det.category = 0;
    det.score    = 1.0f;
    det.box      = {bbox[0], bbox[1], bbox[2], bbox[3]};
    if (keypoints) {
        det.keypoint = {keypoints[0], keypoints[1], keypoints[2], keypoints[3],
                        keypoints[4], keypoints[5], keypoints[6], keypoints[7],
                        keypoints[8], keypoints[9]};
    }

    std::list<dl::detect::result_t> det_list;
    det_list.push_back(det);

    auto results = s_recognizer->recognize(img, det_list);
    if (results.empty()) {
        ESP_LOGI(TAG, "Unknown person (no match above threshold)");
        return false;
    }

    /* Best match = highest similarity */
    const dl::recognition::result_t *best = &results[0];
    for (const auto &r : results) {
        if (r.similarity > best->similarity) best = &r;
    }

    /* Map DB id → PersonEntry */
    const PersonEntry *person = nullptr;
    for (int i = 0; i < s_num_persons; i++) {
        if (best->id >= s_persons[i].db_id_start && best->id < s_persons[i].db_id_end) {
            person = &s_persons[i];
            break;
        }
    }

    if (!person) {
        ESP_LOGW(TAG, "Recognized id=%u sim=%.2f but no PersonEntry matched",
                 (unsigned)best->id, best->similarity);
        return false;
    }

    ESP_LOGI(TAG, "Recognized %s score=%.2f", person->display_name, best->similarity);

    result->recognized = true;
    strlcpy(result->person_id,    person->person_id,    sizeof(result->person_id));
    strlcpy(result->display_name, person->display_name, sizeof(result->display_name));
    strlcpy(result->audio_file,   person->audio_file,   sizeof(result->audio_file));
    result->confidence = best->similarity;
    return true;
#endif
}
