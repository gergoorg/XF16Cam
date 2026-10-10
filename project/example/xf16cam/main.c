/*
 * Copyright (C) 2017 XRADIO TECHNOLOGY CO., LTD.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "kernel/os/os.h"
#include "common/framework/platform_init.h"
#include "common/framework/net_ctrl.h"
#include "driver/chip/hal_csi_jpeg.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_i2c.h"
#include "driver/chip/hal_prcm.h"
#include "driver/component/csi_camera/camera.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_defs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "xf16cam_config.h"
#include "xf16cam_audio.h"
#include "xf16cam_board.h"
#include "xf16cam_esphome.h"
#include "xf16cam_http.h"
#include "xf16cam_log.h"
#include "xf16cam_media.h"
#include "xf16cam_mqtt.h"
#include "xf16cam_net.h"
#include "xf16cam_rail.h"
#include "xf16cam_rtsp_parser.h"
#include "xf16cam_sensor.h"
#include "xf16cam_storage.h"
#include "xf16cam_version.h"
#include "xf16cam_xip.h"

#define JPEG_ONLINE_EN           (1)
#define JPEG_BUFFER_COUNT        (1)
#define JPEG_MPART_EN            (0)
#define JPEG_BUFF_SIZE           (100 * 1024)
#define JPEG_SRAM_SIZE           \
	(JPEG_BUFFER_COUNT * (JPEG_BUFF_SIZE + CAMERA_JPEG_HEADER_LEN + 1023U))
#define JPEG_IMAGE_WIDTH         (320)
#define JPEG_IMAGE_HEIGHT        (240)
#define XF16CAM_RTSP_PORT        (8554)
#define XF16CAM_RTP_MTU          (1300)
#define XF16CAM_RTP_SSRC         (0x58463136UL)
#define XF16CAM_RTP_AUDIO_SSRC   (0x58463137UL)
#define XF16CAM_RTSP_HANDSHAKE_MS (10000U)
#define XF16CAM_RTSP_IO_TIMEOUT_MS (2000)
#define XF16CAM_RTSP_CLIENT_STACK (3 * 1024)

_Static_assert(JPEG_SRAM_SIZE >= JPEG_BUFFER_COUNT *
	       (JPEG_BUFF_SIZE + CAMERA_JPEG_HEADER_LEN + 1023U),
	       "JPEG arena cannot hold aligned buffers");

#define XF16_SENSOR_I2C_ID       I2C0_ID
#define XF16_CTRL_PORT           GPIO_PORT_A
#define XF16_CTRL_PIN            GPIO_PIN_14
#define XF16_SAME_PIN_PWR(_port, _pin) \
	{ \
		.Reset_Port = (_port), \
		.Reset_Pin = (_pin), \
		.Pwdn_Port = (_port), \
		.Pwdn_Pin = (_pin), \
	}

static void xf16_release_camera_wakeup_hold(void);
static void xf16_camera_ctrl_prehold_low(void);
static int xf16_board_camera_power_prepare(void);
static void xf16_board_camera_power_down(void);

/* The one capture arena, allocated for the life of the device. Until
 * v0.17.16 it was malloc'd on every cold camera acquire and freed on
 * release, and a 104 KB block is the first thing a fragmented heap fails
 * to serve: once any task stack or socket buffer had landed inside the
 * hole the released arena left, no client could capture again ("malloc
 * fail" on every RTSP client). A static array costs the same SRAM as
 * holding the allocation and cannot fail. */
static uint8_t g_capture_arena[JPEG_SRAM_SIZE] __attribute__((aligned(4)));
static CAMERA_Mgmt mem_mgmt;
static OS_Mutex_t g_camera_lock;
static int g_camera_lock_ready;
static int g_camera_initialized;
static volatile uint32_t g_camera_users;	/* read by the board task */
/* The capture arena holds a single JPEG buffer; serialize capture-through-send
 * so a second client can't overwrite it while another client is still reading it. */
static OS_Mutex_t g_capture_lock;
static int g_capture_lock_ready;
typedef struct {
	OS_Thread_t thread;
	volatile int active;
	int fd;
} XF16CamMediaClient;
static XF16CamMediaClient g_mjpeg_clients[XF16CAM_MAX_PARALLEL_CLIENTS];
static XF16CamMediaClient g_rtsp_clients[XF16CAM_MAX_PARALLEL_CLIENTS];
static volatile uint32_t g_mjpeg_active_count;
static volatile uint32_t g_rtsp_active_count;

__xip_text
uint32_t xf16cam_media_active_clients(void)
{
	return g_mjpeg_active_count + g_rtsp_active_count;
}

/* Sessions that hold the camera, as opposed to clients that are merely
 * connected: an RTSP client between connect and PLAY, or one that only
 * pulls audio, is not waiting for frames and must not trip the stall
 * watchdog. */
__xip_text
uint32_t xf16cam_media_capturing(void)
{
	return g_camera_users;
}

static XF16CamMediaInfo g_media_info = {
	.jpeg_capacity = JPEG_BUFF_SIZE,
};

static CAMERA_Cfg camera_cfg = {
	.jpeg_cfg.jpeg_en = 1,
	.jpeg_cfg.quality = 60,
	.jpeg_cfg.jpeg_clk = 0,
	.jpeg_cfg.memPartEn = JPEG_MPART_EN,
	.jpeg_cfg.memPartNum = 0,
	.jpeg_cfg.jpeg_mode = JPEG_ONLINE_EN ? JPEG_MOD_ONLINE : JPEG_MOD_OFFLINE,
	.jpeg_cfg.width = JPEG_IMAGE_WIDTH,
	.jpeg_cfg.height = JPEG_IMAGE_HEIGHT,

	.csi_cfg.csi_clk = 24000000,
	.csi_cfg.hor_start = 0,
	.csi_cfg.ver_start = 0,

	.sensor_cfg.i2c_id = XF16_SENSOR_I2C_ID,
	.sensor_cfg.pwcfg = XF16_SAME_PIN_PWR(XF16_CTRL_PORT, XF16_CTRL_PIN),

	.sensor_func.init = xf16cam_sensor_init,
	.sensor_func.deinit = xf16cam_sensor_deinit,
	.sensor_func.ioctl = NULL,
};

