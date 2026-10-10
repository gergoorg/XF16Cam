#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "image/fdcm.h"
#include "kernel/os/os.h"

#include "xf16cam_config.h"

#define XF16CAM_CONFIG_MAGIC          (0x58464346UL)
#define XF16CAM_CONFIG_SCHEMA_LEGACY  (1U)
#define XF16CAM_CONFIG_SCHEMA_V2      (2U)
#define XF16CAM_CONFIG_SCHEMA         (3U)

/* Schema 1 and 2 records are 116 bytes: the fields up to and including psk,
 * then the checksum at 112. Schema 3 appends the integration tag and block
 * after psk, so the first 109 bytes of every schema are byte-identical and an
 * old record migrates by copying that prefix. The loader reads a legacy
 * record with its own length and checksum span; see
 * xf16cam_config_load_legacy(). */
#define XF16CAM_CONFIG_LEGACY_LENGTH           (116U)
#define XF16CAM_CONFIG_LEGACY_CHECKSUM_OFFSET  (112U)

_Static_assert(sizeof(XF16CamConfig) == 220, "XF16Cam config layout changed");
_Static_assert(offsetof(XF16CamConfig, resolution) == 10,
	       "XF16Cam resolution offset changed");
_Static_assert(offsetof(XF16CamConfig, ssid) == 12,
	       "XF16Cam SSID offset changed");
_Static_assert(offsetof(XF16CamConfig, psk) == 45,
	       "XF16Cam PSK offset changed");
_Static_assert(offsetof(XF16CamConfig, feature) == 109,
	       "XF16Cam legacy prefix changed");
_Static_assert(offsetof(XF16CamConfig, u) == 112,
	       "XF16Cam integration block offset changed");
_Static_assert(sizeof(((XF16CamConfig *)0)->u) == XF16CAM_CONFIG_BLOCK_SIZE,
	       "XF16Cam integration block size changed");
#ifdef XF16CAM_MQTT
_Static_assert(sizeof(XF16CamMqttConfig) == XF16CAM_CONFIG_BLOCK_SIZE,
	       "XF16Cam MQTT block must fill the integration block");
#endif
#ifdef XF16CAM_ESPHOME
_Static_assert(sizeof(XF16CamEsphomeConfig) <= XF16CAM_CONFIG_BLOCK_SIZE,
	       "XF16Cam ESPHome block must fit the integration block");
#endif
_Static_assert(offsetof(XF16CamConfig, checksum) == 216,
	       "XF16Cam checksum offset changed");

static fdcm_handle_t *g_config_store;
static XF16CamConfig g_configs[2];
static OS_Mutex_t g_config_lock;
static volatile unsigned int g_config_index;
static volatile int g_update_active;
static int g_config_lock_ready;

static uint32_t xf16cam_config_checksum_span(const void *data, size_t length)
{
	const uint8_t *bytes = data;
	uint32_t hash = 2166136261UL;
	size_t i;

	for (i = 0; i < length; ++i) {
		hash ^= bytes[i];
		hash *= 16777619UL;
	}
	return hash;
}

static uint32_t xf16cam_config_checksum(const XF16CamConfig *config)
{
	return xf16cam_config_checksum_span(config, offsetof(XF16CamConfig, checksum));
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
	config->feature = XF16CAM_CONFIG_FEATURE;
	config->checksum = xf16cam_config_checksum(config);
}

/* Validates the prefix every variant shares; the integration block is the
 * business of xf16cam_config_adopt_block(). */
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

/* The integration block belongs to the variant named by the tag. Another
 * variant's block, or a corrupt one, is cleared rather than failing the whole
 * record, so an OTA between the mqtt and esphome images keeps the Wi-Fi
 * settings and only the integration has to be set up again. */
