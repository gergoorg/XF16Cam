#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "xf16cam_mqtt_payload.h"

typedef struct {
	char text[4096];
	size_t length;
	size_t fail_after;	/* sink returns -7 once this many bytes were seen */
} Capture;

static int capture_sink(void *ctx, const void *data, size_t len)
{
	Capture *capture = ctx;

	if (capture->fail_after != 0 && capture->length + len > capture->fail_after)
		return -7;
	assert(capture->length + len < sizeof(capture->text));
	memcpy(capture->text + capture->length, data, len);
	capture->length += len;
	capture->text[capture->length] = '\0';
	return 0;
}

static int count_sink(void *ctx, const void *data, size_t len)
{
	(void)data;
	*(size_t *)ctx += len;
	return 0;
}

static void test_topics(void)
{
	char topic[XF16CAM_MQTT_TOPIC_MAX];
	char tiny[8];

	assert(xf16cam_mqtt_topic(topic, sizeof(topic), "XF16CAM-A1B2C3", NULL) == 0);
	assert(strcmp(topic, "xf16cam/XF16CAM-A1B2C3") == 0);
	assert(xf16cam_mqtt_topic(topic, sizeof(topic), "XF16CAM-A1B2C3", "cmd/#") == 0);
	assert(strcmp(topic, "xf16cam/XF16CAM-A1B2C3/cmd/#") == 0);
	assert(xf16cam_mqtt_discovery_topic(topic, sizeof(topic), "XF16CAM-A1B2C3") == 0);
	assert(strcmp(topic, "homeassistant/device/XF16CAM-A1B2C3/config") == 0);
	assert(xf16cam_mqtt_topic(tiny, sizeof(tiny), "XF16CAM-A1B2C3", "state") == -1);
}

static void test_state_web_and_rtsp(void)
{
	static const char expected_web[] =
		"{\"id\":\"XF16CAM-A1B2C3\",\"ver\":\"0.17.16\",\"media\":\"web\","
		"\"mjpeg\":\"http://192.168.0.108/stream.mjpeg\",\"stream\":null,"
		"\"cam\":true,\"res\":\"320x240\",\"led\":true,\"ir\":false,"
		"\"ip\":\"192.168.0.108\",\"up\":1234,\"temp\":412,\"heap\":51234,"
		"\"sd\":false,\"bat\":null,\"clients\":0,\"frames\":123,\"ptz\":true,"
		"\"boot\":\"power-on\"}";
	static const char expected_rtsp[] =
		"{\"id\":\"XF16CAM-A1B2C3\",\"ver\":\"0.17.16\",\"media\":\"rtsp\","
		"\"mjpeg\":null,\"stream\":\"rtsp://192.168.0.108:8554/stream\","
		"\"cam\":false,\"res\":\"640x480\",\"led\":false,\"ir\":true,"
		"\"ip\":\"192.168.0.108\",\"up\":7,\"temp\":null,\"heap\":1,"
		"\"sd\":true,\"bat\":3987,\"clients\":2,\"frames\":0,\"ptz\":false,"
		"\"boot\":\"watchdog\"}";
	XF16CamMqttState state = {
		.id = "XF16CAM-A1B2C3", .ip = "192.168.0.108", .version = "0.17.16",
		.res = "320x240", .boot = "power-on", .up = 1234, .heap = 51234,
		.clients = 0, .frames = 123, .temp = 412, .bat = -1, .web = 1, .cam = 1,
		.led = 1, .ir = 0, .sd = 0, .ptz = 1,
	};
	Capture capture = { .length = 0 };
	size_t counted = 0;

	assert(xf16cam_mqtt_state(&state, capture_sink, &capture) == 0);
	assert(strcmp(capture.text, expected_web) == 0);
	assert(xf16cam_mqtt_state(&state, count_sink, &counted) == 0);
	assert(counted == capture.length);

	state.web = 0; state.cam = 0; state.res = "640x480"; state.led = 0;
	state.ir = 1; state.up = 7; state.temp = INT_MIN; state.heap = 1;
	state.sd = 1; state.bat = 3987; state.clients = 2; state.frames = 0;
	state.ptz = 0; state.boot = "watchdog";
	capture.length = 0;
	assert(xf16cam_mqtt_state(&state, capture_sink, &capture) == 0);
	assert(strcmp(capture.text, expected_rtsp) == 0);
}

static int count_occurrences(const char *haystack, const char *needle)
{
	int count = 0;
	const char *at = haystack;

	while ((at = strstr(at, needle)) != NULL) {
		++count;
		at += strlen(needle);
	}
	return count;
}

