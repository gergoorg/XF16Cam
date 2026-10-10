/*
 * ESPHome native API server for Home Assistant. Compiled only with
 * XF16CAM_ESPHOME. See xf16cam_esphome.h for what it exposes.
 *
 * Protocol (api.proto in the ESPHome repository is the specification):
 * TCP 6053, one task, select() over the listener and up to two clients.
 * A client's first byte picks the transport: 0x00 is a plaintext frame
 * (indicator, varint length, varint type, payload), 0x01 a Noise frame
 * (indicator, u16 length, body). Noise bodies are handled by
 * xf16cam_esphome_noise.c; protobuf fields by xf16cam_esphome_proto.c.
 * Messages are built in one transmit buffer whose first 7 bytes are left for
 * whichever frame header applies, so a JPEG chunk is copied once.
 *
 * Without a stored key the server accepts plaintext and, for key
 * provisioning, Noise with the all-zero key; Home Assistant then generates a
 * key and sends NoiseEncryptionSetKeyRequest. With a key only Noise is
 * accepted. The API version advertised is 1.10 on purpose: at 1.14 the
 * client stops sending object ids, at 1.15 it awaits a capabilities reply.
 *
 * All code is __xip_text and every literal is __xip_rodata; see
 * xf16cam_xip.h for why.
 */
#ifdef XF16CAM_ESPHOME

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "kernel/os/os.h"
#include "common/framework/net_ctrl.h"
#include "common/framework/sysinfo.h"
#include "driver/chip/hal_trng.h"
#include "lwip/sockets.h"
/* libmbedtls.a is built with the SDK mini configuration; the structures
 * below must match it. See xf16cam_esphome_mbedtls_config.h. */
#define MBEDTLS_CONFIG_FILE "xf16cam_esphome_mbedtls_config.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "sys/sram_heap.h"

#include "xf16cam_board.h"
#include "xf16cam_config.h"
#include "xf16cam_esphome.h"
#include "xf16cam_esphome_noise.h"
#include "xf16cam_esphome_proto.h"
#include "xf16cam_http.h"
#include "xf16cam_media.h"
#include "xf16cam_net.h"
#include "xf16cam_power.h"
#include "xf16cam_ptz.h"
#include "xf16cam_sensor.h"
#include "xf16cam_storage.h"
#include "xf16cam_version.h"
#include "xf16cam_xip.h"

#define XF16CAM_ESPHOME_STACK_SIZE    (5 * 1024)
#define XF16CAM_ESPHOME_MAX_CLIENTS   (2)
#define XF16CAM_ESPHOME_POLL_MS       (200)
#define XF16CAM_ESPHOME_IO_TIMEOUT_MS (2000)
#define XF16CAM_ESPHOME_IDLE_MS       (150000U)	/* HA pings every 20 s */
#define XF16CAM_ESPHOME_STATE_MS      (60000U)
#define XF16CAM_ESPHOME_STREAM_MS     (5000U)	/* as ESPHome: stop 5 s after the last request */
#define XF16CAM_ESPHOME_CHUNK         (1390)	/* as ESPHome's MAX_BATCH_PACKET_SIZE */
#define XF16CAM_ESPHOME_RX_MAX        (1024)	/* largest inbound payload kept */
#define XF16CAM_ESPHOME_API_MAJOR     (1)
#define XF16CAM_ESPHOME_API_MINOR     (10)
#define XF16CAM_ESPHOME_HELLO_MAX     (32)	/* client hello bytes kept for the prologue */

/* Transmit buffer: 7 bytes of frame header room (3 outer + 4 inner for
 * Noise; the plaintext header is at most 4 for these sizes), the payload,
 * the Noise tag. */
#define FRAME_HEAD        (7)
#define FRAME_PAYLOAD_MAX (XF16CAM_ESPHOME_CHUNK + 32)
#define FRAME_SIZE        (FRAME_HEAD + FRAME_PAYLOAD_MAX + XF16CAM_NOISE_TAG_LEN)
/* Receive buffer: Noise inner header, payload, tag. */
#define RX_SIZE           (4 + XF16CAM_ESPHOME_RX_MAX + XF16CAM_NOISE_TAG_LEN)

#define XF16CAM_ESPHOME_LOG(literal, ...) do { \
	__xip_rodata static const char XF16CAM_XIP_JOIN(g_xip_log_, __LINE__)[] = literal; \
	printf(XF16CAM_XIP_JOIN(g_xip_log_, __LINE__), ##__VA_ARGS__); \
} while (0)

enum {
	MSG_HELLO_REQ = 1, MSG_HELLO_RESP = 2,
	MSG_CONNECT_REQ = 3, MSG_CONNECT_RESP = 4,
	MSG_DISCONNECT_REQ = 5, MSG_DISCONNECT_RESP = 6,
	MSG_PING_REQ = 7, MSG_PING_RESP = 8,
	MSG_DEVICE_INFO_REQ = 9, MSG_DEVICE_INFO_RESP = 10,
	MSG_LIST_REQ = 11, MSG_LIST_BINARY = 12, MSG_LIST_LIGHT = 15,
	MSG_LIST_SENSOR = 16, MSG_LIST_TEXT = 18, MSG_LIST_DONE = 19,
	MSG_SUBSCRIBE_STATES = 20,
	MSG_STATE_BINARY = 21, MSG_STATE_LIGHT = 24, MSG_STATE_SENSOR = 25,
	MSG_STATE_TEXT = 27,
	MSG_LIGHT_CMD = 32,
	MSG_LIST_CAMERA = 43, MSG_CAMERA_IMAGE = 44, MSG_CAMERA_REQ = 45,
	MSG_LIST_BUTTON = 61, MSG_BUTTON_CMD = 62,
	MSG_NOISE_SET_KEY_REQ = 124, MSG_NOISE_SET_KEY_RESP = 125,
};

enum { ENT_LIGHT, ENT_BUTTON, ENT_BINARY, ENT_SENSOR, ENT_TEXT, ENT_CAMERA };
#define ENT_PTZ     (0x01)	/* PTZ builds only */
#define ENT_WEB     (0x02)	/* browser media mode only */
#define ENT_DIAG    (0x04)
#define ENT_CONFIG  (0x08)
#define ENT_OFF     (0x10)	/* created disabled in Home Assistant */

#define ENTITY_CATEGORY_CONFIG      (1)
#define ENTITY_CATEGORY_DIAGNOSTIC  (2)
#define COLOR_MODE_ON_OFF           (1)
#define STATE_CLASS_MEASUREMENT     (1)
#define STATE_CLASS_TOTAL_INCREASING (2)

/* Fixed-size arrays on purpose: a const char * initialiser would leave the
 * literal itself in .rodata, i.e. in the app slot. The entity key is the
 * table index plus one and is what Home Assistant's registry remembers, so
 * append new entities at the end and never reorder. Same set, names and
 * defaults as the MQTT discovery document. */
