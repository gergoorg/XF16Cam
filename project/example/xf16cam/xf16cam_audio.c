#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/manager/audio_manager.h"
#include "audio/pcm/audio_pcm.h"
#include "compiler.h"
#include "kernel/os/os.h"
#include "lwip/sockets.h"

#include "xf16cam_audio.h"
#include "xf16cam_config.h"
#include "xf16cam_media.h"

#define XF16CAM_AUDIO_RATE          (8000)
#define XF16CAM_AUDIO_RING_PACKETS  (8)
#define XF16CAM_AUDIO_STACK_SIZE    (1536)
#define XF16CAM_AUDIO_HTTP_STACK    (1536)
#define XF16CAM_AUDIO_MIC_LEVEL     (VOLUME_LEVEL3)
#define XF16CAM_AUDIO_WARMUP_PACKETS (105)

#ifdef XF16CAM_TALK
/* Ring sizes are bytes of PCMU/8000, i.e. milliseconds times eight. The RTSP
 * session thread only drains its socket between video frames, so the ring
 * must ride out a few frame periods and playback starts only once
 * XF16CAM_TALK_PRIME_BYTES are buffered. */
#define XF16CAM_TALK_RING_BYTES     (4096U)
#define XF16CAM_TALK_PRIME_BYTES    (1600U)
#define XF16CAM_TALK_STACK_SIZE     (1536)
#define XF16CAM_TALK_IDLE_MS        (1500U)
#define XF16CAM_TALK_MUTE_MS        (500U)
#define XF16CAM_TALK_VOLUME         (VOLUME_LEVEL31)	/* line-out gain: 31 is 0 dB, -1.5 dB per step */
#endif

typedef struct {
	OS_Thread_t thread;
	volatile int active;
	int fd;
} XF16CamAudioHttpClient;

typedef struct {
	uint8_t pcmu[XF16CAM_AUDIO_SAMPLES_PER_PACKET];
	uint32_t timestamp;
	volatile uint32_t generation;
} XF16CamAudioPacket;

static OS_Thread_t g_audio_thread;
static OS_Mutex_t g_audio_lock;
static int g_audio_lock_ready;
static XF16CamAudioPacket g_audio_ring[XF16CAM_AUDIO_RING_PACKETS];
static volatile uint32_t g_audio_packets;
static XF16CamAudioInfo g_audio_info;
static volatile uint32_t g_audio_users;
static XF16CamAudioHttpClient g_audio_http_clients[XF16CAM_MAX_PARALLEL_CLIENTS];
static volatile uint32_t g_audio_http_clients_active;
static volatile int g_audio_update_quiesced = 1;

#ifdef XF16CAM_TALK
static OS_Thread_t g_talk_thread;
static uint8_t g_talk_ring[XF16CAM_TALK_RING_BYTES];
/* Single producer (the one RTSP session holding the talk slot), single
 * consumer (the playback task): monotonic byte counters, no lock. */
static volatile uint32_t g_talk_head;
static volatile uint32_t g_talk_tail;
static volatile uint32_t g_talk_users;
static volatile uint32_t g_talk_last_ms;
static volatile int g_talk_quiesced = 1;
static XF16CamTalkInfo g_talk_info;

/* ITU-T G.711 mu-law decoder. */
__xip_text
static int16_t xf16cam_mulaw_decode(uint8_t encoded)
{
	uint8_t value = (uint8_t)~encoded;
	int sample = (((value & 0x0f) << 3) + 0x84) << ((value >> 4) & 0x07);

	sample -= 0x84;
	return (int16_t)((value & 0x80) ? -sample : sample);
}

/* Half-duplex: the microphone is silenced while the speaker is playing so a
 * talker does not hear their own voice back; the SDK has no echo canceller. */
__xip_text
static int xf16cam_talk_muting(void)
{
	return g_talk_info.active &&
	       OS_TicksToMSecs(OS_GetTicks()) - g_talk_last_ms < XF16CAM_TALK_MUTE_MS;
}

