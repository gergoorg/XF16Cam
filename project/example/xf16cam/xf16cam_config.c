#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "image/fdcm.h"
#include "kernel/os/os.h"

#include "xf16cam_config.h"

#define XF16CAM_CONFIG_MAGIC          (0x58464346UL)
#define XF16CAM_CONFIG_SCHEMA_LEGACY  (1U)
#define XF16CAM_CONFIG_SCHEMA         (2U)

/* Schema 2 gives the first byte formerly reserved by schema 1 a defined
 * meaning. Keep the layout unchanged so schema 1 records can be migrated.
 *
 * led_on is the second such byte and is deliberately NOT a schema 3: the
 * record keeps its size, so xf16cam_config_storage_valid() still accepts
 * every deployed record, and each one already carries a zero there, which
 * reads as "lamp off" -- the behaviour before it had a meaning. Bumping the
 * schema instead would make older firmware reject the record and reset the
 * device to AP mode, losing the Wi-Fi credentials. That also makes this the
 * last field available for free: anything further needs a real schema 3 with
 * a length change and a migration, or the three padding bytes that sit
 * between psk and checksum. */
_Static_assert(sizeof(XF16CamConfig) == 116, "XF16Cam config layout changed");
_Static_assert(offsetof(XF16CamConfig, resolution) == 10,
	       "XF16Cam resolution offset changed");
_Static_assert(offsetof(XF16CamConfig, led_on) == 11,
	       "XF16Cam LED offset changed");
_Static_assert(offsetof(XF16CamConfig, ssid) == 12,
	       "XF16Cam SSID offset changed");
_Static_assert(offsetof(XF16CamConfig, checksum) == 112,
	       "XF16Cam checksum offset changed");

static fdcm_handle_t *g_config_store;
static XF16CamConfig g_configs[2];
static OS_Mutex_t g_config_lock;
static volatile unsigned int g_config_index;
static volatile int g_update_active;
static int g_config_lock_ready;

static uint32_t xf16cam_config_checksum(const XF16CamConfig *config)
{
	const uint8_t *data = (const uint8_t *)config;
	uint32_t hash = 2166136261UL;
	size_t i;

	for (i = 0; i < offsetof(XF16CamConfig, checksum); ++i) {
		hash ^= data[i];
		hash *= 16777619UL;
	}
	return hash;
}

static void xf16cam_config_defaults(XF16CamConfig *config)
{
	memset(config, 0, sizeof(*config));
	config->magic = XF16CAM_CONFIG_MAGIC;
	config->schema = XF16CAM_CONFIG_SCHEMA;
	config->length = sizeof(*config);
	config->wifi_mode = XF16CAM_WIFI_AP;
	config->media_mode = XF16CAM_MEDIA_RTSP;
	config->resolution = XF16CAM_RESOLUTION_QVGA;
	config->checksum = xf16cam_config_checksum(config);
}

static int xf16cam_config_storage_valid(const XF16CamConfig *config)
{
	if (config->magic != XF16CAM_CONFIG_MAGIC ||
	    config->length != sizeof(*config) ||
	    config->wifi_mode < XF16CAM_WIFI_AP ||
	    config->wifi_mode > XF16CAM_WIFI_STA ||
	    config->media_mode < XF16CAM_MEDIA_RTSP ||
	    config->media_mode > XF16CAM_MEDIA_WEB ||
	    config->ssid[XF16CAM_SSID_MAX_LEN] != '\0' ||
	    config->psk[XF16CAM_PSK_MAX_LEN] != '\0') {
		return 0;
	}
	return config->checksum == xf16cam_config_checksum(config);
}

__xip_text
int xf16cam_config_init(void)
{
	XF16CamConfig *config = &g_configs[0];

	memset(g_configs, 0, sizeof(g_configs));
	g_config_index = 0;
	g_update_active = 0;
	g_config_lock_ready = 0;
	if (OS_MutexCreate(&g_config_lock) != OS_OK) {
		printf("xf16cam config: mutex create failed\n");
		xf16cam_config_defaults(config);
		return -1;
	}
	g_config_lock_ready = 1;
	g_config_store = fdcm_open(XF16CAM_CONFIG_FLASH,
	                           XF16CAM_CONFIG_ADDR,
	                           XF16CAM_CONFIG_SIZE);
	if (g_config_store == NULL) {
		printf("xf16cam config: FDCM open failed at 0x%08lx\n",
		       (unsigned long)XF16CAM_CONFIG_ADDR);
		xf16cam_config_defaults(config);
		return -1;
	}

	if (fdcm_read(g_config_store, config, sizeof(*config)) != sizeof(*config) ||
	    !xf16cam_config_storage_valid(config) ||
	    (config->schema != XF16CAM_CONFIG_SCHEMA_LEGACY &&
	     config->schema != XF16CAM_CONFIG_SCHEMA) ||
	    (config->schema == XF16CAM_CONFIG_SCHEMA &&
	     config->resolution > XF16CAM_RESOLUTION_VGA)) {
		xf16cam_config_defaults(config);
		printf("xf16cam config: using defaults (AP mode)\n");
		return 0;
	}

	if (config->schema == XF16CAM_CONFIG_SCHEMA_LEGACY) {
		/* The resolution byte was undefined in schema 1; never infer a
		 * resolution from it. Preserve the user's settings, but make the first
		 * resolution selection deterministic and conservative. */
		config->schema = XF16CAM_CONFIG_SCHEMA;
		config->resolution = XF16CAM_RESOLUTION_QVGA;
		config->led_on = 0;
		config->checksum = xf16cam_config_checksum(config);
		if (fdcm_write(g_config_store, config, sizeof(*config)) != sizeof(*config)) {
			printf("xf16cam config: schema 1 migration write failed; using QVGA in RAM\n");
		} else {
			printf("xf16cam config: migrated schema 1 to 2 (resolution=QVGA)\n");
		}
	}

	printf("xf16cam config: loaded schema=%u wifi=%s media=%s resolution=%s ssid=%s\n",
	       config->schema,
	       config->wifi_mode == XF16CAM_WIFI_STA ? "STA" : "AP",
	       config->media_mode == XF16CAM_MEDIA_WEB ? "WEB" : "RTSP",
	       config->resolution == XF16CAM_RESOLUTION_VGA ? "VGA" : "QVGA",
	       config->ssid);
	return 0;
}

