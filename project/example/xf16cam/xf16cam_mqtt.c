/*
 * MQTT client for Home Assistant. Compiled only with XF16CAM_MQTT.
 *
 * The vendored Paho client (src/net/mqtt, always linked and placed in XIP
 * by appos.ld) is used for CONNECT, SUBSCRIBE, PINGREQ and DISCONNECT only:
 *
 * - MQTTPublish() serialises the whole packet into the send buffer, so a
 *   JPEG of up to 100 KiB cannot go through it. Every publish here writes a
 *   header built by xf16cam_mqtt_publish_header() and then streams the
 *   payload straight from its source (the capture arena, or a generator run
 *   a second time), so the two 512-byte buffers only ever hold control
 *   packets.
 * - MQTTYield() lets an inbound packet larger than the read buffer overflow
 *   it (readPacket() only prints its assert). xf16cam_mqtt_poll() reads the
 *   fixed header itself and drains anything that does not fit.
 *
 * All code is __xip_text and every literal is __xip_rodata; see
 * xf16cam_xip.h for why.
 */
#ifdef XF16CAM_MQTT

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "kernel/os/os.h"
#include "common/framework/net_ctrl.h"
#include "common/framework/sysinfo.h"
#include "lwip/sockets.h"
#include "net/mqtt/MQTTClient-C/MQTTClient.h"
#include "sys/sram_heap.h"

#include "xf16cam_board.h"
#include "xf16cam_config.h"
#include "xf16cam_http.h"
#include "xf16cam_media.h"
#include "xf16cam_mqtt.h"
#include "xf16cam_mqtt_payload.h"
#include "xf16cam_net.h"
#include "xf16cam_power.h"
#include "xf16cam_ptz.h"
#include "xf16cam_sensor.h"
#include "xf16cam_storage.h"
#include "xf16cam_version.h"
#include "xf16cam_xip.h"

#define XF16CAM_MQTT_STACK_SIZE      (3 * 1024)
#define XF16CAM_MQTT_BUF_SIZE        (512)
#define XF16CAM_MQTT_KEEPALIVE_S     (60)
#define XF16CAM_MQTT_COMMAND_MS      (5000)
#define XF16CAM_MQTT_POLL_MS         (200)
#define XF16CAM_MQTT_IO_TIMEOUT_MS   (2000)
#define XF16CAM_MQTT_PUBLISH_MS      (10000)
#define XF16CAM_MQTT_STATE_MS        (60000)
#define XF16CAM_MQTT_BACKOFF_MIN_MS  (2000)
#define XF16CAM_MQTT_BACKOFF_MAX_MS  (60000)
/* Holding the camera between shots counts as a capture session for the board
 * task's 30 s stall watchdog, so longer intervals acquire per shot instead. */
#define XF16CAM_MQTT_HOLD_MAX_S      (20)

/* printf with the format string in XIP; same idea as XF16CAM_XIP_FORMAT. */
#define XF16CAM_MQTT_LOG(literal, ...) do { \
	__xip_rodata static const char XF16CAM_XIP_JOIN(g_xip_log_, __LINE__)[] = literal; \
	printf(XF16CAM_XIP_JOIN(g_xip_log_, __LINE__), ##__VA_ARGS__); \
} while (0)

/* Non-static in MQTTClient.c but not declared in its header. */
int keepalive(Client *c);

typedef int (*XF16CamMqttGenerator)(const void *arg, XF16CamMqttSink sink,
                                    void *ctx);

static OS_Thread_t g_mqtt_thread;
static Network g_net;
static Client g_client;
static unsigned char g_sendbuf[XF16CAM_MQTT_BUF_SIZE];
static unsigned char g_readbuf[XF16CAM_MQTT_BUF_SIZE];
static char g_topic_status[XF16CAM_MQTT_TOPIC_MAX];
static char g_topic_state[XF16CAM_MQTT_TOPIC_MAX];
static char g_topic_image[XF16CAM_MQTT_TOPIC_MAX];
static char g_topic_cmd[XF16CAM_MQTT_TOPIC_MAX];	/* B/cmd/# */
static char g_topic_discovery[XF16CAM_MQTT_TOPIC_MAX];
static XF16CamMqttInfo g_info;
static volatile char g_pending_ptz;	/* first letter of the direction */
static volatile uint8_t g_pending_reboot;
static volatile uint8_t g_pending_rediscover;
static volatile uint8_t g_state_dirty;
static int g_camera_held;
static int g_topics_ready;