typedef struct {
	char object_id[10];
	char name[17];
	char icon[21];
	char unit[4];
	char dev_class[12];
	uint8_t kind;
	uint8_t flags;
	uint8_t decimals;
	uint8_t state_class;
} XF16CamEsphomeEntity;

enum {
	KEY_CAMERA = 1, KEY_LED, KEY_IR, KEY_PTZ_UP, KEY_PTZ_DOWN, KEY_PTZ_LEFT,
	KEY_PTZ_RIGHT, KEY_PTZ_HOME, KEY_REBOOT, KEY_CAM, KEY_SD, KEY_MEDIA,
	KEY_STREAM, KEY_RES, KEY_IP, KEY_UP, KEY_TEMP, KEY_HEAP, KEY_BAT,
	KEY_CLIENTS, KEY_BOOT,
};

__xip_rodata static const XF16CamEsphomeEntity g_entities[] = {
	{ "camera",   "Camera",           "",                     "",   "",            ENT_CAMERA, ENT_WEB, 0, 0 },
	{ "led",      "Flash Light",      "mdi:flashlight",       "",   "",            ENT_LIGHT,  0, 0, 0 },
	{ "ir",       "IR Light",         "mdi:lightbulb-night",  "",   "",            ENT_LIGHT,  ENT_PTZ | ENT_OFF, 0, 0 },
	{ "ptz_up",   "PTZ up",           "mdi:arrow-up-bold",    "",   "",            ENT_BUTTON, ENT_PTZ, 0, 0 },
	{ "ptz_down", "PTZ down",         "mdi:arrow-down-bold",  "",   "",            ENT_BUTTON, ENT_PTZ, 0, 0 },
	{ "ptz_left", "PTZ left",         "mdi:arrow-left-bold",  "",   "",            ENT_BUTTON, ENT_PTZ, 0, 0 },
	{ "ptz_right","PTZ right",        "mdi:arrow-right-bold", "",   "",            ENT_BUTTON, ENT_PTZ, 0, 0 },
	{ "ptz_home", "PTZ home",         "mdi:home",             "",   "",            ENT_BUTTON, ENT_PTZ, 0, 0 },
	{ "reboot",   "Reboot",           "",                     "",   "restart",     ENT_BUTTON, ENT_CONFIG, 0, 0 },
	{ "cam",      "Sensor",           "mdi:camera",           "",   "running",     ENT_BINARY, ENT_DIAG | ENT_OFF, 0, 0 },
	{ "sd",       "SD card",          "mdi:micro-sd",         "",   "plug",        ENT_BINARY, ENT_DIAG | ENT_OFF, 0, 0 },
	{ "media",    "Media mode",       "mdi:video-switch",     "",   "",            ENT_TEXT,   ENT_DIAG | ENT_OFF, 0, 0 },
	{ "stream",   "Stream URL",       "mdi:link-variant",     "",   "",            ENT_TEXT,   ENT_DIAG, 0, 0 },
	{ "res",      "Resolution",       "mdi:aspect-ratio",     "",   "",            ENT_TEXT,   ENT_DIAG, 0, 0 },
	{ "ip",       "IP address",       "mdi:ip-network",       "",   "",            ENT_TEXT,   ENT_DIAG, 0, 0 },
	{ "up",       "Uptime",           "mdi:timer-outline",    "s",  "duration",    ENT_SENSOR, ENT_DIAG | ENT_OFF, 0, STATE_CLASS_TOTAL_INCREASING },
	{ "temp",     "Chip temperature", "",                     "\xc2\xb0""C", "temperature", ENT_SENSOR, ENT_DIAG | ENT_OFF, 1, STATE_CLASS_MEASUREMENT },
	{ "heap",     "Free heap",        "mdi:memory",           "B",  "data_size",   ENT_SENSOR, ENT_DIAG | ENT_OFF, 0, STATE_CLASS_MEASUREMENT },
	{ "bat",      "Battery",          "",                     "mV", "voltage",     ENT_SENSOR, ENT_DIAG | ENT_OFF, 0, STATE_CLASS_MEASUREMENT },
	{ "clients",  "Stream clients",   "mdi:account-multiple", "",   "",            ENT_SENSOR, ENT_DIAG | ENT_OFF, 0, STATE_CLASS_MEASUREMENT },
	{ "boot",     "Boot reason",      "mdi:power-cycle",      "",   "",            ENT_TEXT,   ENT_DIAG | ENT_OFF, 0, 0 },
};
#define ENTITY_COUNT (sizeof(g_entities) / sizeof(g_entities[0]))

__xip_rodata static const char g_text_manufacturer[] = "XF16Cam";
__xip_rodata static const char g_text_model_ptz[] = "XR872 PTZ";
__xip_rodata static const char g_text_model_fixed[] = "XR872 A9";
__xip_rodata static const char g_text_web[] = "web";
__xip_rodata static const char g_text_rtsp[] = "rtsp";
__xip_rodata static const char g_text_mac_failure[] = "Handshake MAC failure";
__xip_rodata static const char g_text_bad_indicator[] = "Bad indicator byte";
/* Task states for the console: the image has no other log in the field. */
__xip_rodata static const char g_text_state_off[] = "not started";
__xip_rodata static const char g_text_state_wait[] = "waiting for network";
__xip_rodata static const char g_text_state_listen[] = "listening";
__xip_rodata static const char g_text_state_ap[] = "retired (AP mode)";
__xip_rodata static const char g_text_state_listen_failed[] = "listen failed";
__xip_rodata static const char g_text_state_thread_failed[] = "thread create failed";
__xip_rodata static const uint8_t g_zero_psk[XF16CAM_NOISE_KEY_LEN] = { 0 };

typedef struct {
	int fd;
	XF16CamNoise noise;
	uint32_t last_rx;
	uint32_t stream_until;	/* 0 = no stream */
	uint8_t encrypted;
	uint8_t subscribed;
	uint8_t frame_wanted;
} XF16CamEsphomeClient;

static OS_Thread_t g_esphome_thread;
static XF16CamEsphomeInfo g_info = { .state = g_text_state_off };
static XF16CamEsphomeClient g_clients[XF16CAM_ESPHOME_MAX_CLIENTS];
static int g_listen = -1;
static uint8_t g_frame[FRAME_SIZE];
static uint8_t g_rx[RX_SIZE];
static uint32_t g_fingerprint;
static uint32_t g_last_state;
static volatile uint8_t g_state_dirty;
static uint8_t g_pending_reboot;
static int g_camera_held;

/* ---- crypto primitives for xf16cam_esphome_noise.c ---------------------- */

__xip_text
static int esphome_random(uint8_t *out, size_t len)
{
	while (len > 0) {
		uint32_t words[4];
		size_t n = len < sizeof(words) ? len : sizeof(words);

		if (HAL_TRNG_Extract(0, words) != HAL_OK)
			return -1;
		memcpy(out, words, n);
		out += n;
		len -= n;
	}
	return 0;
}

