#pragma once

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared camera access — serialised by the web server's internal mutex.
 * camera_capture_frame() acquires the mutex; camera_release_frame() releases it.
 * Always call release, even after a failed capture attempt that returned a frame. */
esp_err_t camera_capture_frame(camera_fb_t **fb);
void      camera_release_frame(camera_fb_t *fb);

esp_err_t camera_web_server_start(void);

#ifdef __cplusplus
}
#endif