__xip_rodata static const char g_topic_ha_status[] = "homeassistant/status";
__xip_rodata static const char g_text_online[] = "online";
__xip_rodata static const char g_text_offline[] = "offline";
__xip_rodata static const char g_text_on[] = "ON";
__xip_rodata static const char g_text_press[] = "PRESS";
__xip_rodata static const char g_cmd_led[] = "led";
__xip_rodata static const char g_cmd_ir[] = "ir";
__xip_rodata static const char g_cmd_ptz[] = "ptz";
__xip_rodata static const char g_cmd_reboot[] = "reboot";
__xip_rodata static const char g_suffix_status[] = "status";
__xip_rodata static const char g_suffix_state[] = "state";
__xip_rodata static const char g_suffix_image[] = "image";
__xip_rodata static const char g_suffix_cmd[] = "cmd/#";
__xip_rodata static const uint8_t g_eoi[] = { 0xff, 0xd9 };

__xip_text
static uint32_t xf16cam_mqtt_now(void)
{
	return OS_TicksToMSecs(OS_GetTicks());
}

__xip_text
static int xf16cam_mqtt_net_up(void)
{
	return g_wlan_netif != NULL && NETIF_IS_AVAILABLE(g_wlan_netif);
}

/* ---- transmit ---------------------------------------------------------- */

__xip_text
static int xf16cam_mqtt_write_all(const void *data, size_t length)
{
	const unsigned char *at = data;
	uint32_t deadline = xf16cam_mqtt_now() + XF16CAM_MQTT_PUBLISH_MS;

	while (length > 0) {
		int chunk = length > 4096U ? 4096 : (int)length;
		int sent = g_net.mqttwrite(&g_net, (unsigned char *)at, chunk,
		                           XF16CAM_MQTT_IO_TIMEOUT_MS);

		if (sent < 0)
			return -1;
		if (sent == 0 && (int32_t)(xf16cam_mqtt_now() - deadline) >= 0)
			return -1;
		at += sent;
		length -= (size_t)sent;
	}
	return 0;
}

__xip_text
static int xf16cam_mqtt_count_sink(void *ctx, const void *data, size_t length)
{
	(void)data;
	*(uint32_t *)ctx += (uint32_t)length;
	return 0;
}

__xip_text
static int xf16cam_mqtt_write_sink(void *ctx, const void *data, size_t length)
{
	(void)ctx;
	return xf16cam_mqtt_write_all(data, length);
}

__xip_text
static void xf16cam_mqtt_mark_sent(void)
{
	countdown(&g_client.last_sent, g_client.keepAliveInterval);
}

__xip_text
static int xf16cam_mqtt_publish_raw(const char *topic, const void *payload,
                                    uint32_t length, int retain)
{
	uint8_t header[XF16CAM_MQTT_TOPIC_MAX + 8];
	size_t header_len = xf16cam_mqtt_publish_header(header, sizeof(header),
	                                                topic, length, retain);

	if (header_len == 0 || xf16cam_mqtt_write_all(header, header_len) != 0 ||
	    xf16cam_mqtt_write_all(payload, length) != 0)
		return -1;
	xf16cam_mqtt_mark_sent();
	return 0;
}

/* Run the generator once to learn the length, then again into the socket.
 * The argument must be a snapshot so both passes produce identical bytes. */
__xip_text
static int xf16cam_mqtt_publish_generated(const char *topic,
                                          XF16CamMqttGenerator generate,
                                          const void *arg, int retain)
{
	uint8_t header[XF16CAM_MQTT_TOPIC_MAX + 8];
	uint32_t length = 0;
	size_t header_len;

	if (generate(arg, xf16cam_mqtt_count_sink, &length) != 0)
		return -1;
	header_len = xf16cam_mqtt_publish_header(header, sizeof(header), topic,
	                                         length, retain);
	if (header_len == 0 || xf16cam_mqtt_write_all(header, header_len) != 0 ||
	    generate(arg, xf16cam_mqtt_write_sink, NULL) != 0)
		return -1;
	xf16cam_mqtt_mark_sent();
	return 0;
}

