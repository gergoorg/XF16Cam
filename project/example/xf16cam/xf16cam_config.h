#ifndef XF16CAM_CONFIG_H
#define XF16CAM_CONFIG_H

#include <stdint.h>

#define XF16CAM_SSID_MAX_LEN  (32)
#define XF16CAM_PSK_MAX_LEN   (63)

/* The integration block at offset 112 of a schema 3 record belongs to one
 * build variant, named by the tag at offset 109. The tags are letters so an
 * older layout, where byte 109 held the MQTT interval, cannot match by
 * accident. A record whose tag differs from the image's
 * XF16CAM_CONFIG_FEATURE keeps its Wi-Fi, media and resolution prefix and
 * has the block cleared, so an OTA between the mqtt and esphome variants
 * preserves the common settings and drops only the integration's own. */
#define XF16CAM_FEATURE_NONE      (0U)
#define XF16CAM_FEATURE_MQTT      ('M')
#define XF16CAM_FEATURE_ESPHOME   ('E')
#define XF16CAM_CONFIG_BLOCK_SIZE (104U)

#if defined(XF16CAM_MQTT) && defined(XF16CAM_ESPHOME)
#error "XF16CAM_MQTT and XF16CAM_ESPHOME are mutually exclusive build variants"
#elif defined(XF16CAM_MQTT)
#define XF16CAM_CONFIG_FEATURE  XF16CAM_FEATURE_MQTT
#elif defined(XF16CAM_ESPHOME)
#define XF16CAM_CONFIG_FEATURE  XF16CAM_FEATURE_ESPHOME
#else
#define XF16CAM_CONFIG_FEATURE  XF16CAM_FEATURE_NONE
#endif

/* MQTT broker settings. The host is an IP literal today because lwIP DNS is
 * linked out (prj_config.h); the field is wide enough for a short hostname
 * should DNS ever be switched back on. */
#define XF16CAM_MQTT_HOST_MAX_LEN  (31)
#define XF16CAM_MQTT_USER_MAX_LEN  (31)
#define XF16CAM_MQTT_PASS_MAX_LEN  (31)
#define XF16CAM_MQTT_DEFAULT_PORT  (1883U)
#define XF16CAM_MQTT_DEFAULT_INTERVAL_S (10U)

/* Strings first so host, user and pass keep the offsets (112, 144, 176)
 * schema 3 has had from the start. */
typedef struct {
	char host[XF16CAM_MQTT_HOST_MAX_LEN + 1];	/* empty = MQTT off */
	char user[XF16CAM_MQTT_USER_MAX_LEN + 1];
	char pass[XF16CAM_MQTT_PASS_MAX_LEN + 1];
	uint16_t port;		/* 0 = default 1883 */
	uint8_t interval_s;	/* image publish period; 0 = default */
	uint8_t reserved[5];
} XF16CamMqttConfig;

typedef enum {
	XF16CAM_WIFI_AP = 1,
	XF16CAM_WIFI_STA = 2,
} XF16CamWifiMode;

typedef enum {
	XF16CAM_MEDIA_RTSP = 1,
	XF16CAM_MEDIA_WEB = 2,
} XF16CamMediaMode;

typedef enum {
	XF16CAM_RESOLUTION_QVGA = 0,
	XF16CAM_RESOLUTION_VGA = 1,
} XF16CamResolution;

typedef struct {
	uint32_t magic;
	uint16_t schema;
	uint16_t length;
	uint8_t wifi_mode;
	uint8_t media_mode;
	uint8_t resolution;
	uint8_t reserved;
	char ssid[XF16CAM_SSID_MAX_LEN + 1];
	char psk[XF16CAM_PSK_MAX_LEN + 1];
	/* Schema 3. Every field above is byte-identical to the 116-byte schema
	 * 1/2 record, which is what lets the loader migrate an old record in
	 * place. The rest is the integration block: the tag of the variant that
	 * wrote it and a union sized for the largest block, so every build
	 * shares one record layout without holding fields for more than one
	 * integration. A block that outgrows XF16CAM_CONFIG_BLOCK_SIZE needs a
	 * variable-length record (FDCM reads must match the stored size), not a
	 * bigger union. Keep everything before checksum so the FNV-1a span
	 * covers it. */
	uint8_t feature;	/* XF16CAM_FEATURE_* */
	uint8_t feature_pad[2];
	union {
#ifdef XF16CAM_MQTT
		XF16CamMqttConfig mqtt;
#endif
		uint8_t raw[XF16CAM_CONFIG_BLOCK_SIZE];
	} u;
	uint32_t checksum;
} XF16CamConfig;

int xf16cam_config_init(void);
const XF16CamConfig *xf16cam_config_get(void);
int xf16cam_config_save_sta(const char *ssid, const char *psk);
int xf16cam_config_save_ap(void);
int xf16cam_config_save_media(XF16CamMediaMode mode);
int xf16cam_config_save_resolution(XF16CamResolution resolution);
#ifdef XF16CAM_MQTT
/* Any string may be empty; an empty host disables MQTT. port 0 and
 * interval_s 0 store the defaults. */
int xf16cam_config_save_mqtt(const char *host, uint16_t port, const char *user,
                             const char *pass, uint8_t interval_s);

static inline uint16_t xf16cam_config_mqtt_port(const XF16CamConfig *config)
{
	return config->u.mqtt.port != 0 ? config->u.mqtt.port :
	                                  XF16CAM_MQTT_DEFAULT_PORT;
}

static inline uint8_t xf16cam_config_mqtt_interval(const XF16CamConfig *config)
{
	return config->u.mqtt.interval_s != 0 ? config->u.mqtt.interval_s :
	                                        XF16CAM_MQTT_DEFAULT_INTERVAL_S;
}
#endif

int xf16cam_update_begin(void);
void xf16cam_update_end(void);
int xf16cam_update_active(void);

#endif
