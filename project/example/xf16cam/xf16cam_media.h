#ifndef XF16CAM_MEDIA_H
#define XF16CAM_MEDIA_H

#include <stdint.h>

typedef struct {
	uint32_t frames;
	uint32_t largest_jpeg;
	uint32_t capture_errors;
	uint32_t jpeg_capacity;
	uint32_t last_frame_ms;
} XF16CamMediaInfo;

#define XF16CAM_MAX_PARALLEL_CLIENTS (3U)

int xf16cam_mjpeg_start(int fd);
int xf16cam_media_client_connected(int fd);
int xf16cam_media_quiesce_for_update(uint32_t timeout_ms);
uint32_t xf16cam_media_active_clients(void);
/* Sessions currently holding the camera (0 while it is powered down). */
uint32_t xf16cam_media_capturing(void);
const XF16CamMediaInfo *xf16cam_media_info(void);
/* Single-frame capture for non-stream consumers. begin() acquires the camera
 * and the capture lock and returns the frame; end() releases both. */
int xf16cam_media_snapshot_begin(const uint8_t **jpeg, uint32_t *jpeg_len);
void xf16cam_media_snapshot_end(void);
int xf16cam_media_camera_hold(void);
void xf16cam_media_camera_unhold(void);

#endif