__xip_text
static void xf16cam_config_adopt_block(XF16CamConfig *config)
{
	int clear = config->feature != XF16CAM_CONFIG_FEATURE;

#ifdef XF16CAM_MQTT
	if (!clear &&
	    (config->u.mqtt.host[XF16CAM_MQTT_HOST_MAX_LEN] != '\0' ||
	     config->u.mqtt.user[XF16CAM_MQTT_USER_MAX_LEN] != '\0' ||
	     config->u.mqtt.pass[XF16CAM_MQTT_PASS_MAX_LEN] != '\0'))
		clear = 1;
#endif
#ifdef XF16CAM_ESPHOME
	if (!clear && (config->u.esphome.flags & ~XF16CAM_ESPHOME_KEY_SET) != 0)
		clear = 1;
#endif
	if (!clear)
		return;
	printf("xf16cam config: integration block tag 0x%02x, image expects 0x%02x; cleared\n",
	       (unsigned int)config->feature, (unsigned int)XF16CAM_CONFIG_FEATURE);
	memset(&config->u, 0, sizeof(config->u));
	memset(config->feature_pad, 0, sizeof(config->feature_pad));
	config->feature = XF16CAM_CONFIG_FEATURE;
	config->checksum = xf16cam_config_checksum(config);
	if (fdcm_write(g_config_store, config, sizeof(*config)) != sizeof(*config))
		printf("xf16cam config: block clear write failed; using RAM copy\n");
}

/* Read a 116-byte schema 1 or 2 record, validate it against its own layout,
 * and rebuild it as schema 3 with an empty integration block. Called once
 * from init before any task runs, so the stack buffer is safe. Returns 0 when
 * a migrated record is in *config. */
__xip_text
static int xf16cam_config_load_legacy(XF16CamConfig *config)
{
	uint8_t legacy[XF16CAM_CONFIG_LEGACY_LENGTH];
	uint32_t magic;
	uint32_t checksum;
	uint16_t schema;
	uint16_t length;

	if (fdcm_read(g_config_store, legacy, sizeof(legacy)) != sizeof(legacy))
		return -1;
	memcpy(&magic, legacy, sizeof(magic));
	memcpy(&schema, legacy + offsetof(XF16CamConfig, schema), sizeof(schema));
	memcpy(&length, legacy + offsetof(XF16CamConfig, length), sizeof(length));
	memcpy(&checksum, legacy + XF16CAM_CONFIG_LEGACY_CHECKSUM_OFFSET, sizeof(checksum));
	if (magic != XF16CAM_CONFIG_MAGIC ||
	    length != XF16CAM_CONFIG_LEGACY_LENGTH ||
	    (schema != XF16CAM_CONFIG_SCHEMA_LEGACY &&
	     schema != XF16CAM_CONFIG_SCHEMA_V2) ||
	    checksum != xf16cam_config_checksum_span(legacy,
	                                            XF16CAM_CONFIG_LEGACY_CHECKSUM_OFFSET))
		return -1;

	/* The legacy prefix (magic .. psk) is byte-identical in schema 3. */
	memset(config, 0, sizeof(*config));
	memcpy(config, legacy, offsetof(XF16CamConfig, feature));
	if (config->wifi_mode < XF16CAM_WIFI_AP ||
	    config->wifi_mode > XF16CAM_WIFI_STA ||
	    config->media_mode < XF16CAM_MEDIA_RTSP ||
	    config->media_mode > XF16CAM_MEDIA_WEB ||
	    config->ssid[XF16CAM_SSID_MAX_LEN] != '\0' ||
	    config->psk[XF16CAM_PSK_MAX_LEN] != '\0' ||
	    (schema == XF16CAM_CONFIG_SCHEMA_V2 &&
	     config->resolution > XF16CAM_RESOLUTION_VGA))
		return -1;
	if (schema == XF16CAM_CONFIG_SCHEMA_LEGACY) {
		/* The resolution byte was undefined in schema 1; never infer a
		 * resolution from it. Make the first selection deterministic. */
		config->resolution = XF16CAM_RESOLUTION_QVGA;
	}
	config->reserved = 0;
	config->schema = XF16CAM_CONFIG_SCHEMA;
	config->length = sizeof(*config);
	config->feature = XF16CAM_CONFIG_FEATURE;
	config->checksum = xf16cam_config_checksum(config);
	if (fdcm_write(g_config_store, config, sizeof(*config)) != sizeof(*config))
		printf("xf16cam config: schema %u migration write failed; using RAM copy\n",
		       schema);
	else
		printf("xf16cam config: migrated schema %u to %u\n", schema,
		       XF16CAM_CONFIG_SCHEMA);
	return 0;
}

