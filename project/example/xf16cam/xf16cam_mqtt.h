#ifndef XF16CAM_MQTT_H
#define XF16CAM_MQTT_H

#include <stdint.h>

/*
 * MQTT client for Home Assistant (XF16CAM_MQTT builds). Publishes the
 * device's state document, a JPEG frame per interval in browser media mode,
 * and a Home Assistant device discovery payload; subscribes to a command
 * topic for the LEDs, PTZ and reboot. See xf16cam_mqtt_payload.h for the
 * topic layout. Runs only in station mode with a broker address saved.
 */

typedef struct {
	uint32_t connects;
	uint32_t disconnects;
	uint32_t images;
	uint32_t states;
	uint32_t commands;
	uint32_t dropped;	/* inbound packets too large for the read buffer */
	uint8_t enabled;	/* task running (broker configured, STA mode) */
	uint8_t connected;
} XF16CamMqttInfo;

#ifdef XF16CAM_MQTT
int xf16cam_mqtt_start(void);
const XF16CamMqttInfo *xf16cam_mqtt_info(void);
uint32_t xf16cam_mqtt_stack_min_free(void);
#else
static inline int xf16cam_mqtt_start(void)
{
	return 0;
}
#endif

#endif
