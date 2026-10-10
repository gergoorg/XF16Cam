#ifndef XF16CAM_ESPHOME_NOISE_H
#define XF16CAM_ESPHOME_NOISE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Noise_NNpsk0_25519_ChaChaPoly_SHA256, the handshake and transport the
 * ESPHome native API encrypts with. Written from the Noise specification
 * (revision 34) and the ESPHome frame layout, not ported from ESPHome.
 *
 * The primitives come through a vtable so this file has no SDK dependency:
 * the device fills it from mbedtls (ECP Curve25519, ChaCha20-Poly1305,
 * SHA-256) and the TRNG, the host test from a desktop mbedtls. The responder
 * side is what the camera runs; the initiator side exists for the host test
 * and is compiled only with XF16CAM_NOISE_INITIATOR.
 *
 * Wire format around this module (handled by the caller):
 *   frame        0x01, u16be length, body
 *   client hello body: empty (its bytes still go into the prologue)
 *   server hello body: 0x01, node name, 0, MAC "aa:bb:cc:dd:ee:ff", 0
 *   handshake body:    0x00, 48 bytes (ephemeral key + empty payload tag)
 *   reject body:       0x01, reason text ("Handshake MAC failure")
 *   data body:         ciphertext of (u16be type, u16be length, payload)
 *                      plus the 16-byte tag
 */

typedef struct {
	const uint8_t *data;
	size_t len;
} XF16CamNoiseSeg;

typedef struct {
	/* out = scalar * point, point NULL for the base point. 0 on success. */
	int (*x25519)(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point);
	int (*random)(uint8_t *out, size_t len);
	/* SHA-256 over the concatenation of up to three segments. */
	void (*sha256)(const XF16CamNoiseSeg *segs, size_t count, uint8_t out[32]);
	/* ChaCha20-Poly1305 with a 12-byte nonce. in and out may be the same. */
	int (*aead_encrypt)(const uint8_t key[32], const uint8_t nonce[12],
	                    const uint8_t *ad, size_t ad_len,
	                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
	int (*aead_decrypt)(const uint8_t key[32], const uint8_t nonce[12],
	                    const uint8_t *ad, size_t ad_len,
	                    const uint8_t *in, size_t len, const uint8_t tag[16],
	                    uint8_t *out);
} XF16CamNoiseCrypto;

typedef struct {
	const XF16CamNoiseCrypto *crypto;
	uint8_t h[32];
	uint8_t ck[32];
	uint8_t k[32];
	uint8_t psk[32];
	uint8_t e_priv[32];
	uint8_t re[32];
	uint8_t k_recv[32];
	uint8_t k_send[32];
	uint64_t n;
	uint64_t n_recv;
	uint64_t n_send;
	uint8_t has_k;
	uint8_t ready;
} XF16CamNoise;

#define XF16CAM_NOISE_KEY_LEN   (32)
#define XF16CAM_NOISE_TAG_LEN   (16)
#define XF16CAM_NOISE_MSG_LEN   (48)	/* ephemeral key + empty payload tag */
#define XF16CAM_NOISE_PROLOGUE  "NoiseAPIInit"

void xf16cam_noise_init(XF16CamNoise *n, const XF16CamNoiseCrypto *crypto,
                        const uint8_t psk[XF16CAM_NOISE_KEY_LEN],
                        const uint8_t *prologue, size_t prologue_len);

/* Responder. read_msg1 returns -1 for a wrong key or a malformed message;
 * write_msg2 returns -1 when the RNG or DH fails. After write_msg2 the
 * transport keys are ready. */
int xf16cam_noise_read_msg1(XF16CamNoise *n, const uint8_t *msg, size_t len);
int xf16cam_noise_write_msg2(XF16CamNoise *n, uint8_t out[XF16CAM_NOISE_MSG_LEN]);

#ifdef XF16CAM_NOISE_INITIATOR
int xf16cam_noise_write_msg1(XF16CamNoise *n, uint8_t out[XF16CAM_NOISE_MSG_LEN]);
int xf16cam_noise_read_msg2(XF16CamNoise *n, const uint8_t *msg, size_t len);
#endif

/* Transport. encrypt writes len + 16 bytes; decrypt takes len including the
 * tag and writes len - 16 bytes. in and out may be the same buffer. */
int xf16cam_noise_encrypt(XF16CamNoise *n, const uint8_t *in, size_t len, uint8_t *out);
int xf16cam_noise_decrypt(XF16CamNoise *n, const uint8_t *in, size_t len, uint8_t *out);

#endif