__xip_text
int xf16cam_config_init(void)
{
	XF16CamConfig *config = &g_configs[0];
	const char *block;

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

	if ((fdcm_read(g_config_store, config, sizeof(*config)) != sizeof(*config) ||
	     !xf16cam_config_storage_valid(config) ||
	     config->schema != XF16CAM_CONFIG_SCHEMA ||
	     config->resolution > XF16CAM_RESOLUTION_VGA) &&
	    xf16cam_config_load_legacy(config) != 0) {
		xf16cam_config_defaults(config);
		printf("xf16cam config: using defaults (AP mode)\n");
		return 0;
	}
	xf16cam_config_adopt_block(config);

#ifdef XF16CAM_MQTT
	block = config->u.mqtt.host[0] != '\0' ? config->u.mqtt.host : "mqtt off";
#elif defined(XF16CAM_ESPHOME)
	block = xf16cam_config_esphome_key_set(config) ? "esphome (key set)" :
	                                                 "esphome (no key)";
#else
	block = "none";
#endif
	printf("xf16cam config: loaded schema=%u wifi=%s media=%s resolution=%s ssid=%s integration=%s\n",
	       config->schema,
	       config->wifi_mode == XF16CAM_WIFI_STA ? "STA" : "AP",
	       config->media_mode == XF16CAM_MEDIA_WEB ? "WEB" : "RTSP",
	       config->resolution == XF16CAM_RESOLUTION_VGA ? "VGA" : "QVGA",
	       config->ssid, block);
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

#ifdef XF16CAM_MQTT
int xf16cam_config_save_mqtt(const char *host, uint16_t port, const char *user,
                             const char *pass, uint8_t interval_s)
{
	XF16CamConfig config;
	size_t host_len = strlen(host);
	size_t user_len = strlen(user);
	size_t pass_len = strlen(pass);
	int result;

	if (host_len > XF16CAM_MQTT_HOST_MAX_LEN ||
	    user_len > XF16CAM_MQTT_USER_MAX_LEN ||
	    pass_len > XF16CAM_MQTT_PASS_MAX_LEN)
		return -1;
	if (!g_config_lock_ready || OS_MutexLock(&g_config_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (g_update_active) {
		OS_MutexUnlock(&g_config_lock);
		return -1;
	}
	config = *xf16cam_config_get();
	memset(&config.u.mqtt, 0, sizeof(config.u.mqtt));
	memcpy(config.u.mqtt.host, host, host_len);
	memcpy(config.u.mqtt.user, user, user_len);
	memcpy(config.u.mqtt.pass, pass, pass_len);
	config.u.mqtt.port = port;
	config.u.mqtt.interval_s = interval_s;
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}
#endif

#ifdef XF16CAM_ESPHOME
int xf16cam_config_save_esphome_key(const uint8_t *key)
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
	memset(&config.u.esphome, 0, sizeof(config.u.esphome));
	if (key != NULL) {
		memcpy(config.u.esphome.key, key, sizeof(config.u.esphome.key));
		config.u.esphome.flags = XF16CAM_ESPHOME_KEY_SET;
	}
	result = xf16cam_config_write(&config);
	OS_MutexUnlock(&g_config_lock);
	return result;
}
#endif

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