__xip_text
static int esphome_rng_cb(void *ctx, unsigned char *out, size_t len)
{
	(void)ctx;
	return esphome_random(out, len);
}

__xip_text
static void esphome_reverse32(uint8_t out[32], const uint8_t in[32])
{
	size_t i;

	for (i = 0; i < 32; ++i)
		out[i] = in[31 - i];
}

/* X25519 over the Curve25519 group the WPA3 supplicant already keeps in the
 * image. mbedtls 2.16 cannot export Montgomery points, so the X coordinate is
 * written by hand; RFC 7748 wants it little-endian, mbedtls big-endian. */
__xip_text
static int esphome_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point)
{
	mbedtls_ecp_group grp;
	mbedtls_ecp_point P;
	mbedtls_ecp_point R;
	mbedtls_mpi m;
	uint8_t tmp[32];
	int rc;

	mbedtls_ecp_group_init(&grp);
	mbedtls_ecp_point_init(&P);
	mbedtls_ecp_point_init(&R);
	mbedtls_mpi_init(&m);
	esphome_reverse32(tmp, scalar);
	tmp[31] &= 248;
	tmp[0] &= 127;
	tmp[0] |= 64;
	rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) ||
	     mbedtls_mpi_read_binary(&m, tmp, sizeof(tmp));
	if (rc == 0) {
		if (point == NULL) {
			rc = mbedtls_ecp_copy(&P, &grp.G);
		} else {
			esphome_reverse32(tmp, point);
			tmp[0] &= 127;	/* RFC 7748: ignore the top bit of u */
			rc = mbedtls_mpi_read_binary(&P.X, tmp, sizeof(tmp)) ||
			     mbedtls_mpi_lset(&P.Z, 1);
		}
	}
	if (rc == 0)
		rc = mbedtls_ecp_mul(&grp, &R, &m, &P, esphome_rng_cb, NULL);
	if (rc == 0)
		rc = mbedtls_mpi_write_binary(&R.X, tmp, sizeof(tmp));
	if (rc == 0)
		esphome_reverse32(out, tmp);
	memset(tmp, 0, sizeof(tmp));
	mbedtls_mpi_free(&m);
	mbedtls_ecp_point_free(&R);
	mbedtls_ecp_point_free(&P);
	mbedtls_ecp_group_free(&grp);
	return rc == 0 ? 0 : -1;
}

__xip_text
static void esphome_sha256(const XF16CamNoiseSeg *segs, size_t count, uint8_t out[32])
{
	mbedtls_sha256_context ctx;
	size_t i;

	mbedtls_sha256_init(&ctx);
	mbedtls_sha256_starts_ret(&ctx, 0);
	for (i = 0; i < count; ++i)
		if (segs[i].len != 0)
			mbedtls_sha256_update_ret(&ctx, segs[i].data, segs[i].len);
	mbedtls_sha256_finish_ret(&ctx, out);
	mbedtls_sha256_free(&ctx);
}

__xip_text
static int esphome_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *ad, size_t ad_len,
                                const uint8_t *in, size_t len, uint8_t *out,
                                uint8_t tag[16])
{
	mbedtls_chachapoly_context ctx;
	int rc;

	mbedtls_chachapoly_init(&ctx);
	rc = mbedtls_chachapoly_setkey(&ctx, key) ||
	     mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, ad, ad_len, in, out, tag);
	mbedtls_chachapoly_free(&ctx);
	return rc == 0 ? 0 : -1;
}

__xip_text
static int esphome_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *ad, size_t ad_len,
                                const uint8_t *in, size_t len, const uint8_t tag[16],
                                uint8_t *out)
{
	mbedtls_chachapoly_context ctx;
	int rc;

	mbedtls_chachapoly_init(&ctx);
	rc = mbedtls_chachapoly_setkey(&ctx, key) ||
	     mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce, ad, ad_len, tag, in, out);
	mbedtls_chachapoly_free(&ctx);
	return rc == 0 ? 0 : -1;
}

__xip_rodata static const XF16CamNoiseCrypto g_crypto = {
	esphome_x25519, esphome_random, esphome_sha256,
	esphome_aead_encrypt, esphome_aead_decrypt,
};

/* ---- helpers ----------------------------------------------------------- */

__xip_text
static uint32_t esphome_now(void)
{
	return OS_TicksToMSecs(OS_GetTicks());
}

__xip_text
static int esphome_net_up(void)
{
	return g_wlan_netif != NULL && NETIF_IS_AVAILABLE(g_wlan_netif);
}

__xip_text
static int esphome_ptz_build(void)
{
#ifdef NO_PTZ
	return 0;
#else
	return 1;
#endif
}

__xip_text
static int esphome_web_mode(void)
{
	return xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB;
}

__xip_text
static int esphome_send_all(int fd, const void *data, size_t len)
{
	const uint8_t *bytes = data;

	while (len > 0) {
		int n = send(fd, bytes, len, 0);

		if (n <= 0)
			return -1;
		bytes += n;
		len -= (size_t)n;
	}
	return 0;
}

__xip_text
static int esphome_recv_exact(int fd, void *buf, size_t len)
{
	uint8_t *bytes = buf;

	while (len > 0) {
		int n = recv(fd, bytes, len, 0);

		if (n <= 0)
			return -1;
		bytes += n;
		len -= (size_t)n;
	}
	return 0;
}

__xip_text
static int esphome_drain(int fd, size_t len)
{
	while (len > 0) {
		size_t n = len < sizeof(g_rx) ? len : sizeof(g_rx);

		if (esphome_recv_exact(fd, g_rx, n) != 0)
			return -1;
		len -= n;
	}
	return 0;
}

/* ---- framing ----------------------------------------------------------- */

/* The payload was built at g_frame + FRAME_HEAD. */
__xip_text
static int esphome_send_message(XF16CamEsphomeClient *c, uint32_t type, size_t len)
{
	if (c->encrypted) {
		uint8_t *inner = g_frame + 3;
		size_t total = 4 + len + XF16CAM_NOISE_TAG_LEN;

		inner[0] = (uint8_t)(type >> 8);
		inner[1] = (uint8_t)type;
		inner[2] = (uint8_t)(len >> 8);
		inner[3] = (uint8_t)len;
		if (xf16cam_noise_encrypt(&c->noise, inner, 4 + len, inner) != 0)
			return -1;
		g_frame[0] = 0x01;
		g_frame[1] = (uint8_t)(total >> 8);
		g_frame[2] = (uint8_t)total;
		return esphome_send_all(c->fd, g_frame, 3 + total);
	} else {
		uint8_t header[XF16CAM_PB_FRAME_HEADER_MAX];
		size_t n = xf16cam_pb_frame_header(header, type, len);

		memcpy(g_frame + FRAME_HEAD - n, header, n);
		return esphome_send_all(c->fd, g_frame + FRAME_HEAD - n, n + len);
	}
}