/* XF16 board-specific camera rail and control-pin preparation. */
__xip_text
static void xf16_release_camera_wakeup_hold(void)
{
	uint32_t mask = HAL_BIT(4) | HAL_BIT(5);

	HAL_PRCM_WakeupIODisableCfgHold(mask);
}

__xip_text
static void xf16_camera_ctrl_prehold_low(void)
{
	GPIO_InitParam param;

	param.driving = GPIO_DRIVING_LEVEL_1;
	param.mode = GPIOx_Pn_F1_OUTPUT;
	param.pull = GPIO_PULL_NONE;
	HAL_GPIO_Init(XF16_CTRL_PORT, XF16_CTRL_PIN, &param);
	HAL_GPIO_WritePin(XF16_CTRL_PORT, XF16_CTRL_PIN, GPIO_PIN_LOW);
	OS_MSleep(20);
}

__xip_text
__xip_text
static int xf16_board_camera_power_prepare(void)
{
	if (xf16cam_rail_acquire() != 0)
		return -1;
	xf16_release_camera_wakeup_hold();
	xf16_camera_ctrl_prehold_low();
	return 0;
}

__xip_text
static void xf16_board_camera_power_down(void)
{
	xf16cam_rail_release();
}

/* Fixed, bounded JPEG capture arena. */
__xip_text
static int camera_mem_create(CAMERA_JpegCfg *jpeg_cfg, CAMERA_Mgmt *mgmt)
{
	uint8_t *addr;
	uint8_t *end_addr;
	uint8_t *cursor;
	uint32_t i;
	(void)jpeg_cfg;

	addr = g_capture_arena;
	end_addr = addr + JPEG_SRAM_SIZE;
	printf("xf16cam capture arena: %p -> %p\n", addr, end_addr);

	/* Online JPEG mode does not consume a YUV framebuffer. Still capture keeps
	 * this single buffer immutable until the next capture call; both transports
	 * finish sending the current frame before requesting another. */
	mgmt->yuv_buf.addr = NULL;
	mgmt->yuv_buf.size = 0;
	cursor = addr;
	for (i = 0; i < JPEG_BUFFER_COUNT; i++) {
		mgmt->jpeg_buf[i].addr =
			(uint8_t *)ALIGN_1K((uint32_t)cursor + CAMERA_JPEG_HEADER_LEN);
		mgmt->jpeg_buf[i].size = JPEG_BUFF_SIZE;
		cursor = mgmt->jpeg_buf[i].addr + JPEG_BUFF_SIZE;
		if (cursor > end_addr) {
			printf("jpeg buffer %lu exceeds capture arena\n", (unsigned long)i);
			return -1;
		}
	}
	printf("xf16cam buffers: count=%u bytes_each=%u arena=%u\n",
	       JPEG_BUFFER_COUNT, JPEG_BUFF_SIZE, JPEG_SRAM_SIZE);

	return 0;
}

__xip_text
static void camera_deinit(void)
{
	HAL_CAMERA_DeInit();
	xf16_board_camera_power_down();
}

__xip_text
static int camera_init(void)
{
	const XF16CamConfig *config = xf16cam_config_get();

	camera_cfg.jpeg_cfg.width = config->resolution == XF16CAM_RESOLUTION_VGA ? 640 : 320;
	camera_cfg.jpeg_cfg.height = config->resolution == XF16CAM_RESOLUTION_VGA ? 480 : 240;
	memset(&mem_mgmt, 0, sizeof(mem_mgmt));
	if (camera_mem_create(&camera_cfg.jpeg_cfg, &mem_mgmt) != 0)
		return -1;

	camera_cfg.mgmt = &mem_mgmt;
	if (HAL_CAMERA_Init(&camera_cfg) != HAL_OK) {
		printf("HAL_CAMERA_Init failed\n");
		return -1;
	}
	if (xf16cam_sensor_configure_camera(camera_cfg.jpeg_cfg.width,
	                                  camera_cfg.jpeg_cfg.height) != 0) {
		printf("xf16cam camera: geometry configuration failed\n");
		/* The caller owns the capture arena and shared rail on every init
		 * failure; only unwind the successfully created camera instance here. */
		HAL_CAMERA_DeInit();
		return -1;
	}
	return 0;
}

__xip_text
static int xf16cam_camera_manager_init(void)
{
	if (OS_MutexCreate(&g_camera_lock) != OS_OK)
		return -1;
	g_camera_lock_ready = 1;
	if (OS_MutexCreate(&g_capture_lock) != OS_OK)
		return -1;
	g_capture_lock_ready = 1;
	if (xf16_board_camera_power_prepare() != 0)
		return -1;
	if (camera_init() != 0) {
		xf16_board_camera_power_down();
		return -1;
	}
	g_camera_initialized = 1;
	return 0;
}