const XF16CamConfig *xf16cam_config_get(void)
{
	return &g_configs[g_config_index];
}

/* Caller holds g_config_lock. Publish only after the flash write succeeds, so
 * readers always see one complete immutable snapshot. */
static int xf16cam_config_write(XF16CamConfig *config)
{
	unsigned int next;

	if (g_config_store == NULL)
		return -1;
	config->checksum = xf16cam_config_checksum(config);
	if (fdcm_write(g_config_store, config, sizeof(*config)) != sizeof(*config))
		return -1;
	next = g_config_index ^ 1U;
	g_configs[next] = *config;
	__sync_synchronize();
	g_config_index = next;
	return 0;
}

int xf16cam_config_save_sta(const char *ssid, const char *psk)
{
	XF16CamConfig config;
	size_t ssid_len = strlen(ssid);
	size_t psk_len = strlen(psk);
	int result;

	if (ssid_len == 0 || ssid_len > XF16CAM_SSID_MAX_LEN ||
	    psk_len > XF16CAM_PSK_MAX_LEN || (psk_len > 0 && psk_len < 8)) {
		return -1;
	}

	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	memset(config.ssid, 0, sizeof(config.ssid));
	memset(config.psk, 0, sizeof(config.psk));
	memcpy(config.ssid, ssid, ssid_len);
	memcpy(config.psk, psk, psk_len);
	config.wifi_mode = XF16CAM_WIFI_STA;
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}

int xf16cam_config_save_ap(void)
{
	XF16CamConfig config;
	int result;

	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	config.wifi_mode = XF16CAM_WIFI_AP;
	memset(config.ssid, 0, sizeof(config.ssid));
	memset(config.psk, 0, sizeof(config.psk));
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}

int xf16cam_config_save_media(XF16CamMediaMode mode)
{
	XF16CamConfig config;
	int result;

	if (mode != XF16CAM_MEDIA_RTSP && mode != XF16CAM_MEDIA_WEB)
		return -1;
	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	config.media_mode = mode;
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}

int xf16cam_config_save_resolution(XF16CamResolution resolution)
{
	XF16CamConfig config;
	int result;

	if (resolution != XF16CAM_RESOLUTION_QVGA &&
	    resolution != XF16CAM_RESOLUTION_VGA)
		return -1;
	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	config.resolution = resolution;
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}

/* Called on every manual lamp toggle, so skip a write when nothing changed:
 * FDCM appends each record into one of about 34 slots and erases the whole
 * settings sector when they run out, and that sector also holds the Wi-Fi
 * credentials. The automatic day/night switching drives the IR LED, not
 * this lamp, so it never reaches here. */
int xf16cam_config_save_led(int on)
{
	XF16CamConfig config;
	int result;

	on = on ? 1 : 0;
	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	if (config.led_on == (uint8_t)on) {
		OS_MutexUnlock(&g_config_lock);
		return 0;
	}
	config.led_on = (uint8_t)on;
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}

int xf16cam_update_begin(void)
{
	int result = -1;

	/* Preserve serial upgrade recovery even if the configuration mutex could
	 * not be created; configuration writes are disabled in that state. */
	if (!g_config_lock_ready)
		return __sync_bool_compare_and_swap(&g_update_active, 0, 1) ? 0 : -1;
	if (OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (!g_update_active) {
		g_update_active = 1;
		result = 0;
	}
	OS_MutexUnlock(&g_config_lock);
	return result;
}

void xf16cam_update_end(void)
{
	if (!g_config_lock_ready) {
		__sync_lock_test_and_set(&g_update_active, 0);
		return;
	}
	if (OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	g_update_active = 0;
	OS_MutexUnlock(&g_config_lock);
}

int xf16cam_update_active(void)
{
	return g_update_active;
}