__xip_text
static int esphome_send_built(XF16CamEsphomeClient *c, uint32_t type,
                              const XF16CamPbWriter *w)
{
	if (w->overflow)
		return -1;
	return esphome_send_message(c, type, w->len);
}

__xip_text
static void esphome_begin(XF16CamPbWriter *w)
{
	xf16cam_pb_init(w, g_frame + FRAME_HEAD, FRAME_PAYLOAD_MAX);
}

/* Pre-handshake Noise frame: indicator, u16 length, raw body. */
__xip_text
static int esphome_send_raw_frame(int fd, const void *body, size_t len)
{
	g_frame[0] = 0x01;
	g_frame[1] = (uint8_t)(len >> 8);
	g_frame[2] = (uint8_t)len;
	memcpy(g_frame + 3, body, len);
	return esphome_send_all(fd, g_frame, 3 + len);
}

__xip_text
static int esphome_send_reject(int fd, const char *reason)
{
	size_t len = strlen(reason);

	g_frame[3] = 0x01;
	memcpy(g_frame + 4, reason, len);
	g_frame[0] = 0x01;
	g_frame[1] = (uint8_t)((len + 1) >> 8);
	g_frame[2] = (uint8_t)(len + 1);
	return esphome_send_all(fd, g_frame, 4 + len);
}

/* Reads one message. Returns the payload length with *type and *payload
 * set, -1 on error or close, -2 after draining a message too large for the
 * buffer. */
__xip_text
static int esphome_read_message(XF16CamEsphomeClient *c, uint32_t *type,
                                const uint8_t **payload)
{
	uint8_t head[3];

	if (c->encrypted) {
		size_t total;
		size_t len;

		if (esphome_recv_exact(c->fd, head, sizeof(head)) != 0 || head[0] != 0x01)
			return -1;
		total = (size_t)head[1] << 8 | head[2];
		if (total > sizeof(g_rx)) {
			++g_info.dropped;
			return esphome_drain(c->fd, total) == 0 ? -2 : -1;
		}
		if (total < 4 + XF16CAM_NOISE_TAG_LEN ||
		    esphome_recv_exact(c->fd, g_rx, total) != 0 ||
		    xf16cam_noise_decrypt(&c->noise, g_rx, total, g_rx) != 0)
			return -1;
		*type = (uint32_t)g_rx[0] << 8 | g_rx[1];
		len = (size_t)g_rx[2] << 8 | g_rx[3];
		if (len > total - XF16CAM_NOISE_TAG_LEN - 4)
			len = total - XF16CAM_NOISE_TAG_LEN - 4;
		*payload = g_rx + 4;
		return (int)len;
	} else {
		uint8_t varint[5];
		uint32_t fields[2] = { 0, 0 };
		size_t f;

		if (esphome_recv_exact(c->fd, head, 1) != 0 || head[0] != 0x00)
			return -1;
		for (f = 0; f < 2; ++f) {
			size_t i;
			int n = 0;

			for (i = 0; i < sizeof(varint) && n == 0; ++i) {
				if (esphome_recv_exact(c->fd, varint + i, 1) != 0)
					return -1;
				n = xf16cam_pb_varint_parse(varint, i + 1, &fields[f]);
			}
			if (n <= 0)
				return -1;
		}
		if (fields[0] > XF16CAM_ESPHOME_RX_MAX) {
			++g_info.dropped;
			return esphome_drain(c->fd, fields[0]) == 0 ? -2 : -1;
		}
		if (esphome_recv_exact(c->fd, g_rx, fields[0]) != 0)
			return -1;
		*type = fields[1];
		*payload = g_rx;
		return (int)fields[0];
	}
}

/* ---- handshake --------------------------------------------------------- */

__xip_text
static void esphome_mac_string(char *out, size_t size)
{
	const struct sysinfo *info = sysinfo_get();

	XF16CAM_XIP_FORMAT(out, size, "%02x:%02x:%02x:%02x:%02x:%02x",
	                   info->mac_addr[0], info->mac_addr[1], info->mac_addr[2],
	                   info->mac_addr[3], info->mac_addr[4], info->mac_addr[5]);
}

/* Runs the Noise handshake on a client whose first byte was 0x01. The
 * client hello frame is read first (its bytes are part of the prologue),
 * then the server hello, message 1 and message 2 as described in
 * xf16cam_esphome_noise.h. */
__xip_text
static int esphome_noise_handshake(XF16CamEsphomeClient *c)
{
	const XF16CamConfig *config = xf16cam_config_get();
	uint8_t prologue[sizeof(XF16CAM_NOISE_PROLOGUE) - 1 + 2 + XF16CAM_ESPHOME_HELLO_MAX];
	uint8_t head[3];
	size_t hello_len;
	size_t name_len;
	size_t total;

	if (esphome_recv_exact(c->fd, head, sizeof(head)) != 0 || head[0] != 0x01)
		return -1;
	hello_len = (size_t)head[1] << 8 | head[2];
	if (hello_len > XF16CAM_ESPHOME_HELLO_MAX)
		return -1;
	memcpy(prologue, XF16CAM_NOISE_PROLOGUE, sizeof(XF16CAM_NOISE_PROLOGUE) - 1);
	prologue[sizeof(XF16CAM_NOISE_PROLOGUE) - 1] = head[1];
	prologue[sizeof(XF16CAM_NOISE_PROLOGUE)] = head[2];
	if (hello_len != 0 &&
	    esphome_recv_exact(c->fd, prologue + sizeof(XF16CAM_NOISE_PROLOGUE) + 1, hello_len) != 0)
		return -1;
	xf16cam_noise_init(&c->noise, &g_crypto,
	                   xf16cam_config_esphome_key_set(config) ? config->u.esphome.key : g_zero_psk,
	                   prologue, sizeof(XF16CAM_NOISE_PROLOGUE) + 1 + hello_len);

	/* Server hello: protocol 1, node name, MAC; both NUL-terminated. */
	g_rx[0] = 0x01;
	name_len = strlen(xf16cam_net_hostname());
	memcpy(g_rx + 1, xf16cam_net_hostname(), name_len + 1);
	esphome_mac_string((char *)g_rx + 2 + name_len, 18);
	if (esphome_send_raw_frame(c->fd, g_rx, 2 + name_len + 18) != 0)
		return -1;

	/* Message 1: status byte then the 48-byte Noise message. */
	if (esphome_recv_exact(c->fd, head, sizeof(head)) != 0 || head[0] != 0x01)
		return -1;
	total = (size_t)head[1] << 8 | head[2];
	if (total != 1 + XF16CAM_NOISE_MSG_LEN || esphome_recv_exact(c->fd, g_rx, total) != 0 ||
	    g_rx[0] != 0x00)
		return -1;
	if (xf16cam_noise_read_msg1(&c->noise, g_rx + 1, XF16CAM_NOISE_MSG_LEN) != 0) {
		++g_info.rejected;
		esphome_send_reject(c->fd, g_text_mac_failure);
		XF16CAM_ESPHOME_LOG("xf16cam esphome: handshake rejected (wrong key)\n");
		return -1;
	}
	g_rx[0] = 0x00;
	if (xf16cam_noise_write_msg2(&c->noise, g_rx + 1) != XF16CAM_NOISE_MSG_LEN ||
	    esphome_send_raw_frame(c->fd, g_rx, 1 + XF16CAM_NOISE_MSG_LEN) != 0)
		return -1;
	c->encrypted = 1;
	return 0;
}

