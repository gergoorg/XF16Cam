#ifndef XF16CAM_RTSP_PARSER_H
#define XF16CAM_RTSP_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define XF16CAM_RTSP_INPUT_CAPACITY (1024U)

/* Receives every complete interleaved binary frame ("$" channel length data)
 * before the parser discards it. frame points at the RTP or RTCP packet that
 * follows the 4-byte interleaved header. Unset after init, so frames are
 * dropped exactly as before. */
typedef void (*XF16CamRtspBinarySink)(void *context, uint8_t channel,
				      const uint8_t *frame, size_t length);

typedef struct {
	char data[XF16CAM_RTSP_INPUT_CAPACITY + 1U];
	size_t length;
	size_t message_length;
	char saved_byte;
	XF16CamRtspBinarySink sink;
	void *sink_context;
} XF16CamRtspParser;

void xf16cam_rtsp_parser_init(XF16CamRtspParser *parser);
char *xf16cam_rtsp_parser_write_ptr(XF16CamRtspParser *parser);
size_t xf16cam_rtsp_parser_writable(const XF16CamRtspParser *parser);
int xf16cam_rtsp_parser_commit(XF16CamRtspParser *parser, size_t length);
int xf16cam_rtsp_parser_next(XF16CamRtspParser *parser, const char **request);
void xf16cam_rtsp_parser_consume(XF16CamRtspParser *parser);
int xf16cam_rtsp_header_value(const char *request, const char *name,
			      const char **value, size_t *length);
int xf16cam_rtsp_contains_ci(const char *text, size_t length, const char *needle);
void xf16cam_rtsp_parser_set_sink(XF16CamRtspParser *parser,
				  XF16CamRtspBinarySink sink, void *context);

/* Locates the payload of an RTP packet (RFC 3550): checks the version, skips
 * CSRC entries and a header extension, and strips padding. Returns 0 with the
 * payload offset and length, or -1 for a malformed packet. The payload type
 * is left to the caller (packet[1] & 0x7f). */
int xf16cam_rtp_payload(const uint8_t *packet, size_t length,
			size_t *offset, size_t *payload_length);

#endif