__xip_text
static int xf16cam_camera_acquire(void)
{
	int result = -1;

	if (!g_camera_lock_ready || xf16cam_update_active() ||
	    OS_MutexLock(&g_camera_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (!g_camera_initialized) {
		if (xf16_board_camera_power_prepare() != 0)
			goto out;
		if (camera_init() != 0) {
			xf16_board_camera_power_down();
			goto out;
		}
		g_camera_initialized = 1;
	}
	++g_camera_users;
	/* Restart the stall clock. last_frame_ms otherwise still holds the
	 * previous session's final frame, and a client arriving 30 s after
	 * that was rebooted on the first board poll, before the cold sensor
	 * re-init above could produce anything. */
	g_media_info.last_frame_ms = OS_TicksToMSecs(OS_GetTicks());
	result = 0;
out:
	OS_MutexUnlock(&g_camera_lock);
	return result;
}

__xip_text
static void xf16cam_camera_release(void)
{
	if (!g_camera_lock_ready ||
	    OS_MutexLock(&g_camera_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_camera_users > 0)
		--g_camera_users;
	if (g_camera_users == 0 && g_camera_initialized) {
		camera_deinit();
		g_camera_initialized = 0;
		printf("xf16cam camera: idle; capture memory released\n");
	}
	OS_MutexUnlock(&g_camera_lock);
}

__xip_text
static void xf16cam_camera_release_boot_probe(void)
{
	if (!g_camera_lock_ready ||
	    OS_MutexLock(&g_camera_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_camera_users == 0 && g_camera_initialized) {
		camera_deinit();
		g_camera_initialized = 0;
		printf("xf16cam camera: probe complete; hardware idle\n");
	}
	OS_MutexUnlock(&g_camera_lock);
}

typedef struct {
	const uint8_t *scan;
	uint32_t scan_len;
	uint16_t width;
	uint16_t height;
	uint8_t type;
	uint8_t qtables[128];
	uint8_t qtable_mask;
} XF16CamJpeg;

static uint16_t xf16_be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

/* Extract the entropy scan and RFC 2435 metadata from a complete JPEG. */
static int xf16cam_parse_jpeg(const uint8_t *jpeg, uint32_t len, XF16CamJpeg *out)
{
	uint32_t pos = 2;
	int32_t eoi;

	memset(out, 0, sizeof(*out));
	if (len < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
		return -1;

	while (pos + 4 <= len) {
		uint8_t marker;
		uint16_t seg_len;
		const uint8_t *data;
		uint32_t data_len;

		while (pos < len && jpeg[pos] == 0xff)
			pos++;
		if (pos >= len)
			break;
		marker = jpeg[pos++];
		if (marker == 0xd9)
			break;
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
			continue;
		if (pos + 2 > len)
			return -1;
		seg_len = xf16_be16(jpeg + pos);
		if (seg_len < 2 || pos + seg_len > len)
			return -1;
		data = jpeg + pos + 2;
		data_len = seg_len - 2;

		if (marker == 0xdb) {
			uint32_t qpos = 0;
			while (qpos < data_len) {
				uint8_t pq_tq = data[qpos++];
				uint32_t qlen = (pq_tq >> 4) ? 128 : 64;
				uint8_t id = pq_tq & 0x0f;
				if (qpos + qlen > data_len)
					return -1;
				if ((pq_tq >> 4) == 0 && id < 2) {
					memcpy(out->qtables + id * 64, data + qpos, 64);
					out->qtable_mask |= (uint8_t)(1U << id);
				}
				qpos += qlen;
			}
		} else if (marker == 0xc0 && data_len >= 8) {
			out->height = xf16_be16(data + 1);
			out->width = xf16_be16(data + 3);
			/* RFC 2435 type 1 is 4:2:0, type 0 is 4:2:2. */
			out->type = (data[7] == 0x22) ? 1 : 0;
		} else if (marker == 0xda) {
			out->scan = jpeg + pos + seg_len;
			break;
		}
		pos += seg_len;
	}

	if (!out->scan || !out->width || !out->height || out->qtable_mask != 3)
		return -1;
	for (eoi = (int32_t)len - 2; eoi >= (int32_t)(out->scan - jpeg); eoi--) {
		if (jpeg[eoi] == 0xff && jpeg[eoi + 1] == 0xd9) {
			out->scan_len = (uint32_t)((jpeg + eoi) - out->scan);
			return out->scan_len ? 0 : -1;
		}
	}
	/* XR872's encoder length can omit the terminal EOI marker. RTP/JPEG
	 * transports entropy data only, so the complete remaining buffer is valid. */
	out->scan_len = len - (uint32_t)(out->scan - jpeg);
	return out->scan_len ? 0 : -1;
}

static int xf16cam_send_all(int fd, const void *data, uint32_t len)
{
	const uint8_t *p = data;
	while (len) {
		int n = send(fd, p, len, 0);
		if (n <= 0)
			return -1;
		p += n;
		len -= (uint32_t)n;
	}
	return 0;
}

/* Streaming sockets are otherwise transmit-only. A nonblocking receive catches
 * FIN/RST promptly; any client payload is invalid here and also ends the stream.
 * Do not use MSG_PEEK: this lwIP version credits peeked bytes to the TCP window. */
__xip_text
int xf16cam_media_client_connected(int fd)
{
	uint8_t byte;
	int received = recv(fd, &byte, sizeof(byte), MSG_DONTWAIT);

	return received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

/* Still-mode acquisition leaves the returned buffer immutable until the next
 * call. Video mode rotates immediately and can overwrite a slow socket send. */
__xip_text
static int xf16cam_capture_jpeg(CAMERA_JpegBuffInfo *info, uint8_t **jpeg,
				uint32_t *jpeg_len)
{
	xf16cam_sensor_prepare_capture();
	if (HAL_CAMERA_CaptureImage(CAMERA_OUT_JPEG, info, 1) != 0) {
		++g_media_info.capture_errors;
		return -1;
	}
	if (info->buff_index >= JPEG_BUFFER_COUNT || info->size == 0 ||
	    info->size > mem_mgmt.jpeg_buf[info->buff_index].size) {
		++g_media_info.capture_errors;
		printf("xf16cam invalid capture metadata: index=%u len=%lu\n",
		       info->buff_index, (unsigned long)info->size);
		return 1;
	}
	*jpeg = mem_mgmt.jpeg_buf[info->buff_index].addr - CAMERA_JPEG_HEADER_LEN;
	*jpeg_len = info->size + CAMERA_JPEG_HEADER_LEN;
	++g_media_info.frames;
	g_media_info.last_frame_ms = OS_TicksToMSecs(OS_GetTicks());
	if (info->size > g_media_info.largest_jpeg)
		g_media_info.largest_jpeg = info->size;
	return 0;
}

/* One frame for a non-stream consumer (the MQTT publisher). The capture lock
 * stays held until xf16cam_media_snapshot_end() so the single arena is
 * immutable while the caller transmits it; stream clients wait meanwhile, so
 * keep that window bounded. */
__xip_text
int xf16cam_media_snapshot_begin(const uint8_t **jpeg, uint32_t *jpeg_len)
{
	CAMERA_JpegBuffInfo info;
	uint8_t *frame;
	int attempt;

	if (xf16cam_camera_acquire() != 0)
		return -1;
	if (!g_capture_lock_ready ||
	    OS_MutexLock(&g_capture_lock, OS_WAIT_FOREVER) != OS_OK) {
		xf16cam_camera_release();
		return -1;
	}
	for (attempt = 0; attempt < 3; ++attempt) {
		int capture = xf16cam_capture_jpeg(&info, &frame, jpeg_len);

		if (capture == 0) {
			*jpeg = frame;
			return 0;
		}
		if (capture < 0)
			break;
	}
	OS_MutexUnlock(&g_capture_lock);
	xf16cam_camera_release();
	return -1;
}

__xip_text
void xf16cam_media_snapshot_end(void)
{
	OS_MutexUnlock(&g_capture_lock);
	xf16cam_camera_release();
}

/* Keep the camera powered and initialised between snapshots. A 0 -> 1
 * acquire runs the cold sensor init and toggles the shared PA23 rail, which
 * is too much to repeat every few seconds. */
__xip_text
int xf16cam_media_camera_hold(void)
{
	return xf16cam_camera_acquire();
}

__xip_text
void xf16cam_media_camera_unhold(void)
{
	xf16cam_camera_release();
}

__xip_text
const XF16CamMediaInfo *xf16cam_media_info(void)
{
	return &g_media_info;
}

static int xf16cam_mjpeg_stream(int fd)
{
	static const char response[] =
		"HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=xf16frame\r\n"
		"Cache-Control: no-store, no-transform\r\nPragma: no-cache\r\nConnection: close\r\n\r\n";
	static const uint8_t eoi[] = { 0xff, 0xd9 };
	char part[112];

	if (xf16cam_send_all(fd, response, sizeof(response) - 1) != 0)
		return -1;
	printf("xf16cam WEB PLAY: multipart MJPEG\n");
	while (!xf16cam_update_active() && xf16cam_media_client_connected(fd)) {
		CAMERA_JpegBuffInfo info;
		uint8_t *jpeg;
		uint32_t jpeg_len;
		uint32_t eoi_len;
		int capture;
		int length;
		int failed;

		if (!g_capture_lock_ready ||
		    OS_MutexLock(&g_capture_lock, OS_WAIT_FOREVER) != OS_OK)
			break;
		capture = xf16cam_capture_jpeg(&info, &jpeg, &jpeg_len);
		if (capture != 0) {
			OS_MutexUnlock(&g_capture_lock);
			if (capture < 0)
				break;
			continue;
		}
		eoi_len = jpeg_len >= 2 && jpeg[jpeg_len - 2] == 0xff &&
		          jpeg[jpeg_len - 1] == 0xd9 ? 0 : sizeof(eoi);
		length = XF16CAM_XIP_FORMAT(part, sizeof(part),
		                            "--xf16frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
		                            (unsigned long)(jpeg_len + eoi_len));
		failed = length <= 0 || length >= (int)sizeof(part) ||
		         xf16cam_send_all(fd, part, length) != 0 ||
		         xf16cam_send_all(fd, jpeg, jpeg_len) != 0 ||
		         (eoi_len != 0 && xf16cam_send_all(fd, eoi, eoi_len) != 0) ||
		         xf16cam_send_all(fd, "\r\n", 2) != 0;
		OS_MutexUnlock(&g_capture_lock);
		if (failed)
			break;
	}
	printf("xf16cam WEB client stopped\n");
	return 0;
}

static void xf16cam_mjpeg_task(void *arg)
{
	uint32_t slot = (uint32_t)(uintptr_t)arg;
	int fd = g_mjpeg_clients[slot].fd;

	xf16cam_mjpeg_stream(fd);
	closesocket(fd);
	xf16cam_camera_release();
	OS_ThreadSetInvalid(&g_mjpeg_clients[slot].thread);
	__sync_synchronize();
	g_mjpeg_clients[slot].active = 0;
	if (g_mjpeg_active_count > 0)
		__sync_fetch_and_sub(&g_mjpeg_active_count, 1);
	OS_ThreadDelete(NULL);
}

int xf16cam_find_mjpeg_slot(void)
{
	for (int slot = 0; slot < XF16CAM_MAX_PARALLEL_CLIENTS; ++slot) {
		if (__sync_bool_compare_and_swap(&g_mjpeg_clients[slot].active, 0, 1)) {
			return slot;
		}
	}
	return -1;
}

__xip_text
int xf16cam_mjpeg_start(int fd)
{
	int timeout = XF16CAM_RTSP_IO_TIMEOUT_MS;

	if (xf16cam_update_active())
		return -1;
	int slot = xf16cam_find_mjpeg_slot();
	if (slot < 0)
		return -1;
	if (xf16cam_camera_acquire() != 0)
		goto fail_slot;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	g_mjpeg_clients[slot].fd = fd;
	__sync_fetch_and_add(&g_mjpeg_active_count, 1);
	if (OS_ThreadCreate(&g_mjpeg_clients[slot].thread, "xf16cam-mjpeg", xf16cam_mjpeg_task,
	                    (void *)(uintptr_t)slot, OS_THREAD_PRIO_APP, 3 * 1024) != OS_OK) {
		__sync_fetch_and_sub(&g_mjpeg_active_count, 1);
		xf16cam_camera_release();
		goto fail_slot;
	}
	return 0;

fail_slot:
	__sync_synchronize();
	g_mjpeg_clients[slot].active = 0;
	return -1;
}

typedef struct {
	uint8_t video_channel;
	uint8_t audio_channel;
	uint8_t audio_available;
	uint8_t video_setup;
	uint8_t audio_setup;
	uint8_t audio_acquired;
} XF16CamRtspSession;

static int xf16cam_send_rtp_jpeg(int fd, const XF16CamJpeg *jpg,
				 uint16_t *sequence, uint32_t timestamp,
				 uint8_t channel)
{
	uint32_t offset = 0;
	while (offset < jpg->scan_len) {
		uint8_t header[24];
		uint8_t qheader[132];
		uint32_t payload = jpg->scan_len - offset;
		uint32_t extra = offset == 0 ? sizeof(qheader) : 0;
		uint16_t interleaved_len;
		int last;

		if (payload > XF16CAM_RTP_MTU)
			payload = XF16CAM_RTP_MTU;
		last = (offset + payload == jpg->scan_len);
		interleaved_len = (uint16_t)(12 + 8 + extra + payload);
		header[0] = '$'; header[1] = channel;
		header[2] = (uint8_t)(interleaved_len >> 8);
		header[3] = (uint8_t)interleaved_len;
		header[4] = 0x80; header[5] = (uint8_t)(26 | (last ? 0x80 : 0));
		header[6] = (uint8_t)(*sequence >> 8); header[7] = (uint8_t)*sequence;
		header[8] = (uint8_t)(timestamp >> 24); header[9] = (uint8_t)(timestamp >> 16);
		header[10] = (uint8_t)(timestamp >> 8); header[11] = (uint8_t)timestamp;
		header[12] = (uint8_t)(XF16CAM_RTP_SSRC >> 24);
		header[13] = (uint8_t)(XF16CAM_RTP_SSRC >> 16);
		header[14] = (uint8_t)(XF16CAM_RTP_SSRC >> 8);
		header[15] = (uint8_t)XF16CAM_RTP_SSRC;
		header[16] = 0;
		header[17] = (uint8_t)(offset >> 16);
		header[18] = (uint8_t)(offset >> 8);
		header[19] = (uint8_t)offset;
		header[20] = jpg->type; header[21] = 255;
		header[22] = (uint8_t)((jpg->width + 7) / 8);
		header[23] = (uint8_t)((jpg->height + 7) / 8);
		(*sequence)++;

		if (xf16cam_send_all(fd, header, sizeof(header)) != 0)
			return -1;
		if (offset == 0) {
			qheader[0] = 0; qheader[1] = 0;
			qheader[2] = 0; qheader[3] = 128;
			memcpy(qheader + 4, jpg->qtables, 128);
			if (xf16cam_send_all(fd, qheader, sizeof(qheader)) != 0)
				return -1;
		}
		if (xf16cam_send_all(fd, jpg->scan + offset, payload) != 0)
			return -1;
		offset += payload;
	}
	return 0;
}

static int xf16cam_send_rtp_audio(int fd, uint16_t *sequence, uint32_t *cursor,
				  uint8_t channel)
{
	uint8_t packet[4 + 12 + XF16CAM_AUDIO_SAMPLES_PER_PACKET];
	uint32_t timestamp;

	while (xf16cam_audio_read(cursor, packet + 16, &timestamp) > 0) {
		uint16_t interleaved_len = 12 + XF16CAM_AUDIO_SAMPLES_PER_PACKET;

		packet[0] = '$'; packet[1] = channel;
		packet[2] = (uint8_t)(interleaved_len >> 8);
		packet[3] = (uint8_t)interleaved_len;
		packet[4] = 0x80; packet[5] = 0;
		packet[6] = (uint8_t)(*sequence >> 8); packet[7] = (uint8_t)*sequence;
		packet[8] = (uint8_t)(timestamp >> 24); packet[9] = (uint8_t)(timestamp >> 16);
		packet[10] = (uint8_t)(timestamp >> 8); packet[11] = (uint8_t)timestamp;
		packet[12] = (uint8_t)(XF16CAM_RTP_AUDIO_SSRC >> 24);
		packet[13] = (uint8_t)(XF16CAM_RTP_AUDIO_SSRC >> 16);
		packet[14] = (uint8_t)(XF16CAM_RTP_AUDIO_SSRC >> 8);
		packet[15] = (uint8_t)XF16CAM_RTP_AUDIO_SSRC;
		(*sequence)++;
		if (xf16cam_send_all(fd, packet, sizeof(packet)) != 0)
			return -1;
	}
	return 0;
}

__xip_text
static int xf16cam_cseq(const char *request)
{
	const char *value;
	size_t length;
	int cseq = 0;
	size_t i;

	if (xf16cam_rtsp_header_value(request, "CSeq", &value, &length) != 1 ||
	    length == 0)
		return 0;
	for (i = 0; i < length; ++i) {
		int digit = value[i] - '0';
		if (digit < 0 || digit > 9 || cseq > (INT_MAX - digit) / 10)
			return 0;
		cseq = cseq * 10 + digit;
	}
	return cseq;
}

__xip_text
static int xf16cam_rtsp_channels(const char *request, uint8_t fallback,
				 uint8_t *channel)
{
	const char *transport;
	const char *value;
	size_t length;
	size_t offset;
	unsigned int first = 0;
	unsigned int second = 0;
	int digits = 0;

	if (xf16cam_rtsp_header_value(request, "Transport", &transport, &length) != 1)
		return -1;
	for (offset = 0; offset + sizeof("interleaved=") - 1U <= length; ++offset) {
		if (xf16cam_rtsp_contains_ci(transport + offset,
		                              sizeof("interleaved=") - 1U,
		                              "interleaved="))
			break;
	}
	if (offset + sizeof("interleaved=") - 1U > length) {
		*channel = fallback;
		return 0;
	}
	value = transport + offset + sizeof("interleaved=") - 1U;
	length -= offset + sizeof("interleaved=") - 1U;
	while (length && *value >= '0' && *value <= '9') {
		if (first > 255U)
			return -1;
		first = first * 10U + (unsigned int)(*value++ - '0');
		--length;
		digits = 1;
	}
	if (!digits || !length || *value++ != '-')
		return -1;
	--length;
	digits = 0;
	while (length && *value >= '0' && *value <= '9') {
		if (second > 255U)
			return -1;
		second = second * 10U + (unsigned int)(*value++ - '0');
		--length;
		digits = 1;
	}
	if (!digits || first > 254U || second != first + 1U ||
	    (length && *value != ';'))
		return -1;
	*channel = (uint8_t)first;
	return 0;
}

__xip_text
static int xf16cam_rtsp_tcp_transport(const char *request)
{
	const char *value;
	size_t length;

	return xf16cam_rtsp_header_value(request, "Transport", &value, &length) == 1 &&
	       xf16cam_rtsp_contains_ci(value, length, "RTP/AVP/TCP");
}

__xip_text
static int xf16cam_rtsp_channels_overlap(uint8_t first, uint8_t second)
{
	return first <= (unsigned int)second + 1U && second <= (unsigned int)first + 1U;
}

/* Returns 1 for PLAY, 2 for TEARDOWN, and 0 for other requests. */
__xip_text
static int xf16cam_rtsp_reply(int fd, const char *request, const char *ip,
			      uint16_t video_sequence, uint32_t video_timestamp,
			      uint16_t audio_sequence, uint32_t audio_timestamp,
			      XF16CamRtspSession *session)
{
	char response[768];
	char sdp[256];
	int cseq = xf16cam_cseq(request);
	int n;

	if (!strncmp(request, "OPTIONS ", 8)) {
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			               "RTSP/1.0 200 OK\r\nCSeq: %d\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n",
			               cseq);
	} else if (!strncmp(request, "DESCRIBE ", 9)) {
		int sdp_len = XF16CAM_XIP_FORMAT(sdp, sizeof(sdp),
			"v=0\r\no=- 0 0 IN IP4 %s\r\ns=XF16 %s\r\nc=IN IP4 %s\r\nt=0 0\r\n"
			"m=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=control:track1\r\n"
			"%s",
			ip, xf16cam_sensor_name(), ip,
			session->audio_available ?
			"m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\na=control:track2\r\n" : "");
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			               "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Base: rtsp://%s:%u/stream/\r\n"
			               "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
			               cseq, ip, XF16CAM_RTSP_PORT, sdp_len, sdp);
	} else if (!strncmp(request, "SETUP ", 6) && !xf16cam_rtsp_tcp_transport(request)) {
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
		                       "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %d\r\n\r\n", cseq);
	} else if (!strncmp(request, "SETUP ", 6)) {
		int video = strstr(request, "track1") != NULL;
		int audio = strstr(request, "track2") != NULL;
		uint8_t *channel = audio ? &session->audio_channel : &session->video_channel;
		uint8_t requested_channel;
		if (video == audio || (audio && !session->audio_available) ||
		    xf16cam_rtsp_channels(request, audio ? 2 : 0, &requested_channel) != 0 ||
		    (audio && session->video_setup &&
		     xf16cam_rtsp_channels_overlap(requested_channel, session->video_channel)) ||
		    (video && session->audio_setup &&
		     xf16cam_rtsp_channels_overlap(requested_channel, session->audio_channel))) {
			n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			                       "RTSP/1.0 400 Bad Request\r\nCSeq: %d\r\n\r\n", cseq);
		} else {
			if (audio && !session->audio_acquired && xf16cam_audio_acquire() != 0) {
				n = XF16CAM_XIP_FORMAT(response, sizeof(response),
				                       "RTSP/1.0 503 Service Unavailable\r\nCSeq: %d\r\n\r\n",
				                       cseq);
			} else {
				*channel = requested_channel;
				if (audio) {
					session->audio_acquired = 1;
					session->audio_setup = 1;
				} else {
					session->video_setup = 1;
				}
				n = XF16CAM_XIP_FORMAT(response, sizeof(response),
				                       "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 58463136\r\n"
				                       "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n\r\n",
				                       cseq, (unsigned int)*channel,
				                       (unsigned int)*channel + 1U);
			}
		}
	} else if (!strncmp(request, "PLAY ", 5)) {
		if (!session->video_setup) {
			n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			                       "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n",
			                       cseq);
		} else if (session->audio_setup) {
			n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			                       "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 58463136\r\nRange: npt=0.000-\r\n"
			                       "RTP-Info: url=rtsp://%s:%u/stream/track1;seq=%u;rtptime=%lu,"
			                       "url=rtsp://%s:%u/stream/track2;seq=%u;rtptime=%lu\r\n\r\n",
			                       cseq, ip, XF16CAM_RTSP_PORT, video_sequence, (unsigned long)video_timestamp,
			                       ip, XF16CAM_RTSP_PORT, audio_sequence, (unsigned long)audio_timestamp);
		} else {
			n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			                       "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 58463136\r\nRange: npt=0.000-\r\n"
			                       "RTP-Info: url=rtsp://%s:%u/stream/track1;seq=%u;rtptime=%lu\r\n\r\n",
			                       cseq, ip, XF16CAM_RTSP_PORT, video_sequence,
			                       (unsigned long)video_timestamp);
		}
	} else if (!strncmp(request, "TEARDOWN ", 9)) {
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			               "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 58463136\r\n\r\n", cseq);
		if (n > 0)
			xf16cam_send_all(fd, response, (uint32_t)n);
		return 2;
	} else if (!strncmp(request, "GET_PARAMETER ", 14)) {
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
			               "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: 58463136\r\n\r\n", cseq);
	} else {
		n = XF16CAM_XIP_FORMAT(response, sizeof(response),
		                       "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %d\r\n"
		                       "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n",
		                       cseq);
	}
	if (n <= 0 || n >= (int)sizeof(response) ||
	    xf16cam_send_all(fd, response, (uint32_t)n) != 0)
		return -1;
	return !strncmp(request, "PLAY ", 5) && session->video_setup ? 1 : 0;
}

