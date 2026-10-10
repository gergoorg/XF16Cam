#ifndef XF16CAM_AUDIO_H
#define XF16CAM_AUDIO_H

#include <stdint.h>

#define XF16CAM_AUDIO_SAMPLES_PER_PACKET (160)

typedef struct {
	uint32_t packets;
	uint32_t read_errors;
	uint16_t peak;
	uint16_t mean;
	uint8_t available;
	uint8_t active;
} XF16CamAudioInfo;

int xf16cam_audio_start(void);
int xf16cam_audio_acquire(void);
void xf16cam_audio_release(void);
int xf16cam_audio_http_start(int fd);
int xf16cam_audio_update_ready(void);
const XF16CamAudioInfo *xf16cam_audio_info(void);
uint32_t xf16cam_audio_cursor(void);
int xf16cam_audio_read(uint32_t *cursor, uint8_t *pcmu, uint32_t *timestamp);
uint32_t xf16cam_audio_stack_min_free(void);

#ifdef XF16CAM_TALK
/* Speaker playback for the RTSP audio backchannel (ONVIF Profile T style).
 * PCMU/8000 pushed by one talker at a time is decoded and written to the
 * XR872 internal codec line-out; the microphone is silenced while the
 * remote side is talking because the SDK has no echo cancellation. */
typedef struct {
	uint32_t packets;    /* RTP payloads accepted into the ring */
	uint32_t dropped;    /* payloads discarded because the ring was full */
	uint32_t underruns;  /* silent frames written while the DAC was open */
	uint32_t errors;     /* snd_pcm_write failures */
	uint8_t available;
	uint8_t active;      /* DAC open */
} XF16CamTalkInfo;

int xf16cam_talk_acquire(void);
void xf16cam_talk_release(void);
int xf16cam_talk_push(const uint8_t *pcmu, uint32_t length);
const XF16CamTalkInfo *xf16cam_talk_info(void);
uint32_t xf16cam_talk_stack_min_free(void);
#endif

#endif
