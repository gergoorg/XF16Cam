/*
 * MQTT payload generators, host-testable. Every function is __xip_text and
 * every literal is __xip_rodata: appos.ld puts plain .rodata into the SRAM
 * app slot, and the discovery document alone is about 2.5 KB of text.
 */
#ifdef XF16CAM_MQTT

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"

#include "xf16cam_mqtt_payload.h"

#define XF16CAM_MQTT_PIECE  (176)

/* Component flags. */
#define CMP_PTZ    (1U << 0)	/* PTZ builds only */
#define CMP_WEB    (1U << 1)	/* camera: full entry in web mode, tombstone otherwise */
#define CMP_DIAG   (1U << 2)	/* ent_cat = diagnostic */
#define CMP_CMD    (1U << 3)	/* cmd_t = B/cmd/<cmd> */
#define CMP_PRESS  (1U << 4)	/* pl_prs = <pl> */
#define CMP_CONFIG (1U << 5)	/* ent_cat = config */
#define CMP_OFF    (1U << 6)	/* en = false: created disabled in Home Assistant */

/* Value templates. Kept as an enum so each template string exists once. */
enum {
	TPL_NONE,
	TPL_PLAIN,	/* {{ value_json.<id> }} */
	TPL_ONOFF,	/* {{ 'ON' if value_json.<id> else 'OFF' }} */
	TPL_TEMP,	/* tenths of a degree to degrees, null preserved */
	TPL_STREAM,	/* RTSP URL in RTSP mode, MJPEG URL otherwise */
};

typedef struct {
	char id[9];
	char p[14];
	char name[17];
	char unit[4];
	char dev_cla[12];
	char cmd[7];
	char pl[6];
	char ic[21];
	uint8_t tpl;
	uint8_t flags;
} XF16CamMqttComponent;

/* Fixed-size arrays on purpose: a const char * initialiser would leave the
 * literal itself in .rodata, i.e. in the app slot. Components with a device
 * class (restart, temperature, voltage) get their icon from it; an explicit
 * icon wins over the class icon (SD card, sensor). Home Assistant has no
 * state-dependent icon in discovery, so lights keep one icon. */
__xip_rodata static const XF16CamMqttComponent g_components[] = {
	{ "image",    "camera",        "Camera",           "",   "",            "",       "",      "",                     TPL_NONE,   CMP_WEB },
	{ "led",      "light",         "Flash Light",      "",   "",            "led",    "",      "mdi:flashlight",       TPL_ONOFF,  CMP_CMD },
	{ "ir",       "light",         "IR Light",         "",   "",            "ir",     "",      "mdi:lightbulb-night",  TPL_ONOFF,  CMP_CMD | CMP_PTZ | CMP_OFF },
	{ "ptz_up",   "button",        "PTZ up",           "",   "",            "ptz",    "up",    "mdi:arrow-up-bold",    TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_PTZ },
	{ "ptz_down", "button",        "PTZ down",         "",   "",            "ptz",    "down",  "mdi:arrow-down-bold",  TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_PTZ },
	{ "ptz_left", "button",        "PTZ left",         "",   "",            "ptz",    "left",  "mdi:arrow-left-bold",  TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_PTZ },
	{ "ptz_right","button",        "PTZ right",        "",   "",            "ptz",    "right", "mdi:arrow-right-bold", TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_PTZ },
	{ "ptz_home", "button",        "PTZ home",         "",   "",            "ptz",    "home",  "mdi:home",             TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_PTZ },
	{ "reboot",   "button",        "Reboot",           "",   "restart",     "reboot", "PRESS", "",                     TPL_NONE,   CMP_CMD | CMP_PRESS | CMP_CONFIG },
	{ "cam",      "binary_sensor", "Sensor",           "",   "running",     "",       "",      "mdi:camera",           TPL_ONOFF,  CMP_DIAG | CMP_OFF },
	{ "sd",       "binary_sensor", "SD card",          "",   "plug",        "",       "",      "mdi:micro-sd",         TPL_ONOFF,  CMP_DIAG | CMP_OFF },
	{ "media",    "sensor",        "Media mode",       "",   "",            "",       "",      "mdi:video-switch",     TPL_PLAIN,  CMP_DIAG | CMP_OFF },
	{ "stream",   "sensor",        "Stream URL",       "",   "",            "",       "",      "mdi:link-variant",     TPL_STREAM, CMP_DIAG },
	{ "res",      "sensor",        "Resolution",       "",   "",            "",       "",      "mdi:aspect-ratio",     TPL_PLAIN,  CMP_DIAG },
	{ "ip",       "sensor",        "IP address",       "",   "",            "",       "",      "mdi:ip-network",       TPL_PLAIN,  CMP_DIAG },
	{ "up",       "sensor",        "Uptime",           "s",  "duration",    "",       "",      "mdi:timer-outline",    TPL_PLAIN,  CMP_DIAG | CMP_OFF },
	{ "temp",     "sensor",        "Chip temperature", "\xc2\xb0""C", "temperature", "", "",  "",                     TPL_TEMP,   CMP_DIAG | CMP_OFF },
	{ "heap",     "sensor",        "Free heap",        "B",  "data_size",   "",       "",      "mdi:memory",           TPL_PLAIN,  CMP_DIAG | CMP_OFF },
	{ "bat",      "sensor",        "Battery",          "mV", "voltage",     "",       "",      "",                     TPL_PLAIN,  CMP_DIAG | CMP_OFF },
	{ "clients",  "sensor",        "Stream clients",   "",   "",            "",       "",      "mdi:account-multiple", TPL_PLAIN,  CMP_DIAG | CMP_OFF },
	{ "boot",     "sensor",        "Boot reason",      "",   "",            "",       "",      "mdi:power-cycle",      TPL_PLAIN,  CMP_DIAG | CMP_OFF },
};