__xip_text
static int xf16cam_mqtt_generate_discovery(const void *arg, XF16CamMqttSink sink,
                                           void *ctx)
{
	return xf16cam_mqtt_discovery(arg, sink, ctx);
}

__xip_text
static int xf16cam_mqtt_generate_state(const void *arg, XF16CamMqttSink sink,
                                       void *ctx)
{
	return xf16cam_mqtt_state(arg, sink, ctx);
}

__xip_text
static int xf16cam_mqtt_publish_status(const char *text)
{
	return xf16cam_mqtt_publish_raw(g_topic_status, text, strlen(text), 1);
}

__xip_text
static int xf16cam_mqtt_publish_discovery(void)
{
	XF16CamMqttIdentity identity;
	const struct sysinfo *info = sysinfo_get();
	char mac[18];
	int ptz;

#ifdef NO_PTZ
	ptz = 0;
#else
	ptz = 1;
#endif
	XF16CAM_XIP_FORMAT(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
	                   info->mac_addr[0], info->mac_addr[1], info->mac_addr[2],
	                   info->mac_addr[3], info->mac_addr[4], info->mac_addr[5]);
	identity.host = xf16cam_net_hostname();
	identity.mac = mac;
	identity.sensor = xf16cam_sensor_name();
	identity.ip = xf16cam_net_ip();
	identity.version = XF16CAM_VERSION;
	identity.ptz = (uint8_t)ptz;
	identity.web = xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB;
	return xf16cam_mqtt_publish_generated(g_topic_discovery,
	                                      xf16cam_mqtt_generate_discovery,
	                                      &identity, 1);
}

/* The same sources /api/system reads (xf16cam_http_system_json). Battery is
 * the last measured value; measuring takes the shared ADC for up to a second
 * and belongs to the user's Measure button. */
__xip_text
static uint32_t xf16cam_mqtt_fingerprint(void)
{
	return (uint32_t)(xf16cam_board_get_led_on() != 0) |
	       (uint32_t)(xf16cam_board_get_ir_led_on() != 0) << 1 |
	       (uint32_t)(xf16cam_storage_info()->mounted != 0) << 2 |
	       (uint32_t)(xf16cam_sensor_available() != 0) << 3 |
	       xf16cam_media_active_clients() << 4;
}

__xip_text
static int xf16cam_mqtt_publish_state(void)
{
	XF16CamMqttState state;
	const XF16CamPowerInfo *power = xf16cam_power_info();
	char res[12];
	int ptz;

#ifdef NO_PTZ
	ptz = 0;
#else
	ptz = 1;
#endif
	XF16CAM_XIP_FORMAT(res, sizeof(res), "%ux%u",
	                   (unsigned int)xf16cam_sensor_width(),
	                   (unsigned int)xf16cam_sensor_height());
	state.id = xf16cam_net_hostname();
	state.ip = xf16cam_net_ip();
	state.version = XF16CAM_VERSION;
	state.res = res;
	state.boot = xf16cam_http_boot_reason();
	state.up = xf16cam_mqtt_now() / 1000U;
	state.heap = sram_free_heap_size();
	state.clients = xf16cam_media_active_clients();
	state.frames = xf16cam_media_info()->frames;
	state.temp = xf16cam_http_chip_temperature();
	state.bat = power->valid ? (int)power->millivolts : -1;
	state.web = xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB;
	state.cam = xf16cam_sensor_available() != 0;
	state.led = xf16cam_board_get_led_on() != 0;
	state.ir = xf16cam_board_get_ir_led_on() != 0;
	state.sd = xf16cam_storage_info()->mounted != 0;
	state.ptz = (uint8_t)ptz;
	if (xf16cam_mqtt_publish_generated(g_topic_state, xf16cam_mqtt_generate_state,
	                                   &state, 1) != 0)
		return -1;
	++g_info.states;
	return 0;
}

/* Returns -1 only for a link failure. A capture failure is logged and
 * retried on the next interval; the camera may simply be busy or absent. */
