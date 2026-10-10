/* Host test for the Noise_NNpsk0 responder: a mirrored initiator drives a
 * full handshake, both sides must agree on the transport keys, a wrong PSK
 * must be rejected, and transport frames must round-trip and detect
 * tampering. Needs a desktop mbedtls (Ubuntu: libmbedtls-dev).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -DXF16CAM_NOISE_INITIATOR \
 *     -Iinclude -Iproject/example/xf16cam tests/xf16cam/test_esphome_noise.c \
 *     project/example/xf16cam/xf16cam_esphome_noise.c -lmbedcrypto -o t && ./t
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

#include "xf16cam_esphome_noise.h"

static int host_rng(void *ctx, unsigned char *out, size_t len)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < len; ++i)
		out[i] = (unsigned char)rand();
	return 0;
}

static int host_random(uint8_t *out, size_t len)
{
	return host_rng(NULL, out, len);
}

static void reverse32(uint8_t out[32], const uint8_t in[32])
{
	size_t i;

	for (i = 0; i < 32; ++i)
		out[i] = in[31 - i];
}

/* X25519 over mbedtls ECP, the same way the device does it. */
static int host_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point)
{
	mbedtls_ecp_group grp;
	mbedtls_ecp_point P;
	mbedtls_ecp_point R;
	mbedtls_mpi m;
	uint8_t tmp[32];
	int rc;

	mbedtls_ecp_group_init(&grp);
	mbedtls_ecp_point_init(&P);
	mbedtls_ecp_point_init(&R);
	mbedtls_mpi_init(&m);
	reverse32(tmp, scalar);
	tmp[31] &= 248;
	tmp[0] &= 127;
	tmp[0] |= 64;
	rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) ||
	     mbedtls_mpi_read_binary(&m, tmp, sizeof(tmp));
	if (rc == 0) {
		if (point == NULL) {
			rc = mbedtls_ecp_copy(&P, &grp.G);
		} else {
			reverse32(tmp, point);
			tmp[0] &= 127;
			rc = mbedtls_mpi_read_binary(&P.X, tmp, sizeof(tmp)) ||
			     mbedtls_mpi_lset(&P.Z, 1);
		}
	}
	if (rc == 0)
		rc = mbedtls_ecp_mul(&grp, &R, &m, &P, host_rng, NULL);
	if (rc == 0)
		rc = mbedtls_mpi_write_binary(&R.X, tmp, sizeof(tmp));
	if (rc == 0)
		reverse32(out, tmp);
	mbedtls_mpi_free(&m);
	mbedtls_ecp_point_free(&R);
	mbedtls_ecp_point_free(&P);
	mbedtls_ecp_group_free(&grp);
	return rc == 0 ? 0 : -1;
}

static void host_sha256(const XF16CamNoiseSeg *segs, size_t count, uint8_t out[32])
{
	mbedtls_sha256_context ctx;
	size_t i;

	mbedtls_sha256_init(&ctx);
	mbedtls_sha256_starts_ret(&ctx, 0);
	for (i = 0; i < count; ++i)
		if (segs[i].len != 0)
			mbedtls_sha256_update_ret(&ctx, segs[i].data, segs[i].len);
	mbedtls_sha256_finish_ret(&ctx, out);
	mbedtls_sha256_free(&ctx);
}

static int host_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *ad, size_t ad_len,
                             const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
	mbedtls_chachapoly_context ctx;
	int rc;

	mbedtls_chachapoly_init(&ctx);
	rc = mbedtls_chachapoly_setkey(&ctx, key) ||
	     mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, ad, ad_len, in, out, tag);
	mbedtls_chachapoly_free(&ctx);
	return rc == 0 ? 0 : -1;
}

static int host_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *ad, size_t ad_len,
                             const uint8_t *in, size_t len, const uint8_t tag[16],
                             uint8_t *out)
{
	mbedtls_chachapoly_context ctx;
	int rc;

	mbedtls_chachapoly_init(&ctx);
	rc = mbedtls_chachapoly_setkey(&ctx, key) ||
	     mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce, ad, ad_len, tag, in, out);
	mbedtls_chachapoly_free(&ctx);
	return rc == 0 ? 0 : -1;
}

static const XF16CamNoiseCrypto g_crypto = {
	host_x25519, host_random, host_sha256, host_aead_encrypt, host_aead_decrypt,
};

static const uint8_t g_prologue[] = "NoiseAPIInit\0\0";	/* empty client hello */