__xip_text
static void xf16cam_talk_task(void *arg)
{
	struct pcm_config config;
	int16_t pcm[XF16CAM_AUDIO_SAMPLES_PER_PACKET];
	int opened = 0;
	(void)arg;

	memset(&config, 0, sizeof(config));
	config.channels = 1;
	config.format = PCM_FORMAT_S16_LE;
	config.period_count = 4;
	config.period_size = XF16CAM_AUDIO_SAMPLES_PER_PACKET;
	config.rate = XF16CAM_AUDIO_RATE;
	g_talk_info.available = 1;
	printf("xf16cam talk: speaker ready; playback starts on demand\n");
	while (1) {
		uint32_t now = OS_TicksToMSecs(OS_GetTicks());
		uint32_t buffered = g_talk_head - g_talk_tail;
		int stopping = xf16cam_update_active() || g_talk_users == 0;
		int i;

		if (stopping || (buffered < XF16CAM_AUDIO_SAMPLES_PER_PACKET &&
		                 now - g_talk_last_ms > XF16CAM_TALK_IDLE_MS)) {
			if (opened) {
				snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_OUT);
				opened = 0;
				g_talk_info.active = 0;
				printf("xf16cam talk: speaker off\n");
			}
			g_talk_quiesced = 1;
			if (stopping)
				g_talk_tail = g_talk_head;	/* discard stale audio */
			OS_MSleep(20);
			continue;
		}
		if (!opened) {
			if (buffered < XF16CAM_TALK_PRIME_BYTES) {
				OS_MSleep(10);
				continue;
			}
			g_talk_quiesced = 0;
			audio_manager_handler(AUDIO_SND_CARD_DEFAULT, AUDIO_MANAGER_SET_VOLUME_LEVEL,
			                      AUDIO_OUT_DEV_SPK, XF16CAM_TALK_VOLUME);
			if (snd_pcm_open(AUDIO_SND_CARD_DEFAULT, PCM_OUT, &config) != 0) {
				printf("xf16cam talk: speaker open failed\n");
				g_talk_info.available = 0;
				g_talk_quiesced = 1;
				OS_ThreadDelete(&g_talk_thread);
				return;
			}
			opened = 1;
			g_talk_info.active = 1;
			printf("xf16cam talk: speaker on, PCMU/8000 backchannel\n");
		}
		if (buffered >= XF16CAM_AUDIO_SAMPLES_PER_PACKET) {
			uint32_t tail = g_talk_tail;

			for (i = 0; i < XF16CAM_AUDIO_SAMPLES_PER_PACKET; ++i)
				pcm[i] = xf16cam_mulaw_decode(
					g_talk_ring[(tail + (uint32_t)i) & (XF16CAM_TALK_RING_BYTES - 1U)]);
			__sync_synchronize();
			g_talk_tail = tail + XF16CAM_AUDIO_SAMPLES_PER_PACKET;
		} else {
			/* Keep the DAC clocked through a jitter gap. */
			memset(pcm, 0, sizeof(pcm));
			g_talk_info.underruns++;
		}
		if (snd_pcm_write(AUDIO_SND_CARD_DEFAULT, pcm, sizeof(pcm)) != (int)sizeof(pcm)) {
			g_talk_info.errors++;
			OS_MSleep(20);
		}
	}
}
#endif

static int xf16cam_audio_send_all(int fd, const void *data, uint32_t length)
{
	const uint8_t *cursor = data;

	while (length > 0) {
		int sent = send(fd, cursor, length, 0);
		if (sent <= 0)
			return -1;
		cursor += sent;
		length -= (uint32_t)sent;
	}
	return 0;
}

/* ITU-T G.711 mu-law encoder. */
static uint8_t xf16cam_mulaw(int16_t sample)
{
	static const int16_t ends[8] = { 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff, 0x3fff, 0x7fff };
	int value = sample;
	int mask;
	int segment;
	uint8_t encoded;

	if (value < 0) {
		value = -value;
		mask = 0x7f;
	} else {
		mask = 0xff;
	}
	if (value > 32635)
		value = 32635;
	value += 132;
	for (segment = 0; segment < 8 && value > ends[segment]; ++segment)
		;
	encoded = (uint8_t)((segment << 4) | ((value >> (segment + 3)) & 0x0f));
	return encoded ^ mask;
}