static void test_discovery(void)
{
	XF16CamMqttIdentity identity = {
		.host = "XF16CAM-A1B2C3", .mac = "a4:b1:c2:a1:b2:c3", .sensor = "SP0A39",
		.ip = "192.168.0.108", .version = "0.17.16", .ptz = 1, .web = 1,
	};
	Capture capture = { .length = 0 };
	size_t counted = 0;

	assert(xf16cam_mqtt_discovery(&identity, capture_sink, &capture) == 0);
	assert(xf16cam_mqtt_discovery(&identity, count_sink, &counted) == 0);
	assert(counted == capture.length);
	{
		static const char head[] =
			"{\"dev\":{\"ids\":[\"XF16CAM-A1B2C3\"],\"cns\":[[\"mac\",\"a4:b1:c2:a1:b2:c3\"]],"
			"\"name\":\"XF16Cam A1B2C3\",\"mf\":\"XF16Cam\",\"mdl\":\"XR872 PTZ\","
			"\"hw\":\"SP0A39\",\"sw\":\"0.17.16\",\"cu\":\"http://192.168.0.108/\"},"
			"\"o\":{\"name\":\"xf16cam\",\"sw\":\"0.17.16\"},";

		assert(strncmp(capture.text, head, sizeof(head) - 1) == 0);
	}
	assert(strstr(capture.text, "\"avty_t\":\"xf16cam/XF16CAM-A1B2C3/status\"") != NULL);
	assert(strstr(capture.text, "\"stat_t\":\"xf16cam/XF16CAM-A1B2C3/state\"") != NULL);
	assert(strstr(capture.text,
	              "\"image\":{\"p\":\"camera\",\"uniq_id\":\"XF16CAM-A1B2C3_image\","
	              "\"name\":\"Camera\",\"t\":\"xf16cam/XF16CAM-A1B2C3/image\"}") != NULL);
	assert(strstr(capture.text,
	              "\"led\":{\"p\":\"light\",\"uniq_id\":\"XF16CAM-A1B2C3_led\","
	              "\"name\":\"Flash Light\",\"cmd_t\":\"xf16cam/XF16CAM-A1B2C3/cmd/led\","
	              "\"stat_val_tpl\":\"{{ 'ON' if value_json.led else 'OFF' }}\","
	              "\"ic\":\"mdi:flashlight\"}") != NULL);
	assert(strstr(capture.text,
	              "\"ir\":{\"p\":\"light\",\"uniq_id\":\"XF16CAM-A1B2C3_ir\","
	              "\"name\":\"IR Light\",\"cmd_t\":\"xf16cam/XF16CAM-A1B2C3/cmd/ir\","
	              "\"stat_val_tpl\":\"{{ 'ON' if value_json.ir else 'OFF' }}\","
	              "\"ic\":\"mdi:lightbulb-night\",\"en\":false}") != NULL);
	assert(strstr(capture.text,
	              "\"ptz_home\":{\"p\":\"button\",\"uniq_id\":\"XF16CAM-A1B2C3_ptz_home\","
	              "\"name\":\"PTZ home\",\"cmd_t\":\"xf16cam/XF16CAM-A1B2C3/cmd/ptz\","
	              "\"pl_prs\":\"home\",\"ic\":\"mdi:home\"}") != NULL);
	assert(count_occurrences(capture.text, "\"ptz_") == 5);
	assert(strstr(capture.text,
	              "\"reboot\":{\"p\":\"button\",\"uniq_id\":\"XF16CAM-A1B2C3_reboot\","
	              "\"name\":\"Reboot\",\"cmd_t\":\"xf16cam/XF16CAM-A1B2C3/cmd/reboot\","
	              "\"pl_prs\":\"PRESS\",\"dev_cla\":\"restart\",\"ent_cat\":\"config\"}") != NULL);
	assert(strstr(capture.text,
	              "\"boot\":{\"p\":\"sensor\",\"uniq_id\":\"XF16CAM-A1B2C3_boot\","
	              "\"name\":\"Boot reason\",\"val_tpl\":\"{{ value_json.boot }}\","
	              "\"ic\":\"mdi:power-cycle\",\"ent_cat\":\"diagnostic\",\"en\":false}") != NULL);
	assert(strstr(capture.text,
	              "\"temp\":{\"p\":\"sensor\",\"uniq_id\":\"XF16CAM-A1B2C3_temp\","
	              "\"name\":\"Chip temperature\",\"val_tpl\":\"{{ (value_json.temp / 10)"
	              " if value_json.temp is not none else none }}\","
	              "\"unit_of_meas\":\"\xc2\xb0""C\",\"dev_cla\":\"temperature\","
	              "\"ent_cat\":\"diagnostic\",\"en\":false}") != NULL);
	assert(strstr(capture.text,
	              "\"sd\":{\"p\":\"binary_sensor\",\"uniq_id\":\"XF16CAM-A1B2C3_sd\","
	              "\"name\":\"SD card\",\"val_tpl\":\"{{ 'ON' if value_json.sd else 'OFF' }}\","
	              "\"dev_cla\":\"plug\",\"ic\":\"mdi:micro-sd\",\"ent_cat\":\"diagnostic\","
	              "\"en\":false}") != NULL);
	assert(strstr(capture.text,
	              "\"cam\":{\"p\":\"binary_sensor\",\"uniq_id\":\"XF16CAM-A1B2C3_cam\","
	              "\"name\":\"Sensor\",\"val_tpl\":\"{{ 'ON' if value_json.cam else 'OFF' }}\","
	              "\"dev_cla\":\"running\",\"ic\":\"mdi:camera\",\"ent_cat\":\"diagnostic\","
	              "\"en\":false}") != NULL);
	/* ir, cam, sd, media, up, temp, heap, bat, clients, boot */
	assert(count_occurrences(capture.text, "\"en\":false") == 10);
	assert(strstr(capture.text,
	              "\"stream\":{\"p\":\"sensor\",\"uniq_id\":\"XF16CAM-A1B2C3_stream\","
	              "\"name\":\"Stream URL\",\"val_tpl\":\"{{ value_json.stream or value_json.mjpeg }}\","
	              "\"ic\":\"mdi:link-variant\",\"ent_cat\":\"diagnostic\"}") != NULL);
	assert(capture.text[capture.length - 1] == '}');
	assert(capture.text[capture.length - 2] == '}');
	assert(strstr(capture.text, ",,") == NULL);
	assert(strstr(capture.text, "{,") == NULL);
	printf("discovery (ptz, web): %zu bytes\n", capture.length);

	identity.ptz = 0;
	identity.web = 0;
	capture.length = 0;
	assert(xf16cam_mqtt_discovery(&identity, capture_sink, &capture) == 0);
	assert(strstr(capture.text, "\"mdl\":\"XR872 A9\"") != NULL);
	assert(strstr(capture.text, "\"cmps\":{\"image\":{\"p\":\"camera\"},\"led\":{") != NULL);
	assert(strstr(capture.text, "\"ir\":") == NULL);
	assert(count_occurrences(capture.text, "\"ptz_") == 0);
	assert(strstr(capture.text, "\"reboot\":{") != NULL);
	assert(count_occurrences(capture.text, "\"en\":false") == 9);
	assert(strstr(capture.text, ",,") == NULL);
	printf("discovery (no_ptz, rtsp): %zu bytes\n", capture.length);

	capture.length = 0;
	capture.fail_after = 100;
	assert(xf16cam_mqtt_discovery(&identity, capture_sink, &capture) == -7);
}