static void test_handshake_and_transport(void)
{
	uint8_t psk[32];
	XF16CamNoise client;
	XF16CamNoise server;
	uint8_t msg1[XF16CAM_NOISE_MSG_LEN];
	uint8_t msg2[XF16CAM_NOISE_MSG_LEN];
	uint8_t plain[100];
	uint8_t frame[100 + XF16CAM_NOISE_TAG_LEN];
	uint8_t back[100];
	size_t i;

	for (i = 0; i < sizeof(psk); ++i)
		psk[i] = (uint8_t)(i * 7 + 1);
	xf16cam_noise_init(&client, &g_crypto, psk, g_prologue, sizeof(g_prologue) - 1);
	xf16cam_noise_init(&server, &g_crypto, psk, g_prologue, sizeof(g_prologue) - 1);
	assert(memcmp(client.h, server.h, 32) == 0);

	assert(xf16cam_noise_write_msg1(&client, msg1) == XF16CAM_NOISE_MSG_LEN);
	assert(xf16cam_noise_read_msg1(&server, msg1, sizeof(msg1)) == 0);
	assert(xf16cam_noise_write_msg2(&server, msg2) == XF16CAM_NOISE_MSG_LEN);
	assert(xf16cam_noise_read_msg2(&client, msg2, sizeof(msg2)) == 0);
	assert(client.ready && server.ready);
	assert(memcmp(client.k_send, server.k_recv, 32) == 0);
	assert(memcmp(client.k_recv, server.k_send, 32) == 0);
	assert(memcmp(client.k_send, client.k_recv, 32) != 0);
	/* handshake secrets are wiped */
	assert(server.has_k == 0);
	for (i = 0; i < 32; ++i)
		assert(server.e_priv[i] == 0 && server.psk[i] == 0);

	/* client -> server, including a zero-length message and in-place use */
	for (i = 0; i < sizeof(plain); ++i)
		plain[i] = (uint8_t)i;
	assert(xf16cam_noise_encrypt(&client, plain, sizeof(plain), frame) == 0);
	assert(memcmp(frame, plain, sizeof(plain)) != 0);
	assert(xf16cam_noise_decrypt(&server, frame, sizeof(frame), back) == 0);
	assert(memcmp(back, plain, sizeof(plain)) == 0);
	assert(xf16cam_noise_encrypt(&client, NULL, 0, frame) == 0);
	assert(xf16cam_noise_decrypt(&server, frame, XF16CAM_NOISE_TAG_LEN, back) == 0);
	memcpy(frame, plain, sizeof(plain));
	assert(xf16cam_noise_encrypt(&server, frame, sizeof(plain), frame) == 0);
	assert(xf16cam_noise_decrypt(&client, frame, sizeof(frame), frame) == 0);
	assert(memcmp(frame, plain, sizeof(plain)) == 0);
	assert(client.n_recv == 1 && server.n_recv == 2);

	/* a flipped bit fails authentication and does not advance the nonce */
	assert(xf16cam_noise_encrypt(&client, plain, sizeof(plain), frame) == 0);
	frame[10] ^= 1;
	assert(xf16cam_noise_decrypt(&server, frame, sizeof(frame), back) == -1);
	assert(server.n_recv == 2);
	frame[10] ^= 1;
	assert(xf16cam_noise_decrypt(&server, frame, sizeof(frame), back) == 0);
	assert(xf16cam_noise_decrypt(&server, frame, 5, back) == -1);	/* shorter than a tag */
}

static void test_wrong_psk(void)
{
	uint8_t psk[32];
	uint8_t other[32];
	XF16CamNoise client;
	XF16CamNoise server;
	uint8_t msg1[XF16CAM_NOISE_MSG_LEN];
	uint8_t msg2[XF16CAM_NOISE_MSG_LEN];

	memset(psk, 0x11, sizeof(psk));
	memcpy(other, psk, sizeof(other));
	other[31] ^= 1;
	xf16cam_noise_init(&client, &g_crypto, psk, g_prologue, sizeof(g_prologue) - 1);
	xf16cam_noise_init(&server, &g_crypto, other, g_prologue, sizeof(g_prologue) - 1);
	assert(xf16cam_noise_write_msg1(&client, msg1) == XF16CAM_NOISE_MSG_LEN);
	assert(xf16cam_noise_read_msg1(&server, msg1, sizeof(msg1)) == -1);
	assert(xf16cam_noise_write_msg2(&server, msg2) == -1);	/* never reaches split */
	assert(!server.ready);
	/* a wrong length is rejected before any crypto */
	xf16cam_noise_init(&server, &g_crypto, psk, g_prologue, sizeof(g_prologue) - 1);
	assert(xf16cam_noise_read_msg1(&server, msg1, sizeof(msg1) - 1) == -1);
	/* a different prologue (client hello bytes) also fails */
	xf16cam_noise_init(&server, &g_crypto, psk, g_prologue, sizeof(g_prologue) - 2);
	assert(xf16cam_noise_read_msg1(&server, msg1, sizeof(msg1)) == -1);
}

int main(void)
{
	srand(1234);
	test_handshake_and_transport();
	test_wrong_psk();
	printf("xf16cam ESPHome noise tests passed\n");
	return 0;
}