__xip_text
static int xf16cam_mqtt_publish_image(void)
{
	const uint8_t *jpeg;
	uint32_t jpeg_len;
	uint32_t eoi_len;
	uint8_t header[XF16CAM_MQTT_TOPIC_MAX + 8];
	size_t header_len;
	int failed;

	if (xf16cam_media_snapshot_begin(&jpeg, &jpeg_len) != 0) {
		XF16CAM_MQTT_LOG("xf16cam mqtt: snapshot unavailable\n");
		return 0;
	}
	eoi_len = jpeg_len >= 2 && jpeg[jpeg_len - 2] == 0xff &&
	          jpeg[jpeg_len - 1] == 0xd9 ? 0 : sizeof(g_eoi);
	header_len = xf16cam_mqtt_publish_header(header, sizeof(header), g_topic_image,
	                                         jpeg_len + eoi_len, 1);
	failed = header_len == 0 ||
	         xf16cam_mqtt_write_all(header, header_len) != 0 ||
	         xf16cam_mqtt_write_all(jpeg, jpeg_len) != 0 ||
	         (eoi_len != 0 && xf16cam_mqtt_write_all(g_eoi, eoi_len) != 0);
	xf16cam_media_snapshot_end();
	if (failed)
		return -1;
	xf16cam_mqtt_mark_sent();
	++g_info.images;
	return 0;
}

__xip_text
static void xf16cam_mqtt_unhold_camera(void)
{
	if (g_camera_held) {
		xf16cam_media_camera_unhold();
		g_camera_held = 0;
	}
}

/* ---- receive ----------------------------------------------------------- */

__xip_text
static void xf16cam_mqtt_dispatch(const MQTTString *topic,
                                  const unsigned char *payload, int payload_len)
{
	const char *name = topic->lenstring.data;
	size_t name_len = topic->lenstring.len > 0 ? (size_t)topic->lenstring.len : 0;
	size_t prefix_len = strlen(g_topic_cmd) - 1;	/* drop the '#' */
	char suffix[8];
	char value[8];
	size_t copy;
	int on;

	++g_info.commands;
	if (name_len == strlen(g_topic_ha_status) &&
	    memcmp(name, g_topic_ha_status, name_len) == 0) {
		if (payload_len == (int)strlen(g_text_online) &&
		    memcmp(payload, g_text_online, (size_t)payload_len) == 0)
			g_pending_rediscover = 1;
		return;
	}
	if (name_len <= prefix_len || memcmp(name, g_topic_cmd, prefix_len) != 0)
		return;
	copy = name_len - prefix_len;
	if (copy >= sizeof(suffix))
		return;
	memcpy(suffix, name + prefix_len, copy);
	suffix[copy] = '\0';
	copy = payload_len > 0 ? (size_t)payload_len : 0;
	if (copy >= sizeof(value))
		copy = sizeof(value) - 1;
	memcpy(value, payload, copy);
	value[copy] = '\0';
	on = strcmp(value, g_text_on) == 0;

	if (strcmp(suffix, g_cmd_led) == 0) {
		xf16cam_board_set_led(on);
		g_state_dirty = 1;
	} else if (strcmp(suffix, g_cmd_ir) == 0) {
		xf16cam_board_set_ir_led(on);
		g_state_dirty = 1;
	} else if (strcmp(suffix, g_cmd_ptz) == 0) {
		g_pending_ptz = value[0];
	} else if (strcmp(suffix, g_cmd_reboot) == 0 &&
	           strcmp(value, g_text_press) == 0) {
		g_pending_reboot = 1;
	}
}

/* Used by the library only while it waits for CONNACK or SUBACK. */
__xip_text
static void xf16cam_mqtt_message_handler(MessageData *data)
{
	xf16cam_mqtt_dispatch(data->topicName, data->message->payload,
	                      (int)data->message->payloadlen);
}

__xip_text
static void xf16cam_mqtt_handle_packet(int type, int length)
{
	if (type == PUBLISH) {
		MQTTString topic = MQTTString_initializer;
		unsigned char dup;
		unsigned char retained;
		unsigned short id;
		unsigned char *payload;
		int payload_len;
		int qos;

		if (MQTTDeserialize_publish(&dup, &qos, &retained, &id, &topic, &payload,
		                            &payload_len, g_readbuf, length) == 1)
			xf16cam_mqtt_dispatch(&topic, payload, payload_len);
	} else if (type == PINGRESP) {
		g_client.ping_outstanding = 0;
	}
}