__xip_text
static int xf16cam_rtsp_receive(int fd, XF16CamRtspParser *parser,
				const char **request, int nonblocking)
{
	size_t writable;
	int ready = xf16cam_rtsp_parser_next(parser, request);
	int received;

	if (ready != 0)
		return ready;
	writable = xf16cam_rtsp_parser_writable(parser);
	if (writable == 0)
		return -1;
	received = recv(fd, xf16cam_rtsp_parser_write_ptr(parser), writable,
	                nonblocking ? MSG_DONTWAIT : 0);
	if (received > 0) {
		if (xf16cam_rtsp_parser_commit(parser, (size_t)received) != 0)
			return -1;
		return xf16cam_rtsp_parser_next(parser, request);
	}
	if (received == 0)
		return -1;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return 0;
	return -1;
}

static int xf16cam_stream_client(int fd, const char *ip)
{
	XF16CamRtspParser parser;
	XF16CamRtspSession session = { 0, 2, 0, 0, 0, 0 };
	uint16_t sequence = 1;
	uint16_t audio_sequence = 1;
	uint32_t timestamp = OS_TicksToMSecs(OS_GetTicks()) * 90U;
	uint32_t audio_cursor = xf16cam_audio_cursor();
	uint32_t audio_timestamp = audio_cursor * XF16CAM_AUDIO_SAMPLES_PER_PACKET;
	uint32_t handshake_start = OS_TicksToMSecs(OS_GetTicks());
	int receive_timeout = 1000;
	int send_timeout = XF16CAM_RTSP_IO_TIMEOUT_MS;
	int playing = 0;
	int camera_acquired = 0;

	/* DESCRIBE needs capability, not an active codec. Defer the camera rail,
	 * capture arena, and AMIC until the client actually selects their tracks. */
	session.audio_available = xf16cam_audio_info()->available != 0;
	xf16cam_rtsp_parser_init(&parser);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
	while (!playing && !xf16cam_update_active()) {
		const char *request;
		int action;
		int ready;

		if (OS_TicksToMSecs(OS_GetTicks()) - handshake_start >= XF16CAM_RTSP_HANDSHAKE_MS)
			goto stopped;
		ready = xf16cam_rtsp_receive(fd, &parser, &request, 0);
		if (ready < 0)
			goto stopped;
		if (ready == 0)
			continue;
		if (!strncmp(request, "PLAY ", 5)) {
			if (session.video_setup && !camera_acquired) {
				if (xf16cam_camera_acquire() != 0)
					goto stopped;
				camera_acquired = 1;
			}
			timestamp = OS_TicksToMSecs(OS_GetTicks()) * 90U;
			audio_cursor = xf16cam_audio_cursor();
			audio_timestamp = audio_cursor * XF16CAM_AUDIO_SAMPLES_PER_PACKET;
		}
		action = xf16cam_rtsp_reply(fd, request, ip, sequence, timestamp,
		                              audio_sequence, audio_timestamp, &session);
		xf16cam_rtsp_parser_consume(&parser);
		if (action < 0 || action == 2)
			goto stopped;
		playing = action == 1;
	}
	if (!playing)
		goto stopped;

	printf("xf16cam PLAY: RTP/JPEG%s over RTSP TCP\n",
	       session.audio_setup ? " + PCMU/8000" : "");
	while (!xf16cam_update_active()) {
		CAMERA_JpegBuffInfo info;
		XF16CamJpeg jpg;
		uint8_t *jpeg;
		uint32_t jpeg_len;
		int capture;

		while (1) {
			const char *request;
			int action;
			int ready = xf16cam_rtsp_receive(fd, &parser, &request, 1);

			if (ready < 0)
				goto stopped;
			if (ready == 0)
				break;
			action = xf16cam_rtsp_reply(fd, request, ip, sequence, timestamp,
			                              audio_sequence,
			                              audio_cursor * XF16CAM_AUDIO_SAMPLES_PER_PACKET,
			                              &session);
			xf16cam_rtsp_parser_consume(&parser);
			if (action < 0 || action == 2)
				goto stopped;
		}

		if (!g_capture_lock_ready ||
		    OS_MutexLock(&g_capture_lock, OS_WAIT_FOREVER) != OS_OK)
			break;
		capture = xf16cam_capture_jpeg(&info, &jpeg, &jpeg_len);
		if (capture != 0) {
			OS_MutexUnlock(&g_capture_lock);
			if (capture < 0)
				break;
			continue;
		}
		if (xf16cam_parse_jpeg(jpeg, jpeg_len, &jpg) != 0) {
			OS_MutexUnlock(&g_capture_lock);
			printf("xf16cam invalid jpeg: index=%u len=%lu\n",
			       info.buff_index, (unsigned long)jpeg_len);
			continue;
		}
		if (session.audio_setup &&
		    xf16cam_send_rtp_audio(fd, &audio_sequence, &audio_cursor,
		                             session.audio_channel) != 0) {
			OS_MutexUnlock(&g_capture_lock);
			break;
		}
		if (xf16cam_send_rtp_jpeg(fd, &jpg, &sequence, timestamp,
		                            session.video_channel) != 0) {
			OS_MutexUnlock(&g_capture_lock);
			break;
		}
		OS_MutexUnlock(&g_capture_lock);
		timestamp = OS_TicksToMSecs(OS_GetTicks()) * 90U;
		if (session.audio_setup &&
		    xf16cam_send_rtp_audio(fd, &audio_sequence, &audio_cursor,
		                             session.audio_channel) != 0)
			break;
	}

stopped:
	if (session.audio_acquired)
		xf16cam_audio_release();
	if (camera_acquired)
		xf16cam_camera_release();
	printf("xf16cam client stopped\n");
	return 0;
}