static void xf16cam_audio_task(void *arg)
{
	struct pcm_config config;
	int16_t pcm[XF16CAM_AUDIO_SAMPLES_PER_PACKET];
	int opened = 0;
	unsigned int warmup_packets = 0;
	(void)arg;

	memset(&config, 0, sizeof(config));
	config.channels = 1;
	config.format = PCM_FORMAT_S16_LE;
	config.period_count = 2;
	config.period_size = XF16CAM_AUDIO_SAMPLES_PER_PACKET;
	config.rate = XF16CAM_AUDIO_RATE;
	g_audio_info.available = 1;
	g_audio_update_quiesced = 1;
	printf("xf16cam audio: AMIC ready; capture starts on demand\n");
	while (1) {
		uint32_t packet = g_audio_packets;
		XF16CamAudioPacket *slot = &g_audio_ring[packet % XF16CAM_AUDIO_RING_PACKETS];
		uint32_t total = 0;
		uint16_t peak = 0;
		int i;

		if (xf16cam_update_active() || g_audio_users == 0) {
			if (opened) {
				snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_IN);
				opened = 0;
				g_audio_info.active = 0;
			}
			g_audio_update_quiesced = 1;
			while (xf16cam_update_active())
				OS_MSleep(20);
			OS_MSleep(20);
			continue;
		}
		if (!opened) {
			g_audio_update_quiesced = 0;
			audio_manager_handler(AUDIO_SND_CARD_DEFAULT, AUDIO_MANAGER_SET_VOLUME_LEVEL,
			                      AUDIO_IN_DEV_AMIC, XF16CAM_AUDIO_MIC_LEVEL);
			if (snd_pcm_open(AUDIO_SND_CARD_DEFAULT, PCM_IN, &config) != 0) {
				printf("xf16cam audio: AMIC open failed\n");
				g_audio_info.available = 0;
				g_audio_update_quiesced = 1;
				OS_ThreadDelete(&g_audio_thread);
				return;
			}
			opened = 1;
			warmup_packets = XF16CAM_AUDIO_WARMUP_PACKETS;
			g_audio_info.active = 1;
			printf("xf16cam audio: AMIC warming for %u ms, gain_level=%u\n",
			       (unsigned int)(XF16CAM_AUDIO_WARMUP_PACKETS * 20U),
			       (unsigned int)XF16CAM_AUDIO_MIC_LEVEL);
		}
		if (xf16cam_update_active()) {
			snd_pcm_close(AUDIO_SND_CARD_DEFAULT, PCM_IN);
			opened = 0;
			g_audio_info.active = 0;
			g_audio_update_quiesced = 1;
			while (xf16cam_update_active())
				OS_MSleep(20);
			continue;
		}
		if (snd_pcm_read(AUDIO_SND_CARD_DEFAULT, pcm, sizeof(pcm)) != sizeof(pcm)) {
			g_audio_info.read_errors++;
			/* Avoid monopolising the CPU if the input device fails immediately. */
			OS_MSleep(20);
			continue;
		}
		if (warmup_packets > 0) {
			/* Preserve the 8 kHz media clock while suppressing the analogue
			 * startup transient. PCMU 0xff is zero-level silence. */
			memset(slot->pcmu, 0xff, sizeof(slot->pcmu));
			if (--warmup_packets == 0)
				printf("xf16cam audio: AMIC settled, live PCMU/8000 audio\n");
#ifdef XF16CAM_TALK
		} else if (xf16cam_talk_muting()) {
			memset(slot->pcmu, 0xff, sizeof(slot->pcmu));
#endif
		} else {
			for (i = 0; i < XF16CAM_AUDIO_SAMPLES_PER_PACKET; ++i) {
				uint16_t magnitude = pcm[i] == INT16_MIN ? 32768U :
				                     (uint16_t)(pcm[i] < 0 ? -pcm[i] : pcm[i]);
				if (magnitude > peak)
					peak = magnitude;
				total += magnitude;
				slot->pcmu[i] = xf16cam_mulaw(pcm[i]);
			}
		}
		slot->timestamp = packet * XF16CAM_AUDIO_SAMPLES_PER_PACKET;
		slot->generation = packet + 1;
		g_audio_info.peak = peak;
		g_audio_info.mean = (uint16_t)(total / XF16CAM_AUDIO_SAMPLES_PER_PACKET);
		g_audio_info.packets = packet + 1;
		g_audio_packets = packet + 1;
	}
}