__xip_rodata static const char g_fmt_base[] = "xf16cam/%s";
__xip_rodata static const char g_fmt_topic[] = "xf16cam/%s/%s";
__xip_rodata static const char g_fmt_discovery_topic[] =
	"homeassistant/device/%s/config";

__xip_text
static int xf16cam_mqtt_emit(XF16CamMqttSink sink, void *ctx, const char *text)
{
	return sink(ctx, text, strlen(text));
}

__xip_text
__attribute__((format(printf, 4, 5)))
static int xf16cam_mqtt_emitf(XF16CamMqttSink sink, void *ctx, char *piece,
                              const char *format, ...)
{
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(piece, XF16CAM_MQTT_PIECE, format, args);
	va_end(args);
	if (length <= 0 || length >= XF16CAM_MQTT_PIECE)
		return -1;
	return sink(ctx, piece, (size_t)length);
}

__xip_text
int xf16cam_mqtt_topic(char *out, size_t out_size, const char *host,
                       const char *suffix)
{
	int length = suffix != NULL ? snprintf(out, out_size, g_fmt_topic, host, suffix) :
	                              snprintf(out, out_size, g_fmt_base, host);

	return length > 0 && (size_t)length < out_size ? 0 : -1;
}

__xip_text
int xf16cam_mqtt_discovery_topic(char *out, size_t out_size, const char *host)
{
	int length = snprintf(out, out_size, g_fmt_discovery_topic, host);

	return length > 0 && (size_t)length < out_size ? 0 : -1;
}

__xip_rodata static const char g_fmt_dev_ids[] =
	"{\"dev\":{\"ids\":[\"%s\"],\"cns\":[[\"mac\",\"%s\"]],\"name\":\"XF16Cam %s\",";
__xip_rodata static const char g_fmt_dev_info[] =
	"\"mf\":\"XF16Cam\",\"mdl\":\"%s\",\"hw\":\"%s\",\"sw\":\"%s\",\"cu\":\"http://%s/\"},";
__xip_rodata static const char g_model_ptz[] = "XR872 PTZ";
__xip_rodata static const char g_model_fixed[] = "XR872 A9";
__xip_rodata static const char g_fmt_origin[] =
	"\"o\":{\"name\":\"xf16cam\",\"sw\":\"%s\"},";
__xip_rodata static const char g_fmt_shared[] =
	"\"avty_t\":\"xf16cam/%s/status\",\"stat_t\":\"xf16cam/%s/state\",\"cmps\":{";
__xip_rodata static const char g_fmt_cmp_head[] =
	"%s\"%s\":{\"p\":\"%s\",\"uniq_id\":\"%s_%s\",\"name\":\"%s\"";
__xip_rodata static const char g_fmt_cmp_tombstone[] = "%s\"%s\":{\"p\":\"%s\"}";
__xip_rodata static const char g_fmt_cmp_image[] = ",\"t\":\"xf16cam/%s/image\"";
__xip_rodata static const char g_fmt_cmp_cmd[] = ",\"cmd_t\":\"xf16cam/%s/cmd/%s\"";
__xip_rodata static const char g_fmt_cmp_press[] = ",\"pl_prs\":\"%s\"";
__xip_rodata static const char g_fmt_tpl_plain[] = ",\"%s\":\"{{ value_json.%s }}\"";
__xip_rodata static const char g_fmt_tpl_onoff[] =
	",\"%s\":\"{{ 'ON' if value_json.%s else 'OFF' }}\"";
