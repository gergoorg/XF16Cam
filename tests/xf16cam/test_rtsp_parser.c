#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xf16cam_rtsp_parser.h"

static void feed(XF16CamRtspParser *parser, const void *data, size_t length)
{
	assert(length <= xf16cam_rtsp_parser_writable(parser));
	memcpy(xf16cam_rtsp_parser_write_ptr(parser), data, length);
	assert(xf16cam_rtsp_parser_commit(parser, length) == 0);
}

static void expect_request(XF16CamRtspParser *parser, const char *expected)
{
	const char *request;

	assert(xf16cam_rtsp_parser_next(parser, &request) == 1);
	assert(strcmp(request, expected) == 0);
	xf16cam_rtsp_parser_consume(parser);
}

static void test_split_and_coalesced(void)
{
	static const char first[] = "OPTIONS rtsp://cam/stream RTSP/1.0\r\nCSeq: 1\r\n\r\n";
	static const char second[] = "PLAY rtsp://cam/stream RTSP/1.0\r\nCSeq: 2\r\n\r\n";
	XF16CamRtspParser parser;
	const char *request;

	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, first, 7);
	assert(xf16cam_rtsp_parser_next(&parser, &request) == 0);
	feed(&parser, first + 7, sizeof(first) - 1U - 7U);
	feed(&parser, second, sizeof(second) - 1U);
	expect_request(&parser, first);
	expect_request(&parser, second);
	assert(xf16cam_rtsp_parser_next(&parser, &request) == 0);
}

static void test_body_and_interleaved_rtcp(void)
{
	static const unsigned char rtcp[] = { '$', 1, 0, 4, 0x81, 0xc9, 0, 0 };
	static const char request[] =
		"GET_PARAMETER rtsp://cam/stream RTSP/1.0\r\n"
		"cOnTeNt-LeNgTh: 4\r\nCSeq: 3\r\n\r\nping";
	XF16CamRtspParser parser;
	const char *parsed;

	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, rtcp, sizeof(rtcp));
	feed(&parser, request, sizeof(request) - 3U);
	assert(xf16cam_rtsp_parser_next(&parser, &parsed) == 0);
	feed(&parser, request + sizeof(request) - 3U, 2U);
	expect_request(&parser, request);
}

static void test_rejects_malformed_lengths(void)
{
	static const char oversized[] =
		"OPTIONS * RTSP/1.0\r\nContent-Length: 1024\r\n\r\n";
	static const char duplicate[] =
		"OPTIONS * RTSP/1.0\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n";
	static const unsigned char oversized_rtcp[] = { '$', 1, 4, 1 };
	XF16CamRtspParser parser;
	const char *request;

	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, oversized, sizeof(oversized) - 1U);
	assert(xf16cam_rtsp_parser_next(&parser, &request) == -1);
	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, duplicate, sizeof(duplicate) - 1U);
	assert(xf16cam_rtsp_parser_next(&parser, &request) == -1);
	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, oversized_rtcp, sizeof(oversized_rtcp));
	assert(xf16cam_rtsp_parser_next(&parser, &request) == -1);
}

static void test_case_insensitive_headers(void)
{
	static const char request[] =
		"SETUP rtsp://cam/stream/track1 RTSP/1.0\r\n"
		"cSeQ:\t42 \r\n"
		"tRaNsPoRt: rtp/avp/tcp;Unicast;INTERLEAVED=4-5\r\n\r\n";
	const char *value;
	size_t length;

	assert(xf16cam_rtsp_header_value(request, "CSeq", &value, &length) == 1);
	assert(length == 2U && memcmp(value, "42", 2U) == 0);
	assert(xf16cam_rtsp_header_value(request, "Transport", &value, &length) == 1);
	assert(xf16cam_rtsp_contains_ci(value, length, "RTP/AVP/TCP"));
	assert(xf16cam_rtsp_contains_ci(value, length, "interleaved=4-5"));
	assert(xf16cam_rtsp_header_value(request, "Session", &value, &length) == 0);
	assert(xf16cam_rtsp_header_value("broken", "CSeq", &value, &length) == -1);
}