/* ---- messages ---------------------------------------------------------- */

__xip_text
static int esphome_send_empty(XF16CamEsphomeClient *c, uint32_t type)
{
	return esphome_send_message(c, type, 0);
}

__xip_text
static int esphome_send_hello(XF16CamEsphomeClient *c)
{
	XF16CamPbWriter w;
	char info[32];

	XF16CAM_XIP_FORMAT(info, sizeof(info), "XF16Cam %s", XF16CAM_VERSION);
	esphome_begin(&w);
	xf16cam_pb_uint(&w, 1, XF16CAM_ESPHOME_API_MAJOR);
	xf16cam_pb_uint(&w, 2, XF16CAM_ESPHOME_API_MINOR);
	xf16cam_pb_string(&w, 3, info);
	xf16cam_pb_string(&w, 4, xf16cam_net_hostname());
	return esphome_send_built(c, MSG_HELLO_RESP, &w);
}

__xip_text
static int esphome_send_device_info(XF16CamEsphomeClient *c)
{
	XF16CamPbWriter w;
	const char *host = xf16cam_net_hostname();
	char mac[18];
	char friendly[24];
	int ptz = esphome_ptz_build();

	esphome_mac_string(mac, sizeof(mac));
	XF16CAM_XIP_FORMAT(friendly, sizeof(friendly), "XF16Cam %s", host + 8);
	esphome_begin(&w);
	xf16cam_pb_string(&w, 2, host);
	xf16cam_pb_string(&w, 3, mac);
	xf16cam_pb_string(&w, 4, XF16CAM_VERSION);
	/* No project_name (8/9): Home Assistant would then show the project id
	 * split into manufacturer and model instead of these, and the device
	 * card would differ from the MQTT one. */
	xf16cam_pb_string(&w, 6, ptz ? g_text_model_ptz : g_text_model_fixed);
	xf16cam_pb_uint(&w, 10, 80);			/* webserver_port: "Visit device" */
	xf16cam_pb_string(&w, 12, g_text_manufacturer);
	xf16cam_pb_string(&w, 13, friendly);
	xf16cam_pb_uint(&w, 19, 1);			/* api_encryption_supported */
	if (!xf16cam_config_esphome_key_set(xf16cam_config_get()))
		xf16cam_pb_uint(&w, 26, 1);		/* api_encryption_provisionable */
	return esphome_send_built(c, MSG_DEVICE_INFO_RESP, &w);
}

__xip_text
static int esphome_entity_present(const XF16CamEsphomeEntity *e)
{
	if ((e->flags & ENT_PTZ) && !esphome_ptz_build())
		return 0;
	if ((e->flags & ENT_WEB) && !esphome_web_mode())
		return 0;
	return 1;
}

__xip_text
static int esphome_send_entities(XF16CamEsphomeClient *c)
{
	size_t i;

	for (i = 0; i < ENTITY_COUNT; ++i) {
		const XF16CamEsphomeEntity *e = &g_entities[i];
		uint32_t category = (e->flags & ENT_CONFIG) ? ENTITY_CATEGORY_CONFIG :
		                    (e->flags & ENT_DIAG) ? ENTITY_CATEGORY_DIAGNOSTIC : 0;
		uint32_t off = (e->flags & ENT_OFF) != 0;
		XF16CamPbWriter w;
		uint32_t type;

		if (!esphome_entity_present(e))
			continue;
		esphome_begin(&w);
		xf16cam_pb_string(&w, 1, e->object_id);
		xf16cam_pb_fixed32(&w, 2, (uint32_t)i + 1);
		xf16cam_pb_string(&w, 3, e->name);
		switch (e->kind) {
		case ENT_LIGHT:
			type = MSG_LIST_LIGHT;
			xf16cam_pb_uint(&w, 12, COLOR_MODE_ON_OFF);
			xf16cam_pb_uint(&w, 13, off);
			xf16cam_pb_string(&w, 14, e->icon);
			xf16cam_pb_uint(&w, 15, category);
			break;
		case ENT_BUTTON:
			type = MSG_LIST_BUTTON;
			xf16cam_pb_string(&w, 5, e->icon);
			xf16cam_pb_uint(&w, 6, off);
			xf16cam_pb_uint(&w, 7, category);
			xf16cam_pb_string(&w, 8, e->dev_class);
			break;
		case ENT_BINARY:
			type = MSG_LIST_BINARY;
			xf16cam_pb_string(&w, 5, e->dev_class);
			xf16cam_pb_uint(&w, 7, off);
			xf16cam_pb_string(&w, 8, e->icon);
			xf16cam_pb_uint(&w, 9, category);
			break;
		case ENT_SENSOR:
			type = MSG_LIST_SENSOR;
			xf16cam_pb_string(&w, 5, e->icon);
			xf16cam_pb_string(&w, 6, e->unit);
			xf16cam_pb_uint(&w, 7, e->decimals);
			xf16cam_pb_string(&w, 9, e->dev_class);
			xf16cam_pb_uint(&w, 10, e->state_class);
			xf16cam_pb_uint(&w, 12, off);
			xf16cam_pb_uint(&w, 13, category);
			break;
		case ENT_TEXT:
			type = MSG_LIST_TEXT;
			xf16cam_pb_string(&w, 5, e->icon);
			xf16cam_pb_uint(&w, 6, off);
			xf16cam_pb_uint(&w, 7, category);
			break;
		default:
			type = MSG_LIST_CAMERA;
			xf16cam_pb_uint(&w, 5, off);
			xf16cam_pb_string(&w, 6, e->icon);
			xf16cam_pb_uint(&w, 7, category);
			break;
		}
		if (esphome_send_built(c, type, &w) != 0)
			return -1;
	}
	return esphome_send_empty(c, MSG_LIST_DONE);
}

/* The same sources /api/system reads. Battery is the last measured value;
 * measuring takes the shared ADC for up to a second and belongs to the
 * user's Measure button. */
