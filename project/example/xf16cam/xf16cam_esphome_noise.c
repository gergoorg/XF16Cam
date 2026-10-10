/*
 * Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder (and, for the host test,
 * initiator). See xf16cam_esphome_noise.h for the wire layout and the
 * division of labour with the caller.
 *
 * Symbols from the Noise specification: h is the handshake hash, ck the
 * chaining key, k the current cipher key, n its nonce. HKDF here is the
 * Noise HKDF (HMAC chain with 0x01, 0x02, 0x03 counters), not RFC 5869.
 */
#include <string.h>

#include "xf16cam_esphome_noise.h"

#include "compiler.h"

__xip_rodata static const char g_protocol_name[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

__xip_text
static void noise_hmac(const XF16CamNoiseCrypto *c, const uint8_t key[32],
                       const uint8_t *d1, size_t l1, const uint8_t *d2, size_t l2,
                       uint8_t out[32])
{
	uint8_t pad[64];
	uint8_t inner[32];
	XF16CamNoiseSeg segs[3];
	size_t i;

	for (i = 0; i < sizeof(pad); ++i)
		pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x36);
	segs[0].data = pad;
	segs[0].len = sizeof(pad);
	segs[1].data = d1;
	segs[1].len = l1;
	segs[2].data = d2;
	segs[2].len = l2;
	c->sha256(segs, 3, inner);
	for (i = 0; i < sizeof(pad); ++i)
		pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x5c);
	segs[1].data = inner;
	segs[1].len = sizeof(inner);
	c->sha256(segs, 2, out);
	memset(pad, 0, sizeof(pad));
	memset(inner, 0, sizeof(inner));
}

/* out3 may be NULL. out1 may alias n->ck (the usual MixKey form). */
__xip_text
static void noise_hkdf(const XF16CamNoiseCrypto *c, const uint8_t ck[32],
                       const uint8_t *input, size_t input_len,
                       uint8_t out1[32], uint8_t out2[32], uint8_t *out3)
{
	uint8_t temp[32];
	uint8_t counter;

	noise_hmac(c, ck, input, input_len, NULL, 0, temp);
	counter = 1;
	noise_hmac(c, temp, &counter, 1, NULL, 0, out1);
	counter = 2;
	noise_hmac(c, temp, out1, 32, &counter, 1, out2);
	if (out3 != NULL) {
		counter = 3;
		noise_hmac(c, temp, out2, 32, &counter, 1, out3);
	}
	memset(temp, 0, sizeof(temp));
}

__xip_text
static void noise_mix_hash(XF16CamNoise *n, const uint8_t *data, size_t len)
{
	XF16CamNoiseSeg segs[2];

	segs[0].data = n->h;
	segs[0].len = sizeof(n->h);
	segs[1].data = data;
	segs[1].len = len;
	n->crypto->sha256(segs, 2, n->h);
}

__xip_text
static void noise_mix_key(XF16CamNoise *n, const uint8_t *input, size_t len)
{
	noise_hkdf(n->crypto, n->ck, input, len, n->ck, n->k, NULL);
	n->has_k = 1;
	n->n = 0;
}

__xip_text
static void noise_mix_key_and_hash(XF16CamNoise *n, const uint8_t *input, size_t len)
{
	uint8_t temp_h[32];

	noise_hkdf(n->crypto, n->ck, input, len, n->ck, temp_h, n->k);
	noise_mix_hash(n, temp_h, sizeof(temp_h));
	n->has_k = 1;
	n->n = 0;
}

__xip_text
static void noise_nonce(uint8_t out[12], uint64_t n)
{
	size_t i;

	memset(out, 0, 4);
	for (i = 0; i < 8; ++i)
		out[4 + i] = (uint8_t)(n >> (8 * i));
}

/* Handshake payloads are always empty here, so these only produce or check
 * the tag over the current h. */
__xip_text
static int noise_encrypt_and_hash_empty(XF16CamNoise *n, uint8_t tag[16])
{
	uint8_t nonce[12];

	noise_nonce(nonce, n->n);
	if (n->crypto->aead_encrypt(n->k, nonce, n->h, sizeof(n->h), NULL, 0, NULL, tag) != 0)
		return -1;
	++n->n;
	noise_mix_hash(n, tag, 16);
	return 0;
}

__xip_text
static int noise_decrypt_and_hash_empty(XF16CamNoise *n, const uint8_t tag[16])
{
	uint8_t nonce[12];

	noise_nonce(nonce, n->n);
	if (n->crypto->aead_decrypt(n->k, nonce, n->h, sizeof(n->h), NULL, 0, tag, NULL) != 0)
		return -1;
	++n->n;
	noise_mix_hash(n, tag, 16);
	return 0;
}

__xip_text
static void noise_wipe_handshake(XF16CamNoise *n)
{
	memset(n->h, 0, sizeof(n->h));
	memset(n->ck, 0, sizeof(n->ck));
	memset(n->k, 0, sizeof(n->k));
	memset(n->psk, 0, sizeof(n->psk));
	memset(n->e_priv, 0, sizeof(n->e_priv));
	memset(n->re, 0, sizeof(n->re));
	n->has_k = 0;
}