__xip_text
static int xf16cam_rtsp_listener_open(void)
{
	struct sockaddr_in addr;
	int option = 1;
	int server;

	server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0)
		return -1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(XF16CAM_RTSP_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(server, XF16CAM_MAX_PARALLEL_CLIENTS) != 0) {
		closesocket(server);
		return -1;
	}
	return server;
}

static void xf16cam_rtsp_client_task(void *arg)
{
	char ip[16];
	uint32_t slot = (uint32_t)(uintptr_t)arg;
	int fd = g_rtsp_clients[slot].fd;

	XF16CAM_XIP_FORMAT(ip, sizeof(ip), "%s", xf16cam_net_ip());
	xf16cam_stream_client(fd, ip);
	closesocket(fd);
	OS_ThreadSetInvalid(&g_rtsp_clients[slot].thread);
	__sync_synchronize();
	g_rtsp_clients[slot].active = 0;
	if (g_rtsp_active_count > 0)
		__sync_fetch_and_sub(&g_rtsp_active_count, 1);
	OS_ThreadDelete(NULL);
}

static int xf16cam_find_rtsp_slot(void)
{
	for (int slot = 0; slot < XF16CAM_MAX_PARALLEL_CLIENTS; ++slot) {
		if (__sync_bool_compare_and_swap(&g_rtsp_clients[slot].active, 0, 1)) {
			return slot;
		}
	}
	return -1;
}