int xf16cam_audio_start(void)
{
	memset(&g_audio_info, 0, sizeof(g_audio_info));
	memset(g_audio_ring, 0, sizeof(g_audio_ring));
	memset(g_audio_http_clients, 0, sizeof(g_audio_http_clients));
	g_audio_packets = 0;
	g_audio_users = 0;
	g_audio_http_clients_active = 0;
	g_audio_update_quiesced = 1;
	if (OS_MutexCreate(&g_audio_lock) != OS_OK)
		return -1;
	g_audio_lock_ready = 1;
	if (OS_ThreadCreate(&g_audio_thread, "xf16cam-audio", xf16cam_audio_task, NULL,
	                    OS_THREAD_PRIO_APP, XF16CAM_AUDIO_STACK_SIZE) != OS_OK) {
		OS_MutexDelete(&g_audio_lock);
		g_audio_lock_ready = 0;
		return -1;
	}
#ifdef XF16CAM_TALK
	/* A failure here leaves talk unavailable; capture is unaffected. */
	if (OS_ThreadCreate(&g_talk_thread, "xf16cam-talk", xf16cam_talk_task, NULL,
	                    OS_THREAD_PRIO_APP, XF16CAM_TALK_STACK_SIZE) != OS_OK)
		printf("xf16cam talk: task start failed\n");
#endif
	return 0;
}