__xip_text
void xf16cam_noise_init(XF16CamNoise *n, const XF16CamNoiseCrypto *crypto,
                        const uint8_t psk[XF16CAM_NOISE_KEY_LEN],
                        const uint8_t *prologue, size_t prologue_len)
{
	XF16CamNoiseSeg seg;

	memset(n, 0, sizeof(*n));
	n->crypto = crypto;
	/* The protocol name is longer than 32 bytes, so h starts as its hash. */
	seg.data = (const uint8_t *)g_protocol_name;
	seg.len = sizeof(g_protocol_name) - 1;
	crypto->sha256(&seg, 1, n->h);
	memcpy(n->ck, n->h, sizeof(n->ck));
	memcpy(n->psk, psk, sizeof(n->psk));
	noise_mix_hash(n, prologue, prologue_len);
}

/* Message 1: "-> psk, e" with an empty payload. */
__xip_text
int xf16cam_noise_read_msg1(XF16CamNoise *n, const uint8_t *msg, size_t len)
{
	if (len != XF16CAM_NOISE_MSG_LEN || n->ready)
		return -1;
	noise_mix_key_and_hash(n, n->psk, sizeof(n->psk));
	memcpy(n->re, msg, 32);
	noise_mix_hash(n, n->re, 32);
	noise_mix_key(n, n->re, 32);	/* the psk modifier adds this MixKey */
	if (noise_decrypt_and_hash_empty(n, msg + 32) != 0) {
		/* A wrong key: forget everything so message 2 cannot follow. */
		noise_wipe_handshake(n);
		return -1;
	}
	return 0;
}

/* Message 2: "<- e, ee" with an empty payload, then Split. */
__xip_text
int xf16cam_noise_write_msg2(XF16CamNoise *n, uint8_t out[XF16CAM_NOISE_MSG_LEN])
{
	uint8_t dh[32];

	if (!n->has_k || n->ready)
		return -1;
	if (n->crypto->random(n->e_priv, sizeof(n->e_priv)) != 0 ||
	    n->crypto->x25519(out, n->e_priv, NULL) != 0)
		return -1;
	noise_mix_hash(n, out, 32);
	noise_mix_key(n, out, 32);
	if (n->crypto->x25519(dh, n->e_priv, n->re) != 0)
		return -1;
	noise_mix_key(n, dh, sizeof(dh));
	memset(dh, 0, sizeof(dh));
	if (noise_encrypt_and_hash_empty(n, out + 32) != 0)
		return -1;
	/* Split: the initiator sends with the first key, so we receive with it. */
	noise_hkdf(n->crypto, n->ck, NULL, 0, n->k_recv, n->k_send, NULL);
	noise_wipe_handshake(n);
	n->ready = 1;
	return XF16CAM_NOISE_MSG_LEN;
}

#ifdef XF16CAM_NOISE_INITIATOR
__xip_text
int xf16cam_noise_write_msg1(XF16CamNoise *n, uint8_t out[XF16CAM_NOISE_MSG_LEN])
{
	if (n->ready)
		return -1;
	noise_mix_key_and_hash(n, n->psk, sizeof(n->psk));
	if (n->crypto->random(n->e_priv, sizeof(n->e_priv)) != 0 ||
	    n->crypto->x25519(out, n->e_priv, NULL) != 0)
		return -1;
	noise_mix_hash(n, out, 32);
	noise_mix_key(n, out, 32);
	if (noise_encrypt_and_hash_empty(n, out + 32) != 0)
		return -1;
	return XF16CAM_NOISE_MSG_LEN;
}

__xip_text
int xf16cam_noise_read_msg2(XF16CamNoise *n, const uint8_t *msg, size_t len)
{
	uint8_t dh[32];

	if (len != XF16CAM_NOISE_MSG_LEN || !n->has_k || n->ready)
		return -1;
	memcpy(n->re, msg, 32);
	noise_mix_hash(n, n->re, 32);
	noise_mix_key(n, n->re, 32);
	if (n->crypto->x25519(dh, n->e_priv, n->re) != 0)
		return -1;
	noise_mix_key(n, dh, sizeof(dh));
	memset(dh, 0, sizeof(dh));
	if (noise_decrypt_and_hash_empty(n, msg + 32) != 0)
		return -1;
	noise_hkdf(n->crypto, n->ck, NULL, 0, n->k_send, n->k_recv, NULL);
	noise_wipe_handshake(n);
	n->ready = 1;
	return 0;
}
#endif

__xip_text
int xf16cam_noise_encrypt(XF16CamNoise *n, const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t nonce[12];

	if (!n->ready)
		return -1;
	noise_nonce(nonce, n->n_send);
	if (n->crypto->aead_encrypt(n->k_send, nonce, NULL, 0, in, len, out,
	                            out + len) != 0)
		return -1;
	++n->n_send;
	return 0;
}

__xip_text
int xf16cam_noise_decrypt(XF16CamNoise *n, const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t nonce[12];
	uint8_t tag[16];

	if (!n->ready || len < XF16CAM_NOISE_TAG_LEN)
		return -1;
	/* The tag is copied out first so in and out may overlap. */
	memcpy(tag, in + len - XF16CAM_NOISE_TAG_LEN, sizeof(tag));
	noise_nonce(nonce, n->n_recv);
	if (n->crypto->aead_decrypt(n->k_recv, nonce, NULL, 0, in,
	                            len - XF16CAM_NOISE_TAG_LEN, tag, out) != 0)
		return -1;
	++n->n_recv;
	return 0;
}
