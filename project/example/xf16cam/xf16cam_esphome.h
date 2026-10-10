#ifndef XF16CAM_ESPHOME_H
#define XF16CAM_ESPHOME_H

#include <stddef.h>
#include <stdint.h>

/*
 * ESPHome native API server for Home Assistant (XF16CAM_ESPHOME builds).
 * Listens on TCP 6053, speaks the ESPHome protobuf protocol in plaintext or
 * under Noise encryption, lists the same entity set the MQTT discovery
 * document publishes, pushes their states, takes light and button commands
 * and serves JPEG frames on request. Runs only in station mode.
 */

typedef struct {
	uint32_t connects;
	uint32_t disconnects;
	uint32_t frames;	/* camera images sent */
	uint32_t states;	/* state pushes (all entities) */
	uint32_t commands;
	uint32_t dropped;	/* inbound messages too large for the buffer */
	uint32_t rejected;	/* handshakes with a wrong key */
	const char *state;	/* human-readable task state, never NULL */
	uint8_t enabled;	/* task running (STA mode) */
	uint8_t clients;	/* connections past the handshake */
	uint8_t encrypted;	/* any client under Noise */
} XF16CamEsphomeInfo;

#define XF16CAM_ESPHOME_PORT        (6053)
#define XF16CAM_ESPHOME_KEY_B64_LEN (45)	/* 32 bytes base64 + NUL */

#ifdef XF16CAM_ESPHOME
int xf16cam_esphome_start(void);
const XF16CamEsphomeInfo *xf16cam_esphome_info(void);
uint32_t xf16cam_esphome_stack_min_free(void);
/* Generates, stores and returns (base64) a fresh Noise key. Existing
 * connections keep running; new ones must use the key. */
int xf16cam_esphome_generate_key(char *b64, size_t size);
int xf16cam_esphome_clear_key(void);
#else
static inline int xf16cam_esphome_start(void)
{
	return 0;
}
#endif

#endif
