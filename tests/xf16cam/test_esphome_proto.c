/* Host test for the ESPHome protobuf helpers and plaintext framing.
 * Build and run:
 *   cc -std=c11 -Wall -Wextra -Werror -Iinclude -Iproject/example/xf16cam \
 *     tests/xf16cam/test_esphome_proto.c \
 *     project/example/xf16cam/xf16cam_esphome_proto.c -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xf16cam_esphome_proto.h"

static void test_varint(void)
{
	uint8_t out[5];
	uint32_t value;

	assert(xf16cam_pb_varint(out, 0) == 1 && out[0] == 0);
	assert(xf16cam_pb_varint(out, 127) == 1 && out[0] == 0x7f);
	assert(xf16cam_pb_varint(out, 128) == 2 && out[0] == 0x80 && out[1] == 0x01);
	assert(xf16cam_pb_varint(out, 300) == 2 && out[0] == 0xac && out[1] == 0x02);
	assert(xf16cam_pb_varint(out, 0xffffffffU) == 5 && out[4] == 0x0f);

	assert(xf16cam_pb_varint_parse(out, 5, &value) == 5 && value == 0xffffffffU);
	assert(xf16cam_pb_varint_parse(out, 3, &value) == 0);	/* incomplete */
	memset(out, 0x80, sizeof(out));
	assert(xf16cam_pb_varint_parse(out, 5, &value) == -1);	/* too long */
	out[0] = 0x05;
	assert(xf16cam_pb_varint_parse(out, 1, &value) == 1 && value == 5);
}

static void test_writer(void)
{
	uint8_t buf[64];
	XF16CamPbWriter w;
	/* HelloResponse{major=1, minor=10, server_info="x", name="cam"} */
	static const uint8_t expected[] = {
		0x08, 0x01, 0x10, 0x0a, 0x1a, 0x01, 'x', 0x22, 0x03, 'c', 'a', 'm'
	};

	xf16cam_pb_init(&w, buf, sizeof(buf));
	xf16cam_pb_uint(&w, 1, 1);
	xf16cam_pb_uint(&w, 2, 10);
	xf16cam_pb_string(&w, 3, "x");
	xf16cam_pb_string(&w, 4, "cam");
	xf16cam_pb_uint(&w, 5, 0);		/* zero is omitted */
	xf16cam_pb_string(&w, 6, "");		/* empty is omitted */
	xf16cam_pb_string(&w, 7, NULL);
	assert(xf16cam_pb_len(&w) == sizeof(expected));
	assert(memcmp(buf, expected, sizeof(expected)) == 0);

	/* fixed32 key and a float state: SensorStateResponse{key=7, state=1.5} */
	xf16cam_pb_init(&w, buf, sizeof(buf));
	xf16cam_pb_fixed32(&w, 1, 7);
	xf16cam_pb_float(&w, 2, 1.5f);
	assert(xf16cam_pb_len(&w) == 10);
	assert(buf[0] == 0x0d && buf[1] == 7 && buf[4] == 0);
	assert(buf[5] == 0x15 && buf[6] == 0x00 && buf[8] == 0xc0 && buf[9] == 0x3f);

	/* overflow is sticky and reports zero length */
	xf16cam_pb_init(&w, buf, 4);
	xf16cam_pb_string(&w, 1, "toolong");
	xf16cam_pb_uint(&w, 2, 1);
	assert(xf16cam_pb_len(&w) == 0);
}

static void test_reader(void)
{
	/* LightCommandRequest{key=2 (fixed32), has_state=true, state=true,
	 * brightness=0.5 (field 5, wire 5), effect="x" (field 19), and a
	 * negative int32 as a 10-byte varint in field 30 } */
	static const uint8_t msg[] = {
		0x0d, 0x02, 0x00, 0x00, 0x00,
		0x10, 0x01,
		0x18, 0x01,
		0x2d, 0x00, 0x00, 0x00, 0x3f,
		0x9a, 0x01, 0x01, 'x',
		0xf0, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
	};
	XF16CamPbReader r;
	uint32_t key = 0;
	int has_state = 0;
	int state = 0;
	int seen_effect = 0;
	int seen_negative = 0;

	xf16cam_pb_reader_init(&r, msg, sizeof(msg));
	while (xf16cam_pb_next(&r) == 1) {
		switch (r.field) {
		case 1: assert(r.wire == 5); key = r.value; break;
		case 2: has_state = r.value != 0; break;
		case 3: state = r.value != 0; break;
		case 19:
			assert(r.wire == 2 && r.data_len == 1 && r.data[0] == 'x');
			seen_effect = 1;
			break;
		case 30:
			assert(r.wire == 0 && r.value == 0xffffffffU);
			seen_negative = 1;
			break;
		default: break;
		}
	}
	assert(r.pos == sizeof(msg));
	assert(key == 2 && has_state && state && seen_effect && seen_negative);

	/* truncated length-delimited field is malformed, not read past the end */
	xf16cam_pb_reader_init(&r, msg + 14, 3);	/* 0x9a 0x01 0x01 then nothing */
	assert(xf16cam_pb_next(&r) == -1);
	/* empty message ends immediately */
	xf16cam_pb_reader_init(&r, msg, 0);
	assert(xf16cam_pb_next(&r) == 0);
}

static void test_frame_header(void)
{
	uint8_t out[XF16CAM_PB_FRAME_HEADER_MAX];

	/* type 2 (HelloResponse), 12-byte payload: 00 0c 02 */
	assert(xf16cam_pb_frame_header(out, 2, 12) == 3);
	assert(out[0] == 0x00 && out[1] == 12 && out[2] == 2);
	/* type 44 (CameraImageResponse), 1400-byte payload: 00 f8 0a 2c */
	assert(xf16cam_pb_frame_header(out, 44, 1400) == 4);
	assert(out[1] == 0xf8 && out[2] == 0x0a && out[3] == 44);
	/* type 124 fits one byte, type 149 needs two */
	assert(xf16cam_pb_frame_header(out, 124, 0) == 3 && out[2] == 124);
	assert(xf16cam_pb_frame_header(out, 149, 0) == 4 && out[2] == 0x95 && out[3] == 0x01);
}

int main(void)
{
	test_varint();
	test_writer();
	test_reader();
	test_frame_header();
	printf("xf16cam ESPHome proto tests passed\n");
	return 0;
}