__xip_text
int xf16cam_audio_acquire(void)
{
	int result = -1;

	if (!g_audio_lock_ready || OS_MutexLock(&g_audio_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (!xf16cam_update_active() && g_audio_info.available && g_audio_users < UINT32_MAX) {
		++g_audio_users;
		result = 0;
	}
	OS_MutexUnlock(&g_audio_lock);
	return result;
}

__xip_text
void xf16cam_audio_release(void)
{
	if (!g_audio_lock_ready || OS_MutexLock(&g_audio_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_audio_users > 0)
		--g_audio_users;
	OS_MutexUnlock(&g_audio_lock);
}

static void xf16cam_audio_http_task(void *arg)
{
	static const char header[] =
		"HTTP/1.1 200 OK\r\nContent-Type: audio/basic\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n";
	uint32_t slot = (uint32_t)(uintptr_t)arg;
	uint8_t pcmu[XF16CAM_AUDIO_SAMPLES_PER_PACKET];
	uint32_t cursor = xf16cam_audio_cursor();
	uint32_t timestamp;
	int fd = g_audio_http_clients[slot].fd;

	if (xf16cam_audio_send_all(fd, header, sizeof(header) - 1) == 0) {
		printf("xf16cam WEB audio: PCMU/8000 client connected\n");
		while (!xf16cam_update_active() && xf16cam_media_client_connected(fd)) {
			int ready = xf16cam_audio_read(&cursor, pcmu, &timestamp);
			(void)timestamp;
			if (ready > 0) {
				if (xf16cam_audio_send_all(fd, pcmu, sizeof(pcmu)) != 0)
					break;
			} else {
				OS_MSleep(5);
			}
		}
	}
	closesocket(fd);
	xf16cam_audio_release();
	printf("xf16cam WEB audio client stopped\n");
	OS_ThreadSetInvalid(&g_audio_http_clients[slot].thread);
	__sync_synchronize();
	g_audio_http_clients[slot].active = 0;
	if (g_audio_http_clients_active > 0)
		__sync_fetch_and_sub(&g_audio_http_clients_active, 1);
	OS_ThreadDelete(NULL);
}

int xf16cam_audio_http_start(int fd)
{
	int timeout = 2000;
	uint32_t slot;
	int found = 0;

	if (xf16cam_update_active() || !g_audio_info.available)
		return -1;
	for (slot = 0; slot < XF16CAM_MAX_PARALLEL_CLIENTS; ++slot) {
		if (__sync_bool_compare_and_swap(&g_audio_http_clients[slot].active, 0, 1)) {
			found = 1;
			break;
		}
	}
	if (!found || xf16cam_audio_acquire() != 0)
		goto fail_slot;

	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	g_audio_http_clients[slot].fd = fd;
	__sync_fetch_and_add(&g_audio_http_clients_active, 1);
	if (OS_ThreadCreate(&g_audio_http_clients[slot].thread, "xf16cam-web-audio",
	                    xf16cam_audio_http_task, (void *)(uintptr_t)slot,
	                    OS_THREAD_PRIO_APP, XF16CAM_AUDIO_HTTP_STACK) != OS_OK) {
		__sync_fetch_and_sub(&g_audio_http_clients_active, 1);
		xf16cam_audio_release();
		goto fail_slot;
	}
	return 0;

fail_slot:
	if (found) {
		__sync_synchronize();
		g_audio_http_clients[slot].active = 0;
	}
	return -1;
}

int xf16cam_audio_update_ready(void)
{
	int ready = g_audio_update_quiesced && g_audio_http_clients_active == 0;

#ifdef XF16CAM_TALK
	ready = ready && g_talk_quiesced;
#endif
	return ready;
}

const XF16CamAudioInfo *xf16cam_audio_info(void)
{
	return &g_audio_info;
}

uint32_t xf16cam_audio_cursor(void)
{
	return g_audio_packets;
}

int xf16cam_audio_read(uint32_t *cursor, uint8_t *pcmu, uint32_t *timestamp)
{
	uint32_t available = g_audio_packets;
	XF16CamAudioPacket *slot;
	uint32_t generation;

	if (*cursor == available)
		return 0;
	if (available - *cursor > XF16CAM_AUDIO_RING_PACKETS)
		*cursor = available - XF16CAM_AUDIO_RING_PACKETS;
	slot = &g_audio_ring[*cursor % XF16CAM_AUDIO_RING_PACKETS];
	generation = *cursor + 1;
	if (slot->generation != generation)
		return 0;
	memcpy(pcmu, slot->pcmu, XF16CAM_AUDIO_SAMPLES_PER_PACKET);
	*timestamp = slot->timestamp;
	if (slot->generation != generation)
		return 0;
	(*cursor)++;
	return 1;
}

uint32_t xf16cam_audio_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_audio_thread);
}

#ifdef XF16CAM_TALK
/* One talker at a time: the ring is single-producer, and two clients
 * interleaving packets would only produce noise. */
__xip_text
int xf16cam_talk_acquire(void)
{
	int result = -1;

	if (!g_audio_lock_ready || OS_MutexLock(&g_audio_lock, OS_WAIT_FOREVER) != OS_OK)
		return -1;
	if (!xf16cam_update_active() && g_talk_info.available && g_talk_users == 0) {
		g_talk_tail = g_talk_head;
		g_talk_users = 1;
		result = 0;
	}
	OS_MutexUnlock(&g_audio_lock);
	return result;
}

__xip_text
void xf16cam_talk_release(void)
{
	if (!g_audio_lock_ready || OS_MutexLock(&g_audio_lock, OS_WAIT_FOREVER) != OS_OK)
		return;
	if (g_talk_users > 0)
		--g_talk_users;
	OS_MutexUnlock(&g_audio_lock);
}

/* Called from the RTSP session thread with one RTP payload of PCMU. A payload
 * that does not fit is dropped whole rather than split. */
__xip_text
int xf16cam_talk_push(const uint8_t *pcmu, uint32_t length)
{
	uint32_t head = g_talk_head;
	uint32_t space = XF16CAM_TALK_RING_BYTES - (head - g_talk_tail);
	uint32_t i;

	if (g_talk_users == 0 || !g_talk_info.available)
		return -1;
	if (length == 0 || length > space) {
		g_talk_info.dropped++;
		return -1;
	}
	for (i = 0; i < length; ++i)
		g_talk_ring[(head + i) & (XF16CAM_TALK_RING_BYTES - 1U)] = pcmu[i];
	__sync_synchronize();
	g_talk_head = head + length;
	g_talk_last_ms = OS_TicksToMSecs(OS_GetTicks());
	g_talk_info.packets++;
	return 0;
}

__xip_text
const XF16CamTalkInfo *xf16cam_talk_info(void)
{
	return &g_talk_info;
}

__xip_text
uint32_t xf16cam_talk_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_talk_thread);
}
#endif