__xip_rodata static const char g_fmt_tpl_temp[] =
	",\"%s\":\"{{ (value_json.temp / 10) if value_json.temp is not none else none }}\"";
__xip_rodata static const char g_fmt_tpl_stream[] =
	",\"%s\":\"{{ value_json.stream or value_json.mjpeg }}\"";
__xip_rodata static const char g_fmt_unit[] = ",\"unit_of_meas\":\"%s\"";
__xip_rodata static const char g_fmt_dev_cla[] = ",\"dev_cla\":\"%s\"";
__xip_rodata static const char g_fmt_icon[] = ",\"ic\":\"%s\"";
__xip_rodata static const char g_key_val_tpl[] = "val_tpl";
__xip_rodata static const char g_key_stat_val_tpl[] = "stat_val_tpl";
__xip_rodata static const char g_text_diag[] = ",\"ent_cat\":\"diagnostic\"";
__xip_rodata static const char g_text_config[] = ",\"ent_cat\":\"config\"";
__xip_rodata static const char g_text_off[] = ",\"en\":false";
__xip_rodata static const char g_text_close[] = "}";
__xip_rodata static const char g_text_end[] = "}}";

__xip_text
int xf16cam_mqtt_discovery(const XF16CamMqttIdentity *identity,
                           XF16CamMqttSink sink, void *ctx)
{
	char piece[XF16CAM_MQTT_PIECE];
	const char *host = identity->host;
	const char *comma = "";
	size_t i;
	int rc;

	if ((rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_dev_ids, host,
	                             identity->mac, host + 8)) != 0 ||
	    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_dev_info,
	                             identity->ptz ? g_model_ptz : g_model_fixed,
	                             identity->sensor, identity->version,
	                             identity->ip)) != 0 ||
	    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_origin,
	                             identity->version)) != 0 ||
	    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_shared, host, host)) != 0)
		return rc;

	for (i = 0; i < sizeof(g_components) / sizeof(g_components[0]); ++i) {
		const XF16CamMqttComponent *cmp = &g_components[i];
		const char *tpl_key;

		if ((cmp->flags & CMP_PTZ) && !identity->ptz)
			continue;
		if ((cmp->flags & CMP_WEB) && !identity->web) {
			/* Home Assistant only drops a component it discovered
			 * earlier when the entry reappears with nothing but its
			 * platform, so RTSP mode publishes this tombstone. */
			if ((rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_cmp_tombstone,
			                             comma, cmp->id, cmp->p)) != 0)
				return rc;
			comma = ",";
			continue;
		}
		if ((rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_cmp_head, comma,
		                             cmp->id, cmp->p, host, cmp->id, cmp->name)) != 0)
			return rc;
		comma = ",";
		if ((cmp->flags & CMP_WEB) &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_cmp_image, host)) != 0)
			return rc;
		if ((cmp->flags & CMP_CMD) &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_cmp_cmd, host,
		                             cmp->cmd)) != 0)
			return rc;
		if ((cmp->flags & CMP_PRESS) &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_cmp_press, cmp->pl)) != 0)
			return rc;
		/* A light reads its ON/OFF from the shared state topic through
		 * state_value_template; sensors use value_template. */
		tpl_key = cmp->p[0] == 'l' ? g_key_stat_val_tpl : g_key_val_tpl;
		switch (cmp->tpl) {
		case TPL_PLAIN:
			rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_tpl_plain, tpl_key, cmp->id);
			break;
		case TPL_ONOFF:
			rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_tpl_onoff, tpl_key, cmp->id);
			break;
		case TPL_TEMP:
			rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_tpl_temp, tpl_key);
			break;
		case TPL_STREAM:
			rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_tpl_stream, tpl_key);
			break;
		default:
			rc = 0;
			break;
		}
		if (rc != 0)
			return rc;
		if (cmp->unit[0] != '\0' &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_unit, cmp->unit)) != 0)
			return rc;
		if (cmp->dev_cla[0] != '\0' &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_dev_cla, cmp->dev_cla)) != 0)
			return rc;
		if (cmp->ic[0] != '\0' &&
		    (rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_icon, cmp->ic)) != 0)
			return rc;
		if ((cmp->flags & CMP_DIAG) &&
		    (rc = xf16cam_mqtt_emit(sink, ctx, g_text_diag)) != 0)
			return rc;
		if ((cmp->flags & CMP_CONFIG) &&
		    (rc = xf16cam_mqtt_emit(sink, ctx, g_text_config)) != 0)
			return rc;
		/* Diagnostics nobody watches daily, and the IR light, which the
		 * CDS check overrides within 5 s on PTZ boards, start disabled. */
		if ((cmp->flags & CMP_OFF) &&
		    (rc = xf16cam_mqtt_emit(sink, ctx, g_text_off)) != 0)
			return rc;
		if ((rc = xf16cam_mqtt_emit(sink, ctx, g_text_close)) != 0)
			return rc;
	}
	return xf16cam_mqtt_emit(sink, ctx, g_text_end);
}

