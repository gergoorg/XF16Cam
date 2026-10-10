#ifndef XF16CAM_MQTT_PAYLOAD_H
#define XF16CAM_MQTT_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure generators for everything the MQTT client publishes, kept free of
 * SDK dependencies so tests/xf16cam/test_mqtt_payload.c can compile them on
 * the host. Each generator streams its output through a sink in small pieces;
 * the client runs every generator twice, once into a counting sink to learn
 * the payload length for the MQTT PUBLISH header and once into the socket.
 *
 * Topic layout (B = xf16cam/<host>, host = XF16CAM-XXXXXX from the eFuse MAC):
 *   B/status   online | offline (retained; offline is also the LWT)
 *   B/state    the JSON from xf16cam_mqtt_state(), retained
 *   B/image    one JPEG per interval, retained, browser media mode only
 *   B/cmd/led, B/cmd/ir      ON | OFF
 *   B/cmd/ptz                up | down | left | right | home
 *   B/cmd/reboot             PRESS
 *   homeassistant/device/<host>/config   xf16cam_mqtt_discovery(), retained
 * The state document carries the same keys in both media modes so a Home
 * Assistant template written against one mode keeps working in the other.
 */

/* Return 0 to continue, non-zero to abort the generator with that value. */
typedef int (*XF16CamMqttSink)(void *ctx, const void *data, size_t len);

typedef struct {
	const char *host;	/* device id: node id, client id and topic id */
	const char *mac;	/* "aa:bb:cc:dd:ee:ff", the device's connection */
	const char *sensor;	/* image sensor name, reported as hardware */
	const char *ip;
	const char *version;
	uint8_t ptz;		/* PTZ build: IR light and PTZ buttons exist */
	uint8_t web;		/* browser media mode: camera component exists */
} XF16CamMqttIdentity;

typedef struct {
	const char *id;
	const char *ip;
	const char *version;
	const char *res;	/* "320x240" */
	const char *boot;	/* boot reason text */
	uint32_t up;		/* seconds */
	uint32_t heap;		/* bytes */
	uint32_t clients;
	uint32_t frames;
	int temp;		/* tenths of a degree Celsius; INT_MIN = null */
	int bat;		/* millivolts; negative = null */
	uint8_t web;
	uint8_t cam;
	uint8_t led;
	uint8_t ir;
	uint8_t sd;
	uint8_t ptz;
} XF16CamMqttState;

/* Longest topic: "homeassistant/device/" + host (15) + "/config". */
#define XF16CAM_MQTT_TOPIC_MAX  (48)
#define XF16CAM_MQTT_RTSP_PORT  (8554)

int xf16cam_mqtt_topic(char *out, size_t out_size, const char *host,
                       const char *suffix);
int xf16cam_mqtt_discovery_topic(char *out, size_t out_size, const char *host);
int xf16cam_mqtt_discovery(const XF16CamMqttIdentity *identity,
                           XF16CamMqttSink sink, void *ctx);
int xf16cam_mqtt_state(const XF16CamMqttState *state, XF16CamMqttSink sink,
                       void *ctx);
/* QoS 0 PUBLISH fixed + variable header; the payload follows on the wire.
 * Returns the header length, or 0 when it does not fit. */
size_t xf16cam_mqtt_publish_header(uint8_t *out, size_t out_size,
                                   const char *topic, uint32_t payload_len,
                                   int retain);

#endif
