/*
 * Protobuf wire helpers for the ESPHome native API. Pure C, no SDK includes;
 * see xf16cam_esphome_proto.h. Every function is __xip_text (compiler.h),
 * which is empty on the host.
 */
#include <string.h>

#include "xf16cam_esphome_proto.h"

#include "compiler.h"

#define PB_WIRE_VARINT   (0)
#define PB_WIRE_FIXED64  (1)
#define PB_WIRE_LENGTH   (2)
#define PB_WIRE_FIXED32  (5)

__xip_text
size_t xf16cam_pb_varint(uint8_t out[5], uint32_t value)
{
	size_t n = 0;

	do {
		uint8_t byte = (uint8_t)(value & 0x7fU);

		value >>= 7;
		out[n++] = value != 0 ? (uint8_t)(byte | 0x80U) : byte;
	} while (value != 0);
	return n;
}

__xip_text
int xf16cam_pb_varint_parse(const uint8_t *in, size_t len, uint32_t *value)
{
	uint32_t result = 0;
	size_t i;

	for (i = 0; i < len && i < 5; ++i) {
		result |= (uint32_t)(in[i] & 0x7fU) << (7 * i);
		if ((in[i] & 0x80U) == 0) {
			*value = result;
			return (int)i + 1;
		}
	}
	return i == 5 ? -1 : 0;
}

__xip_text
void xf16cam_pb_init(XF16CamPbWriter *w, uint8_t *buf, size_t size)
{
	w->buf = buf;
	w->size = size;
	w->len = 0;
	w->overflow = 0;
}

__xip_text
size_t xf16cam_pb_len(const XF16CamPbWriter *w)
{
	return w->overflow ? 0 : w->len;
}

__xip_text
static void xf16cam_pb_put(XF16CamPbWriter *w, const void *data, size_t len)
{
	if (w->overflow || len > w->size - w->len) {
		w->overflow = 1;
		return;
	}
	memcpy(w->buf + w->len, data, len);
	w->len += len;
}

__xip_text
static void xf16cam_pb_put_varint(XF16CamPbWriter *w, uint32_t value)
{
	uint8_t tmp[5];
	size_t n = xf16cam_pb_varint(tmp, value);

	xf16cam_pb_put(w, tmp, n);
}

__xip_text
static void xf16cam_pb_tag(XF16CamPbWriter *w, uint32_t field, uint32_t wire)
{
	xf16cam_pb_put_varint(w, (field << 3) | wire);
}

__xip_text
void xf16cam_pb_uint(XF16CamPbWriter *w, uint32_t field, uint32_t value)
{
	if (value == 0)
		return;
	xf16cam_pb_tag(w, field, PB_WIRE_VARINT);
	xf16cam_pb_put_varint(w, value);
}

__xip_text
void xf16cam_pb_fixed32(XF16CamPbWriter *w, uint32_t field, uint32_t value)
{
	uint8_t tmp[4];

	if (value == 0)
		return;
	tmp[0] = (uint8_t)value;
	tmp[1] = (uint8_t)(value >> 8);
	tmp[2] = (uint8_t)(value >> 16);
	tmp[3] = (uint8_t)(value >> 24);
	xf16cam_pb_tag(w, field, PB_WIRE_FIXED32);
	xf16cam_pb_put(w, tmp, sizeof(tmp));
}

__xip_text
void xf16cam_pb_float(XF16CamPbWriter *w, uint32_t field, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	xf16cam_pb_fixed32(w, field, bits);
}

__xip_text
void xf16cam_pb_bytes(XF16CamPbWriter *w, uint32_t field, const void *data,
                      size_t len)
{
	if (len == 0)
		return;
	xf16cam_pb_tag(w, field, PB_WIRE_LENGTH);
	xf16cam_pb_put_varint(w, (uint32_t)len);
	xf16cam_pb_put(w, data, len);
}

__xip_text
void xf16cam_pb_string(XF16CamPbWriter *w, uint32_t field, const char *text)
{
	xf16cam_pb_bytes(w, field, text, text != NULL ? strlen(text) : 0);
}

__xip_text
void xf16cam_pb_reader_init(XF16CamPbReader *r, const uint8_t *buf, size_t len)
{
	memset(r, 0, sizeof(*r));
	r->buf = buf;
	r->len = len;
}

__xip_text
int xf16cam_pb_next(XF16CamPbReader *r)
{
	uint32_t tag;
	int n;

	if (r->pos >= r->len)
		return 0;
	n = xf16cam_pb_varint_parse(r->buf + r->pos, r->len - r->pos, &tag);
	if (n <= 0)
		return -1;
	r->pos += (size_t)n;
	r->field = tag >> 3;
	r->wire = (uint8_t)(tag & 7U);
	r->value = 0;
	r->data = NULL;
	r->data_len = 0;
	switch (r->wire) {
	case PB_WIRE_VARINT: {
		/* ESPHome fields are 32-bit; a wider varint (a negative int32)
		 * is consumed in full and only its low 32 bits kept. */
		uint32_t value = 0;
		size_t i;

		for (i = 0; i < 10; ++i) {
			uint8_t byte;

			if (r->pos >= r->len)
				return -1;
			byte = r->buf[r->pos++];
			if (i < 5)
				value |= (uint32_t)(byte & 0x7fU) << (7 * i);
			if ((byte & 0x80U) == 0)
				break;
		}
		if (i == 10)
			return -1;
		r->value = value;
		return 1;
	}
	case PB_WIRE_FIXED64:
		if (r->len - r->pos < 8)
			return -1;
		r->value = (uint32_t)r->buf[r->pos] | (uint32_t)r->buf[r->pos + 1] << 8 |
		           (uint32_t)r->buf[r->pos + 2] << 16 | (uint32_t)r->buf[r->pos + 3] << 24;
		r->pos += 8;
		return 1;
	case PB_WIRE_LENGTH: {
		uint32_t length;

		n = xf16cam_pb_varint_parse(r->buf + r->pos, r->len - r->pos, &length);
		if (n <= 0)
			return -1;
		r->pos += (size_t)n;
		if (length > r->len - r->pos)
			return -1;
		r->data = r->buf + r->pos;
		r->data_len = length;
		r->pos += length;
		return 1;
	}
	case PB_WIRE_FIXED32:
		if (r->len - r->pos < 4)
			return -1;
		r->value = (uint32_t)r->buf[r->pos] | (uint32_t)r->buf[r->pos + 1] << 8 |
		           (uint32_t)r->buf[r->pos + 2] << 16 | (uint32_t)r->buf[r->pos + 3] << 24;
		r->pos += 4;
		return 1;
	default:
		return -1;
	}
}

__xip_text
size_t xf16cam_pb_frame_header(uint8_t out[XF16CAM_PB_FRAME_HEADER_MAX],
                               uint32_t type, size_t len)
{
	size_t n = 1;

	out[0] = 0x00;
	n += xf16cam_pb_varint(out + n, (uint32_t)len);
	n += xf16cam_pb_varint(out + n, type);
	return n;
}