static void xf16cam_rtsp_server(int server)
{
	char ip[16];

	XF16CAM_XIP_FORMAT(ip, sizeof(ip), "%s", xf16cam_net_ip());
	printf("xf16cam ready: rtsp://%s:%u/stream\n", ip, XF16CAM_RTSP_PORT);
	while (1) {
		int client = accept(server, NULL, NULL);

		if (client < 0)
			continue;
		if (xf16cam_update_active()) {
			closesocket(client);
			continue;
		}
		int slot = xf16cam_find_rtsp_slot();
		if (slot < 0) {
			closesocket(client);
			continue;
		}
		g_rtsp_clients[slot].fd = client;
		__sync_fetch_and_add(&g_rtsp_active_count, 1);
		printf("xf16cam RTSP client connected\n");
		if (OS_ThreadCreate(&g_rtsp_clients[slot].thread, "xf16cam-rtsp-client",
		                    xf16cam_rtsp_client_task, (void *)(uintptr_t)slot,
		                    OS_THREAD_PRIO_APP, XF16CAM_RTSP_CLIENT_STACK) != OS_OK) {
			__sync_fetch_and_sub(&g_rtsp_active_count, 1);
			__sync_synchronize();
			g_rtsp_clients[slot].active = 0;
			closesocket(client);
		}
	}
}