__xip_text
static int esphome_send_state(XF16CamEsphomeClient *c, size_t index)
{
	const XF16CamEsphomeEntity *e = &g_entities[index];
	uint32_t key = (uint32_t)index + 1;
	XF16CamPbWriter w;
	char text[48];
	const char *value = text;
	float number = 0.0f;
	int missing = 0;
	int on = 0;

	text[0] = 0;
	switch (key) {
	case KEY_LED: on = xf16cam_board_get_led_on() != 0; break;
	case KEY_IR: on = xf16cam_board_get_ir_led_on() != 0; break;
	case KEY_CAM: on = xf16cam_sensor_available() != 0; break;
	case KEY_SD: on = xf16cam_storage_info()->mounted != 0; break;
	case KEY_MEDIA: value = esphome_web_mode() ? g_text_web : g_text_rtsp; break;
	case KEY_STREAM:
		if (esphome_web_mode())
			XF16CAM_XIP_FORMAT(text, sizeof(text), "http://%s/stream.mjpeg", xf16cam_net_ip());
		else
			XF16CAM_XIP_FORMAT(text, sizeof(text), "rtsp://%s:8554/stream", xf16cam_net_ip());
		break;
	case KEY_RES:
		XF16CAM_XIP_FORMAT(text, sizeof(text), "%ux%u",
		                   (unsigned int)xf16cam_sensor_width(),
		                   (unsigned int)xf16cam_sensor_height());
		break;
	case KEY_IP: value = xf16cam_net_ip(); break;
	case KEY_UP: number = (float)(esphome_now() / 1000U); break;
	case KEY_TEMP: {
		int tenths = xf16cam_http_chip_temperature();

		missing = tenths == INT_MIN;
		number = (float)tenths / 10.0f;
		break;
	}
	case KEY_HEAP: number = (float)sram_free_heap_size(); break;
	case KEY_BAT: {
		const XF16CamPowerInfo *power = xf16cam_power_info();

		missing = !power->valid;
		number = (float)power->millivolts;
		break;
	}
	case KEY_CLIENTS: number = (float)xf16cam_media_active_clients(); break;
	case KEY_BOOT: value = xf16cam_http_boot_reason(); break;
	default: return 0;	/* buttons and the camera have no state */
	}

	esphome_begin(&w);
	xf16cam_pb_fixed32(&w, 1, key);
	switch (e->kind) {
	case ENT_LIGHT:
		xf16cam_pb_uint(&w, 2, (uint32_t)on);
		xf16cam_pb_uint(&w, 11, COLOR_MODE_ON_OFF);
		return esphome_send_built(c, MSG_STATE_LIGHT, &w);
	case ENT_BINARY:
		xf16cam_pb_uint(&w, 2, (uint32_t)on);
		return esphome_send_built(c, MSG_STATE_BINARY, &w);
	case ENT_SENSOR:
		if (missing)
			xf16cam_pb_uint(&w, 3, 1);
		else
			xf16cam_pb_float(&w, 2, number);
		return esphome_send_built(c, MSG_STATE_SENSOR, &w);
	default:
		xf16cam_pb_string(&w, 2, value);
		return esphome_send_built(c, MSG_STATE_TEXT, &w);
	}
}

__xip_text
static int esphome_send_states(XF16CamEsphomeClient *c)
{
	size_t i;

	for (i = 0; i < ENTITY_COUNT; ++i) {
		if (!esphome_entity_present(&g_entities[i]))
			continue;
		if (esphome_send_state(c, i) != 0)
			return -1;
	}
	++g_info.states;
	return 0;
}

__xip_text
static uint32_t esphome_fingerprint(void)
{
	return (uint32_t)(xf16cam_board_get_led_on() != 0) |
	       (uint32_t)(xf16cam_board_get_ir_led_on() != 0) << 1 |
	       (uint32_t)(xf16cam_storage_info()->mounted != 0) << 2 |
	       (uint32_t)(xf16cam_sensor_available() != 0) << 3 |
	       xf16cam_media_active_clients() << 4;
}

/* One JPEG as CameraImageResponse chunks; the last carries done. */
__xip_text
static int esphome_send_frame(XF16CamEsphomeClient *c)
{
	const uint8_t *jpeg;
	uint32_t jpeg_len;
	uint32_t offset = 0;
	int failed = 0;

	if (xf16cam_media_snapshot_begin(&jpeg, &jpeg_len) != 0) {
		XF16CAM_ESPHOME_LOG("xf16cam esphome: snapshot unavailable\n");
		return 0;
	}
	while (offset < jpeg_len && !failed) {
		uint32_t n = jpeg_len - offset < XF16CAM_ESPHOME_CHUNK ?
		             jpeg_len - offset : XF16CAM_ESPHOME_CHUNK;
		XF16CamPbWriter w;

		esphome_begin(&w);
		xf16cam_pb_fixed32(&w, 1, KEY_CAMERA);
		xf16cam_pb_bytes(&w, 2, jpeg + offset, n);
		offset += n;
		xf16cam_pb_uint(&w, 3, offset == jpeg_len);
		failed = esphome_send_built(c, MSG_CAMERA_IMAGE, &w) != 0;
	}
	xf16cam_media_snapshot_end();
	if (failed)
		return -1;
	++g_info.frames;
	return 0;
}

__xip_text
static void esphome_handle_light(const uint8_t *payload, size_t len)
{
	XF16CamPbReader r;
	uint32_t key = 0;
	int has_state = 0;
	int state = 0;

	xf16cam_pb_reader_init(&r, payload, len);
	while (xf16cam_pb_next(&r) == 1) {
		if (r.field == 1)
			key = r.value;
		else if (r.field == 2)
			has_state = r.value != 0;
		else if (r.field == 3)
			state = r.value != 0;
	}
	if (!has_state)
		return;
	if (key == KEY_LED)
		xf16cam_board_set_led(state);
	else if (key == KEY_IR && esphome_ptz_build())
		xf16cam_board_set_ir_led(state);
	else
		return;
	g_state_dirty = 1;
}

__xip_text
static void esphome_handle_button(const uint8_t *payload, size_t len)
{
	XF16CamPbReader r;
	uint32_t key = 0;

	xf16cam_pb_reader_init(&r, payload, len);
	while (xf16cam_pb_next(&r) == 1)
		if (r.field == 1)
			key = r.value;
	switch (key) {
#ifndef NO_PTZ
	case KEY_PTZ_UP: ptz_move_up(); break;
	case KEY_PTZ_DOWN: ptz_move_down(); break;
	case KEY_PTZ_LEFT: ptz_move_left(); break;
	case KEY_PTZ_RIGHT: ptz_move_right(); break;
	case KEY_PTZ_HOME: ptz_move_home(); break;
#endif
	case KEY_REBOOT: g_pending_reboot = 1; break;
	default: break;
	}
}