/* Wait up to timeout_ms for one packet, then run the library's keepalive.
 * Returns FAILURE when the link is gone. */
__xip_text
static int xf16cam_mqtt_poll(int timeout_ms)
{
	unsigned char byte;
	int got = g_net.mqttread(&g_net, &byte, 1, timeout_ms);
	int remaining = 0;
	int multiplier = 1;
	int length = 1;
	int digits;

	if (got < 0)
		return FAILURE;
	if (got == 1) {
		g_readbuf[0] = byte;
		for (digits = 0; digits < 4; ++digits) {
			if (g_net.mqttread(&g_net, &byte, 1, XF16CAM_MQTT_IO_TIMEOUT_MS) != 1)
				return FAILURE;
			g_readbuf[length++] = byte;
			remaining += (byte & 0x7f) * multiplier;
			multiplier *= 128;
			if ((byte & 0x80) == 0)
				break;
		}
		if (digits == 4)
			return FAILURE;
		if ((size_t)(length + remaining) > sizeof(g_readbuf)) {
			++g_info.dropped;
			XF16CAM_MQTT_LOG("xf16cam mqtt: dropping %d-byte packet\n", remaining);
			while (remaining > 0) {
				int chunk = remaining > (int)sizeof(g_readbuf) ?
				            (int)sizeof(g_readbuf) : remaining;
				int read = g_net.mqttread(&g_net, g_readbuf, chunk,
				                          XF16CAM_MQTT_IO_TIMEOUT_MS);

				if (read <= 0)
					return FAILURE;
				remaining -= read;
			}
		} else {
			if (remaining > 0 &&
			    g_net.mqttread(&g_net, g_readbuf + length, remaining,
			                   XF16CAM_MQTT_IO_TIMEOUT_MS) != remaining)
				return FAILURE;
			xf16cam_mqtt_handle_packet(g_readbuf[0] >> 4, length + remaining);
		}
		countdown(&g_client.last_received, g_client.keepAliveInterval);
	}
	return keepalive(&g_client);
}

/* ---- session ----------------------------------------------------------- */