typedef struct {
	unsigned int calls;
	uint8_t channel;
	size_t length;
	uint8_t frame[16];
} SinkRecord;

static void record_frame(void *context, uint8_t channel, const uint8_t *frame,
			 size_t length)
{
	SinkRecord *record = context;

	record->calls++;
	record->channel = channel;
	record->length = length;
	if (length <= sizeof(record->frame))
		memcpy(record->frame, frame, length);
}

static void test_binary_sink(void)
{
	static const unsigned char rtp[] = { '$', 4, 0, 6, 0x80, 0, 0, 1, 0xaa, 0xbb };
	static const char request[] = "GET_PARAMETER rtsp://cam/stream RTSP/1.0\r\nCSeq: 7\r\n\r\n";
	XF16CamRtspParser parser;
	SinkRecord record;
	const char *parsed;

	memset(&record, 0, sizeof(record));
	xf16cam_rtsp_parser_init(&parser);
	xf16cam_rtsp_parser_set_sink(&parser, record_frame, &record);
	/* A frame split across reads reaches the sink once, when complete. */
	feed(&parser, rtp, 5);
	assert(xf16cam_rtsp_parser_next(&parser, &parsed) == 0);
	assert(record.calls == 0);
	feed(&parser, rtp + 5, sizeof(rtp) - 5U);
	feed(&parser, request, sizeof(request) - 1U);
	expect_request(&parser, request);
	assert(record.calls == 1);
	assert(record.channel == 4);
	assert(record.length == 6);
	assert(memcmp(record.frame, rtp + 4, 6) == 0);
	/* Without a sink the frame is dropped as before. */
	xf16cam_rtsp_parser_init(&parser);
	feed(&parser, rtp, sizeof(rtp));
	assert(xf16cam_rtsp_parser_next(&parser, &parsed) == 0);
	assert(record.calls == 1);
}

static void test_rtp_payload(void)
{
	unsigned char packet[40];
	size_t offset;
	size_t length;

	memset(packet, 0, sizeof(packet));
	packet[0] = 0x80;
	assert(xf16cam_rtp_payload(packet, 15, &offset, &length) == 0);
	assert(offset == 12 && length == 3);
	assert(xf16cam_rtp_payload(packet, 12, &offset, &length) == 0);
	assert(offset == 12 && length == 0);
	assert(xf16cam_rtp_payload(packet, 11, &offset, &length) == -1);
	/* Two CSRC entries. */
	packet[0] = 0x82;
	assert(xf16cam_rtp_payload(packet, 22, &offset, &length) == 0);
	assert(offset == 20 && length == 2);
	/* Header extension of one 32-bit word. */
	packet[0] = 0x90;
	packet[15] = 1;
	assert(xf16cam_rtp_payload(packet, 24, &offset, &length) == 0);
	assert(offset == 20 && length == 4);
	assert(xf16cam_rtp_payload(packet, 14, &offset, &length) == -1);
	/* Padding: last byte counts the padding bytes. */
	packet[0] = 0xa0;
	packet[15] = 0;
	packet[19] = 2;
	assert(xf16cam_rtp_payload(packet, 20, &offset, &length) == 0);
	assert(offset == 12 && length == 6);
	packet[19] = 0;
	assert(xf16cam_rtp_payload(packet, 20, &offset, &length) == -1);
	packet[19] = 9;
	assert(xf16cam_rtp_payload(packet, 20, &offset, &length) == -1);
	/* Wrong RTP version. */
	packet[0] = 0x40;
	assert(xf16cam_rtp_payload(packet, 20, &offset, &length) == -1);
}

int main(void)
{
	test_split_and_coalesced();
	test_body_and_interleaved_rtcp();
	test_rejects_malformed_lengths();
	test_case_insensitive_headers();
	test_binary_sink();
	test_rtp_payload();
	puts("xf16cam RTSP parser tests passed");
	return 0;
}