__xip_text
static int esphome_handle_set_key(XF16CamEsphomeClient *c, const uint8_t *payload, size_t len)
{
	XF16CamPbReader r;
	XF16CamPbWriter w;
	const uint8_t *key = NULL;
	uint8_t raw[XF16CAM_ESPHOME_KEY_LEN];
	size_t key_len = 0;
	size_t raw_len = 0;
	int ok;

	xf16cam_pb_reader_init(&r, payload, len);
	while (xf16cam_pb_next(&r) == 1) {
		if (r.field == 1 && r.wire == 2) {
			key = r.data;
			key_len = r.data_len;
		}
	}
	/* As ESPHome: the field carries the key's base64 text (44 bytes for a
	 * 32-byte key), which is what Home Assistant sends; empty clears. */
	if (key_len == 0)
		ok = xf16cam_config_save_esphome_key(NULL) == 0;
	else
		ok = mbedtls_base64_decode(raw, sizeof(raw), &raw_len, key, key_len) == 0 &&
		     raw_len == XF16CAM_ESPHOME_KEY_LEN &&
		     xf16cam_config_save_esphome_key(raw) == 0;
	memset(raw, 0, sizeof(raw));
	XF16CAM_ESPHOME_LOG("xf16cam esphome: key %s %s\n", key_len != 0 ? "set" : "cleared",
	                    ok ? "ok" : "failed");
	esphome_begin(&w);
	xf16cam_pb_uint(&w, 1, (uint32_t)ok);
	return esphome_send_built(c, MSG_NOISE_SET_KEY_RESP, &w);
}

/* Returns -1 when the connection must close. */
__xip_text
static int esphome_handle(XF16CamEsphomeClient *c, uint32_t type,
                          const uint8_t *payload, size_t len)
{
	switch (type) {
	case MSG_HELLO_REQ:
		return esphome_send_hello(c);
	case MSG_CONNECT_REQ:
		/* Pre-2026.1 clients still log in; there is no password to check. */
		return esphome_send_empty(c, MSG_CONNECT_RESP);
	case MSG_DISCONNECT_REQ:
		esphome_send_empty(c, MSG_DISCONNECT_RESP);
		return -1;
	case MSG_PING_REQ:
		return esphome_send_empty(c, MSG_PING_RESP);
	case MSG_DEVICE_INFO_REQ:
		return esphome_send_device_info(c);
	case MSG_LIST_REQ:
		return esphome_send_entities(c);
	case MSG_SUBSCRIBE_STATES:
		c->subscribed = 1;
		return esphome_send_states(c);
	case MSG_LIGHT_CMD:
		++g_info.commands;
		esphome_handle_light(payload, len);
		return 0;
	case MSG_BUTTON_CMD:
		++g_info.commands;
		esphome_handle_button(payload, len);
		return 0;
	case MSG_CAMERA_REQ: {
		XF16CamPbReader r;

		xf16cam_pb_reader_init(&r, payload, len);
		while (xf16cam_pb_next(&r) == 1) {
			if ((r.field == 1 || r.field == 2) && r.value != 0)
				c->frame_wanted = 1;
			if (r.field == 2 && r.value != 0)
				c->stream_until = esphome_now() + XF16CAM_ESPHOME_STREAM_MS;
		}
		return 0;
	}
	case MSG_NOISE_SET_KEY_REQ:
		return esphome_handle_set_key(c, payload, len);
	default:
		return 0;	/* logs, HA states and services subscriptions, time */
	}
}

/* ---- connections ------------------------------------------------------- */

__xip_text
static void esphome_close_client(XF16CamEsphomeClient *c)
{
	if (c->fd < 0)
		return;
	closesocket(c->fd);
	c->fd = -1;
	if (c->subscribed || c->encrypted)
		++g_info.disconnects;
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

__xip_text
static void esphome_refresh_info(void)
{
	size_t i;

	g_info.clients = 0;
	g_info.encrypted = 0;
	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
		if (g_clients[i].fd < 0)
			continue;
		++g_info.clients;
		if (g_clients[i].encrypted)
			g_info.encrypted = 1;
	}
}

__xip_text
static void esphome_accept(void)
{
	struct sockaddr_in address;
	socklen_t address_len = sizeof(address);
	XF16CamEsphomeClient *c = NULL;
	int timeout = XF16CAM_ESPHOME_IO_TIMEOUT_MS;
	uint8_t first;
	size_t i;
	int fd;

	fd = accept(g_listen, (struct sockaddr *)&address, &address_len);
	if (fd < 0)
		return;
	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
		if (g_clients[i].fd < 0) {
			c = &g_clients[i];
			break;
		}
	}
	if (c == NULL) {
		closesocket(fd);	/* both slots busy; the client backs off */
		return;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	/* The first byte selects the transport; it is left in the socket for
	 * the frame reader. */
	if (recv(fd, &first, 1, MSG_PEEK) != 1) {
		esphome_close_client(c);
		return;
	}
	if (first == 0x01) {
		if (esphome_noise_handshake(c) != 0) {
			esphome_close_client(c);
			return;
		}
	} else if (xf16cam_config_esphome_key_set(xf16cam_config_get())) {
		/* A keyed device does not talk plaintext; the 0x01 reply tells
		 * the client that encryption is required. */
		esphome_send_reject(fd, g_text_bad_indicator);
		esphome_close_client(c);
		return;
	}
	c->last_rx = esphome_now();
	++g_info.connects;
	esphome_refresh_info();
	XF16CAM_ESPHOME_LOG("xf16cam esphome: client %s connected (%s)\n",
	                    inet_ntoa(address.sin_addr),
	                    c->encrypted ? "encrypted" : "plaintext");
}

__xip_text
static void esphome_unhold_camera(void)
{
	if (g_camera_held) {
		xf16cam_media_camera_unhold();
		g_camera_held = 0;
	}
}

__xip_text
static void esphome_service_camera(uint32_t now)
{
	int streaming = 0;
	size_t i;

	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
		XF16CamEsphomeClient *c = &g_clients[i];

		if (c->fd < 0)
			continue;
		if (c->stream_until != 0 && (int32_t)(now - c->stream_until) >= 0)
			c->stream_until = 0;
		if (c->stream_until != 0)
			streaming = 1;
	}
	/* Hold the rail between frames while a stream is live, as the MQTT
	 * publisher does for short intervals; the 5 s expiry releases it so the
	 * stall watchdog never sees a held idle camera. */
	if (streaming && !g_camera_held)
		g_camera_held = xf16cam_media_camera_hold() == 0;
	else if (!streaming)
		esphome_unhold_camera();
	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
		XF16CamEsphomeClient *c = &g_clients[i];

		if (c->fd < 0 || !c->frame_wanted)
			continue;
		c->frame_wanted = 0;
		if (esphome_web_mode() && esphome_send_frame(c) != 0)
			esphome_close_client(c);
	}
}

__xip_text
static void esphome_service_states(uint32_t now)
{
	uint32_t current = esphome_fingerprint();
	size_t i;

	if (current == g_fingerprint && !g_state_dirty &&
	    now - g_last_state < XF16CAM_ESPHOME_STATE_MS)
		return;
	g_fingerprint = current;
	g_state_dirty = 0;
	g_last_state = now;
	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
		XF16CamEsphomeClient *c = &g_clients[i];

		if (c->fd >= 0 && c->subscribed && esphome_send_states(c) != 0)
			esphome_close_client(c);
	}
}

