#ifndef XF16CAM_ESPHOME_PROTO_H
#define XF16CAM_ESPHOME_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Protobuf wire helpers and the plaintext frame layout of the ESPHome native
 * API, free of SDK dependencies so tests/xf16cam/test_esphome_proto.c can
 * compile them on the host. Only what the camera needs: varint, fixed32,
 * float and length-delimited fields. Zero, false and empty values are left
 * out, as proto3 and ESPHome's own encoder do.
 *
 * Writers are bounds-checked and sticky: after an overflow every further
 * write is ignored and xf16cam_pb_len() returns 0, so a caller checks once
 * at the end.
 */

typedef struct {
	uint8_t *buf;
	size_t size;
	size_t len;
	uint8_t overflow;
} XF16CamPbWriter;

void xf16cam_pb_init(XF16CamPbWriter *w, uint8_t *buf, size_t size);
size_t xf16cam_pb_len(const XF16CamPbWriter *w);
void xf16cam_pb_uint(XF16CamPbWriter *w, uint32_t field, uint32_t value);
void xf16cam_pb_fixed32(XF16CamPbWriter *w, uint32_t field, uint32_t value);
void xf16cam_pb_float(XF16CamPbWriter *w, uint32_t field, float value);
void xf16cam_pb_string(XF16CamPbWriter *w, uint32_t field, const char *text);
void xf16cam_pb_bytes(XF16CamPbWriter *w, uint32_t field, const void *data,
                      size_t len);

/* Encodes value into out; returns the byte count, 1 to 5. */
size_t xf16cam_pb_varint(uint8_t out[5], uint32_t value);
/* Decodes a varint of at most 5 bytes. Returns the bytes consumed, 0 when
 * the input ends before the varint does, -1 when it is longer than 5 bytes. */
int xf16cam_pb_varint_parse(const uint8_t *in, size_t len, uint32_t *value);

typedef struct {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	uint32_t field;
	uint8_t wire;		/* 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32 */
	uint32_t value;		/* varint (low 32 bits) or fixed32 */
	const uint8_t *data;	/* length-delimited payload */
	size_t data_len;
} XF16CamPbReader;

void xf16cam_pb_reader_init(XF16CamPbReader *r, const uint8_t *buf, size_t len);
/* 1: a field is in r->field, wire, value and data; 0: end; -1: malformed. */
int xf16cam_pb_next(XF16CamPbReader *r);

/* Plaintext framing: 0x00, varint payload length, varint message type,
 * payload. Writes the header for a payload of len bytes; returns its size. */
#define XF16CAM_PB_FRAME_HEADER_MAX  (11)
size_t xf16cam_pb_frame_header(uint8_t out[XF16CAM_PB_FRAME_HEADER_MAX],
                               uint32_t type, size_t len);

#endif