__xip_rodata static const char g_fmt_state_head[] =
	"{\"id\":\"%s\",\"ver\":\"%s\",\"media\":\"%s\",";
__xip_rodata static const char g_fmt_state_mjpeg[] =
	"\"mjpeg\":\"http://%s/stream.mjpeg\",\"stream\":null,";
__xip_rodata static const char g_fmt_state_rtsp[] =
	"\"mjpeg\":null,\"stream\":\"rtsp://%s:%u/stream\",";
__xip_rodata static const char g_fmt_state_body[] =
	"\"cam\":%s,\"res\":\"%s\",\"led\":%s,\"ir\":%s,\"ip\":\"%s\",\"up\":%lu,"
	"\"temp\":%s,\"heap\":%lu,";
__xip_rodata static const char g_fmt_state_tail[] =
	"\"sd\":%s,\"bat\":%s,\"clients\":%lu,\"frames\":%lu,\"ptz\":%s,\"boot\":\"%s\"}";
__xip_rodata static const char g_fmt_int[] = "%d";
__xip_rodata static const char g_text_null[] = "null";
__xip_rodata static const char g_text_true[] = "true";
__xip_rodata static const char g_text_false[] = "false";
__xip_rodata static const char g_text_web[] = "web";
__xip_rodata static const char g_text_rtsp[] = "rtsp";

__xip_text
static const char *xf16cam_mqtt_bool(int value)
{
	return value ? g_text_true : g_text_false;
}

__xip_text
int xf16cam_mqtt_state(const XF16CamMqttState *state, XF16CamMqttSink sink,
                       void *ctx)
{
	char piece[XF16CAM_MQTT_PIECE];
	char temp[12];
	char bat[12];
	int rc;

	if (state->temp == INT_MIN)
		strcpy(temp, g_text_null);
	else
		snprintf(temp, sizeof(temp), g_fmt_int, state->temp);
	if (state->bat < 0)
		strcpy(bat, g_text_null);
	else
		snprintf(bat, sizeof(bat), g_fmt_int, state->bat);

	if ((rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_state_head, state->id,
	                             state->version,
	                             state->web ? g_text_web : g_text_rtsp)) != 0)
		return rc;
	rc = state->web ?
	     xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_state_mjpeg, state->ip) :
	     xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_state_rtsp, state->ip,
	                        (unsigned int)XF16CAM_MQTT_RTSP_PORT);
	if (rc != 0)
		return rc;
	if ((rc = xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_state_body,
	                             xf16cam_mqtt_bool(state->cam), state->res,
	                             xf16cam_mqtt_bool(state->led),
	                             xf16cam_mqtt_bool(state->ir), state->ip,
	                             (unsigned long)state->up, temp,
	                             (unsigned long)state->heap)) != 0)
		return rc;
	return xf16cam_mqtt_emitf(sink, ctx, piece, g_fmt_state_tail,
	                          xf16cam_mqtt_bool(state->sd), bat,
	                          (unsigned long)state->clients,
	                          (unsigned long)state->frames,
	                          xf16cam_mqtt_bool(state->ptz), state->boot);
}

__xip_text
size_t xf16cam_mqtt_publish_header(uint8_t *out, size_t out_size,
                                   const char *topic, uint32_t payload_len,
                                   int retain)
{
	size_t topic_len = strlen(topic);
	uint32_t remaining;
	size_t n = 0;
	size_t varint_len = 1;
	uint32_t probe;

	if (topic_len == 0 || topic_len > 0xffffU ||
	    payload_len > 268435455UL - 2U - topic_len)
		return 0;
	remaining = 2U + (uint32_t)topic_len + payload_len;
	for (probe = remaining; probe >= 128U; probe /= 128U)
		++varint_len;
	if (1U + varint_len + 2U + topic_len > out_size)
		return 0;
	out[n++] = (uint8_t)(0x30U | (retain ? 1U : 0U));
	do {
		uint8_t byte = (uint8_t)(remaining % 128U);

		remaining /= 128U;
		if (remaining != 0)
			byte |= 0x80U;
		out[n++] = byte;
	} while (remaining != 0);
	out[n++] = (uint8_t)(topic_len >> 8);
	out[n++] = (uint8_t)(topic_len & 0xffU);
	memcpy(out + n, topic, topic_len);
	return n + topic_len;
}

#endif /* XF16CAM_MQTT */
