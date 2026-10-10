#ifndef XF16CAM_CONFIG_H
#define XF16CAM_CONFIG_H

#include <stdint.h>

#define XF16CAM_SSID_MAX_LEN  (32)
#define XF16CAM_PSK_MAX_LEN   (63)

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
	uint8_t led_on;
	char ssid[XF16CAM_SSID_MAX_LEN + 1];
	char psk[XF16CAM_PSK_MAX_LEN + 1];
	uint32_t checksum;
} XF16CamConfig;

int xf16cam_config_init(void);
const XF16CamConfig *xf16cam_config_get(void);
int xf16cam_config_save_sta(const char *ssid, const char *psk);
int xf16cam_config_save_ap(void);
int xf16cam_config_save_media(XF16CamMediaMode mode);
int xf16cam_config_save_resolution(XF16CamResolution resolution);
int xf16cam_config_save_led(int on);
int xf16cam_update_begin(void);
void xf16cam_update_end(void);
int xf16cam_update_active(void);

#endif