__xip_text
int xf16cam_media_quiesce_for_update(uint32_t timeout_ms)
{
	uint32_t start = OS_TicksToMSecs(OS_GetTicks());

	while (OS_TicksToMSecs(OS_GetTicks()) - start < timeout_ms) {
		if (g_mjpeg_active_count == 0 && g_rtsp_active_count == 0 &&
		    xf16cam_audio_update_ready())
			return 0;
		OS_MSleep(10);
	}
	return -1;
}

static void __attribute__((noreturn)) xf16cam_idle(void)
{
	while (1)
		OS_MSleep(10000);
}

int main(void)
{
	xf16cam_board_check_previous_crash();
	int board_ready;
	int camera_ready;
	int rtsp_server = -1;

	platform_init();
	xf16cam_log_init();	/* console mirror, if XF16CAM_NETLOG */
	printf("xf16cam version %s\n", XF16CAM_VERSION);
	if (xf16cam_rail_init() != 0)
		printf("xf16cam media rail: initialization failed\n");
	if (xf16cam_config_init() != 0)
		printf("xf16cam config: persistence unavailable\n");
	/* The MQTT and ESPHome tasks (one per build) sleep until the network is
	 * up and retire themselves in AP mode, so they can be created before the
	 * network exists. */
	if (xf16cam_mqtt_start() != 0)
		printf("xf16cam mqtt: start failed; management remains available\n");
	if (xf16cam_esphome_start() != 0)
		printf("xf16cam esphome: start failed; management remains available\n");

	/* Probe once so management can report the sensor, then release the rail
	 * after services start. Stream clients reacquire it; the capture arena
	 * is static and stays. */
	camera_ready = xf16cam_camera_manager_init() == 0;
	if (!camera_ready) {
		printf("xf16cam camera unavailable; continuing with management services\n");
	}
	if (xf16cam_audio_start() != 0)
		printf("xf16cam audio: task start failed\n");
	board_ready = xf16cam_board_init() == 0;
	xf16cam_storage_init();
	if (xf16cam_net_start(xf16cam_config_get()) != 0) {
		printf("xf16cam network start failed; console recovery remains available\n");
		if (camera_ready)
			xf16cam_camera_release_boot_probe();
		xf16cam_idle();
	}
	if (camera_ready && xf16cam_config_get()->media_mode == XF16CAM_MEDIA_RTSP) {
		rtsp_server = xf16cam_rtsp_listener_open();
		if (rtsp_server < 0)
			printf("xf16cam RTSP listener failed; management remains available\n");
	}
	if (xf16cam_http_start() != 0) {
		printf("xf16cam management unavailable; console recovery remains available\n");
		if (rtsp_server >= 0)
			closesocket(rtsp_server);
		if (camera_ready)
			xf16cam_camera_release_boot_probe();
		xf16cam_idle();
	}
	if (board_ready)
		xf16cam_board_set_ready();
	if (camera_ready)
		xf16cam_camera_release_boot_probe();
	if (rtsp_server < 0 && camera_ready &&
	    xf16cam_config_get()->media_mode == XF16CAM_MEDIA_RTSP)
		xf16cam_idle();
	if (camera_ready && xf16cam_config_get()->media_mode == XF16CAM_MEDIA_RTSP) {
		xf16cam_rtsp_server(rtsp_server);
	} else if (camera_ready) {
		printf("xf16cam browser video ready: http://%s/stream.mjpeg\n", xf16cam_net_ip());
		xf16cam_idle();
	} else {
		printf("xf16cam management ready without camera: http://%s/\n", xf16cam_net_ip());
		xf16cam_idle();
	}
	return 0;
}