static void test_publish_header(void)
{
	uint8_t out[64];
	size_t n;

	n = xf16cam_mqtt_publish_header(out, sizeof(out), "a/b", 0, 0);
	assert(n == 7);
	assert(out[0] == 0x30 && out[1] == 5 && out[2] == 0 && out[3] == 3 &&
	       memcmp(out + 4, "a/b", 3) == 0);

	n = xf16cam_mqtt_publish_header(out, sizeof(out), "a/b", 122, 1);
	assert(n == 7 && out[0] == 0x31 && out[1] == 127);

	n = xf16cam_mqtt_publish_header(out, sizeof(out), "a/b", 123, 0);
	assert(n == 8 && out[1] == 0x80 && out[2] == 0x01 && out[3] == 0 && out[4] == 3);

	n = xf16cam_mqtt_publish_header(out, sizeof(out), "a/b", 16383 - 5, 0);
	assert(n == 8 && out[1] == 0xff && out[2] == 0x7f);

	n = xf16cam_mqtt_publish_header(out, sizeof(out), "a/b", 100000, 1);
	/* 100005 = 0x186A5 -> varint a5 8d 06 */
	assert(n == 9 && out[0] == 0x31 && out[1] == 0xa5 && out[2] == 0x8d &&
	       out[3] == 0x06 && out[4] == 0 && out[5] == 3);

	assert(xf16cam_mqtt_publish_header(out, 6, "a/b", 0, 0) == 0);
	assert(xf16cam_mqtt_publish_header(out, sizeof(out), "", 0, 0) == 0);
}

int main(void)
{
	test_topics();
	test_state_web_and_rtsp();
	test_discovery();
	test_publish_header();
	printf("xf16cam MQTT payload tests passed\n");
	return 0;
}