__xip_text
static void esphome_close_all(void)
{
	size_t i;

	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i)
		esphome_close_client(&g_clients[i]);
	esphome_unhold_camera();
	esphome_refresh_info();
}

/* Runs until the link fails. */
__xip_text
static void esphome_session(void)
{
	g_fingerprint = esphome_fingerprint();
	g_last_state = esphome_now();
	for (;;) {
		struct timeval tv;
		fd_set readable;
		int maxfd = g_listen;
		uint32_t now;
		size_t i;

		if (!esphome_net_up())
			return;
		FD_ZERO(&readable);
		FD_SET(g_listen, &readable);
		for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
			if (g_clients[i].fd < 0)
				continue;
			FD_SET(g_clients[i].fd, &readable);
			if (g_clients[i].fd > maxfd)
				maxfd = g_clients[i].fd;
		}
		tv.tv_sec = 0;
		tv.tv_usec = XF16CAM_ESPHOME_POLL_MS * 1000;
		if (select(maxfd + 1, &readable, NULL, NULL, &tv) < 0)
			return;
		if (FD_ISSET(g_listen, &readable))
			esphome_accept();
		now = esphome_now();
		for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
			XF16CamEsphomeClient *c = &g_clients[i];
			const uint8_t *payload;
			uint32_t type;
			int len;

			if (c->fd < 0 || !FD_ISSET(c->fd, &readable))
				continue;
			len = esphome_read_message(c, &type, &payload);
			if (len == -1 ||
			    (len >= 0 && esphome_handle(c, type, payload, (size_t)len) != 0)) {
				esphome_close_client(c);
				esphome_refresh_info();
				continue;
			}
			c->last_rx = now;
		}
		for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i) {
			XF16CamEsphomeClient *c = &g_clients[i];

			if (c->fd >= 0 && now - c->last_rx > XF16CAM_ESPHOME_IDLE_MS) {
				XF16CAM_ESPHOME_LOG("xf16cam esphome: client idle, dropped\n");
				esphome_close_client(c);
				esphome_refresh_info();
			}
		}
		if (g_pending_reboot) {
			g_pending_reboot = 0;
			XF16CAM_ESPHOME_LOG("xf16cam esphome: reboot command\n");
			esphome_close_all();
			xf16cam_board_reboot();	/* returns only when deferred */
			continue;
		}
		if (xf16cam_update_active()) {
			/* Release the rail for the OTA/reboot path; pings still
			 * get answered so the client does not reconnect mid-update. */
			esphome_unhold_camera();
			continue;
		}
		esphome_service_camera(now);
		esphome_service_states(now);
	}
}

/* The hostname exists only once xf16cam_net_start() has run, so the listener
 * is opened on first contact with the network rather than at task creation.
 * Returns 0 when the server should run, -1 when it should retire. */
__xip_text
static int esphome_prepare(void)
{
	struct sockaddr_in address;
	int option = 1;

	if (xf16cam_net_mode() != XF16CAM_WIFI_STA) {
		XF16CAM_ESPHOME_LOG("xf16cam esphome: not in station mode; server retired\n");
		g_info.state = g_text_state_ap;
		return -1;
	}
	if (g_listen >= 0) {
		g_info.state = g_text_state_listen;
		return 0;
	}
	g_info.state = g_text_state_listen_failed;
	g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listen < 0)
		return -1;
	setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(XF16CAM_ESPHOME_PORT);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(g_listen, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(g_listen, XF16CAM_ESPHOME_MAX_CLIENTS) != 0) {
		closesocket(g_listen);
		g_listen = -1;
		XF16CAM_ESPHOME_LOG("xf16cam esphome: listen failed; server retired\n");
		return -1;
	}
	XF16CAM_ESPHOME_LOG("xf16cam esphome: API on port %u, id %s, key %s\n",
	                    (unsigned int)XF16CAM_ESPHOME_PORT, xf16cam_net_hostname(),
	                    xf16cam_config_esphome_key_set(xf16cam_config_get()) ?
	                    "set" : "not set (provisionable)");
	g_info.state = g_text_state_listen;
	return 0;
}

__xip_text
static void xf16cam_esphome_task(void *arg)
{
	(void)arg;
	for (;;) {
		while (!esphome_net_up())
			OS_MSleep(1000);
		if (esphome_prepare() != 0)
			break;
		esphome_session();
		esphome_close_all();
		g_info.state = g_text_state_wait;
		XF16CAM_ESPHOME_LOG("xf16cam esphome: link lost\n");
	}
	/* AP mode or no listener: give the stack back to the heap. */
	g_info.enabled = 0;
	OS_ThreadSetInvalid(&g_esphome_thread);
	OS_ThreadDelete(NULL);
}

/* ---- public ------------------------------------------------------------ */

/* Called from main() before the network exists; the task sleeps until the
 * link is up and retires itself in AP mode. */
__xip_text
int xf16cam_esphome_start(void)
{
	size_t i;

	for (i = 0; i < XF16CAM_ESPHOME_MAX_CLIENTS; ++i)
		g_clients[i].fd = -1;
	g_info.enabled = 1;
	g_info.state = g_text_state_wait;
	if (OS_ThreadCreate(&g_esphome_thread, "xf16cam-esphome", xf16cam_esphome_task,
	                    NULL, OS_THREAD_PRIO_APP, XF16CAM_ESPHOME_STACK_SIZE) != OS_OK) {
		g_info.enabled = 0;
		g_info.state = g_text_state_thread_failed;
		XF16CAM_ESPHOME_LOG("xf16cam esphome: thread create failed\n");
		return -1;
	}
	return 0;
}

__xip_text
const XF16CamEsphomeInfo *xf16cam_esphome_info(void)
{
	return &g_info;
}

__xip_text
uint32_t xf16cam_esphome_stack_min_free(void)
{
	return g_info.enabled ? OS_ThreadGetStackMinFreeSize(&g_esphome_thread) : 0;
}

__xip_text
int xf16cam_esphome_generate_key(char *b64, size_t size)
{
	uint8_t key[XF16CAM_ESPHOME_KEY_LEN];
	size_t olen;
	int rc;

	if (esphome_random(key, sizeof(key)) != 0 ||
	    xf16cam_config_save_esphome_key(key) != 0)
		return -1;
	rc = mbedtls_base64_encode((unsigned char *)b64, size, &olen, key, sizeof(key));
	memset(key, 0, sizeof(key));
	return rc == 0 ? 0 : -1;
}

__xip_text
int xf16cam_esphome_clear_key(void)
{
	return xf16cam_config_save_esphome_key(NULL);
}

#endif /* XF16CAM_ESPHOME */