__xip_text
static int xf16cam_mqtt_connect(void)
{
	const XF16CamConfig *config = xf16cam_config_get();
	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	char host[XF16CAM_MQTT_HOST_MAX_LEN + 1];
	int timeout = XF16CAM_MQTT_IO_TIMEOUT_MS;
	int rc;

	memcpy(host, config->u.mqtt.host, sizeof(host));
	NewNetwork(&g_net);
	rc = ConnectNetwork(&g_net, host, xf16cam_config_mqtt_port(config));
	if (rc != 0) {
		/* A failed connect() closes the socket but leaves the number. */
		g_net.my_socket = -1;
		XF16CAM_MQTT_LOG("xf16cam mqtt: connect to %s:%u failed (%d)\n", host,
		                 (unsigned int)xf16cam_config_mqtt_port(config), rc);
		return -1;
	}
	/* Bound every send so a stalled broker cannot pin the capture lock. */
	setsockopt(g_net.my_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	setsockopt(g_net.my_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	MQTTClient(&g_client, &g_net, XF16CAM_MQTT_COMMAND_MS, g_sendbuf,
	           sizeof(g_sendbuf), g_readbuf, sizeof(g_readbuf));
	data.MQTTVersion = 4;
	data.clientID.cstring = (char *)xf16cam_net_hostname();
	data.keepAliveInterval = XF16CAM_MQTT_KEEPALIVE_S;
	data.cleansession = 1;
	if (config->u.mqtt.user[0] != '\0')
		data.username.cstring = (char *)config->u.mqtt.user;
	if (config->u.mqtt.pass[0] != '\0')
		data.password.cstring = (char *)config->u.mqtt.pass;
	data.willFlag = 1;
	data.will.topicName.cstring = g_topic_status;
	data.will.message.cstring = (char *)g_text_offline;
	data.will.retained = 1;
	data.will.qos = 0;
	rc = MQTTConnect(&g_client, &data);
	if (rc != SUCCESS) {
		XF16CAM_MQTT_LOG("xf16cam mqtt: CONNECT refused (%d)\n", rc);
		return -1;
	}
	if (xf16cam_mqtt_publish_status(g_text_online) != 0 ||
	    xf16cam_mqtt_publish_discovery() != 0 ||
	    xf16cam_mqtt_publish_state() != 0 ||
	    MQTTSubscribe(&g_client, g_topic_cmd, QOS0,
	                  xf16cam_mqtt_message_handler) != SUCCESS ||
	    MQTTSubscribe(&g_client, g_topic_ha_status, QOS0,
	                  xf16cam_mqtt_message_handler) != SUCCESS) {
		XF16CAM_MQTT_LOG("xf16cam mqtt: announce failed\n");
		return -1;
	}
	++g_info.connects;
	g_info.connected = 1;
	XF16CAM_MQTT_LOG("xf16cam mqtt: connected to %s\n", host);
	return 0;
}

__xip_text
static void xf16cam_mqtt_disconnect(void)
{
	xf16cam_mqtt_unhold_camera();
	if (g_client.isconnected)
		MQTTDisconnect(&g_client);
	if (g_net.my_socket >= 0)
		g_net.disconnect(&g_net);
	g_client.isconnected = 0;
	if (g_info.connected) {
		g_info.connected = 0;
		++g_info.disconnects;
	}
}

#ifndef NO_PTZ
__xip_text
static void xf16cam_mqtt_run_ptz(char direction)
{
	switch (direction) {
	case 'u': ptz_move_up(); break;
	case 'd': ptz_move_down(); break;
	case 'l': ptz_move_left(); break;
	case 'r': ptz_move_right(); break;
	case 'h': ptz_move_home(); break;
	default: break;
	}
}
#endif

/* Runs until the link fails. */
__xip_text
static void xf16cam_mqtt_session(void)
{
	uint32_t interval_ms = (uint32_t)xf16cam_config_mqtt_interval(xf16cam_config_get()) * 1000U;
	int hold = interval_ms <= XF16CAM_MQTT_HOLD_MAX_S * 1000U;
	int web = xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB;
	uint32_t last_state = xf16cam_mqtt_now();
	uint32_t last_image = last_state - interval_ms;	/* first frame now */
	uint32_t fingerprint = xf16cam_mqtt_fingerprint();

	for (;;) {
		uint32_t now;
		uint32_t current;

		if (xf16cam_update_active()) {
			/* Release the rail for the OTA/reboot path; the broker
			 * will time us out if the update takes longer than the
			 * keepalive, and the reconnect loop handles that. */
			xf16cam_mqtt_unhold_camera();
			OS_MSleep(500);
			continue;
		}
		if (!xf16cam_mqtt_net_up())
			return;
		if (xf16cam_mqtt_poll(XF16CAM_MQTT_POLL_MS) == FAILURE)
			return;

		if (g_pending_reboot) {
			g_pending_reboot = 0;
			XF16CAM_MQTT_LOG("xf16cam mqtt: reboot command\n");
			/* A clean DISCONNECT would not fire the will. */
			xf16cam_mqtt_publish_status(g_text_offline);
			xf16cam_mqtt_disconnect();
			xf16cam_board_reboot();	/* returns only when deferred */
			return;
		}
		if (g_pending_ptz != 0) {
			char direction = g_pending_ptz;

			g_pending_ptz = 0;
#ifndef NO_PTZ
			xf16cam_mqtt_run_ptz(direction);
#else
			(void)direction;
#endif
		}
		if (g_pending_rediscover) {
			g_pending_rediscover = 0;
			if (xf16cam_mqtt_publish_discovery() != 0)
				return;
			g_state_dirty = 1;
		}

		now = xf16cam_mqtt_now();
		current = xf16cam_mqtt_fingerprint();
		if (current != fingerprint || g_state_dirty ||
		    now - last_state >= XF16CAM_MQTT_STATE_MS) {
			fingerprint = current;
			g_state_dirty = 0;
			if (xf16cam_mqtt_publish_state() != 0)
				return;
			last_state = now;
		}
		if (web && now - last_image >= interval_ms) {
			last_image = now;
			if (hold && !g_camera_held)
				g_camera_held = xf16cam_media_camera_hold() == 0;
			if (xf16cam_mqtt_publish_image() != 0)
				return;
		}
	}
}

/* The hostname exists only once xf16cam_net_start() has run, so the topics
 * are built on first contact with the network rather than at task creation.
 * Returns 0 when the client should run, -1 when it should retire. */
__xip_text
static int xf16cam_mqtt_prepare(void)
{
	const XF16CamConfig *config = xf16cam_config_get();
	const char *host = xf16cam_net_hostname();

	if (g_topics_ready)
		return 0;
	if (xf16cam_net_mode() != XF16CAM_WIFI_STA) {
		XF16CAM_MQTT_LOG("xf16cam mqtt: not in station mode; client retired\n");
		return -1;
	}
	if (xf16cam_mqtt_topic(g_topic_status, sizeof(g_topic_status), host, g_suffix_status) != 0 ||
	    xf16cam_mqtt_topic(g_topic_state, sizeof(g_topic_state), host, g_suffix_state) != 0 ||
	    xf16cam_mqtt_topic(g_topic_image, sizeof(g_topic_image), host, g_suffix_image) != 0 ||
	    xf16cam_mqtt_topic(g_topic_cmd, sizeof(g_topic_cmd), host, g_suffix_cmd) != 0 ||
	    xf16cam_mqtt_discovery_topic(g_topic_discovery, sizeof(g_topic_discovery), host) != 0) {
		XF16CAM_MQTT_LOG("xf16cam mqtt: topic build failed; client retired\n");
		return -1;
	}
	g_topics_ready = 1;
	XF16CAM_MQTT_LOG("xf16cam mqtt: broker %s:%u, image every %u s, id %s\n",
	                 config->u.mqtt.host,
	                 (unsigned int)xf16cam_config_mqtt_port(config),
	                 (unsigned int)xf16cam_config_mqtt_interval(config), host);
	return 0;
}

__xip_text
static void xf16cam_mqtt_task(void *arg)
{
	uint32_t backoff = XF16CAM_MQTT_BACKOFF_MIN_MS;

	(void)arg;
	for (;;) {
		while (!xf16cam_mqtt_net_up())
			OS_MSleep(1000);
		if (xf16cam_mqtt_prepare() != 0)
			break;
		if (xf16cam_mqtt_connect() == 0) {
			backoff = XF16CAM_MQTT_BACKOFF_MIN_MS;
			xf16cam_mqtt_session();
			XF16CAM_MQTT_LOG("xf16cam mqtt: link lost\n");
		}
		xf16cam_mqtt_disconnect();
		OS_MSleep(backoff);
		backoff = backoff * 2 > XF16CAM_MQTT_BACKOFF_MAX_MS ?
		          XF16CAM_MQTT_BACKOFF_MAX_MS : backoff * 2;
	}
	/* AP mode or no hostname: give the 3 KiB stack back to the heap. */
	g_info.enabled = 0;
	OS_ThreadSetInvalid(&g_mqtt_thread);
	OS_ThreadDelete(NULL);
}

/* ---- public ------------------------------------------------------------ */

/* Called from main() before the network exists. Only the config is needed
 * at this point; the task sleeps until the link is up. */
__xip_text
int xf16cam_mqtt_start(void)
{
	const XF16CamConfig *config = xf16cam_config_get();

	if (config->u.mqtt.host[0] == '\0')
		return 0;
	g_net.my_socket = -1;
	g_info.enabled = 1;
	if (OS_ThreadCreate(&g_mqtt_thread, "xf16cam-mqtt", xf16cam_mqtt_task, NULL,
	                    OS_THREAD_PRIO_APP, XF16CAM_MQTT_STACK_SIZE) != OS_OK) {
		g_info.enabled = 0;
		XF16CAM_MQTT_LOG("xf16cam mqtt: thread create failed\n");
		return -1;
	}
	return 0;
}

__xip_text
const XF16CamMqttInfo *xf16cam_mqtt_info(void)
{
	return &g_info;
}

__xip_text
uint32_t xf16cam_mqtt_stack_min_free(void)
{
	return g_info.enabled ? OS_ThreadGetStackMinFreeSize(&g_mqtt_thread) : 0;
}

#endif /* XF16CAM_MQTT */
