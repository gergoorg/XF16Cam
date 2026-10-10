/*
 * ChaCha20-Poly1305 for the ESPHome Noise transport, compiled from the SDK's
 * vendored mbedtls 2.16.0 sources because libmbedtls.a is built with a
 * configuration that leaves them out (see xf16cam_esphome_mbedtls_config.h).
 * The sources are included, not copied, so an SDK update carries them along.
 *
 * appos.ld places library archives in XIP by archive name, which does not
 * cover an object in the application, so every function the three files
 * define is declared here first with the XIP section attribute: GCC applies
 * a declaration's section attribute to the definition that follows. Only
 * chacha20_quarter_round (static inline, always inlined) and mul64 (a
 * one-line helper) are left to the compiler.
 */
#ifdef XF16CAM_ESPHOME

#define MBEDTLS_CONFIG_FILE "xf16cam_esphome_mbedtls_config.h"

#include "compiler.h"
#include "mbedtls/chacha20.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/poly1305.h"

/* chacha20.c */
static void chacha20_inner_block(uint32_t state[16]) __xip_text;
static void chacha20_block(const uint32_t initial_state[16],
                           unsigned char keystream[64]) __xip_text;
__xip_text void mbedtls_chacha20_init(mbedtls_chacha20_context *ctx);
__xip_text void mbedtls_chacha20_free(mbedtls_chacha20_context *ctx);
__xip_text int mbedtls_chacha20_setkey(mbedtls_chacha20_context *ctx,
                                       const unsigned char key[32]);
__xip_text int mbedtls_chacha20_starts(mbedtls_chacha20_context *ctx,
                                       const unsigned char nonce[12], uint32_t counter);
__xip_text int mbedtls_chacha20_update(mbedtls_chacha20_context *ctx, size_t size,
                                       const unsigned char *input, unsigned char *output);
__xip_text int mbedtls_chacha20_crypt(const unsigned char key[32],
                                      const unsigned char nonce[12], uint32_t counter,
                                      size_t size, const unsigned char *input,
                                      unsigned char *output);

/* poly1305.c */
static void poly1305_process(mbedtls_poly1305_context *ctx, size_t nblocks,
                             const unsigned char *input,
                             uint32_t needs_padding) __xip_text;
static void poly1305_compute_mac(const mbedtls_poly1305_context *ctx,
                                 unsigned char mac[16]) __xip_text;
__xip_text void mbedtls_poly1305_init(mbedtls_poly1305_context *ctx);
__xip_text void mbedtls_poly1305_free(mbedtls_poly1305_context *ctx);
__xip_text int mbedtls_poly1305_starts(mbedtls_poly1305_context *ctx,
                                       const unsigned char key[32]);
__xip_text int mbedtls_poly1305_update(mbedtls_poly1305_context *ctx,
                                       const unsigned char *input, size_t ilen);
__xip_text int mbedtls_poly1305_finish(mbedtls_poly1305_context *ctx,
                                       unsigned char mac[16]);
__xip_text int mbedtls_poly1305_mac(const unsigned char key[32],
                                    const unsigned char *input, size_t ilen,
                                    unsigned char mac[16]);

/* chachapoly.c */
static int chachapoly_pad_aad(mbedtls_chachapoly_context *ctx) __xip_text;
static int chachapoly_pad_ciphertext(mbedtls_chachapoly_context *ctx) __xip_text;
static int chachapoly_crypt_and_tag(mbedtls_chachapoly_context *ctx,
                                    mbedtls_chachapoly_mode_t mode, size_t length,
                                    const unsigned char nonce[12],
                                    const unsigned char *aad, size_t aad_len,
                                    const unsigned char *input, unsigned char *output,
                                    unsigned char tag[16]) __xip_text;
__xip_text void mbedtls_chachapoly_init(mbedtls_chachapoly_context *ctx);
__xip_text void mbedtls_chachapoly_free(mbedtls_chachapoly_context *ctx);
__xip_text int mbedtls_chachapoly_setkey(mbedtls_chachapoly_context *ctx,
                                         const unsigned char key[32]);
__xip_text int mbedtls_chachapoly_starts(mbedtls_chachapoly_context *ctx,
                                         const unsigned char nonce[12],
                                         mbedtls_chachapoly_mode_t mode);
__xip_text int mbedtls_chachapoly_update_aad(mbedtls_chachapoly_context *ctx,
                                             const unsigned char *aad, size_t aad_len);
__xip_text int mbedtls_chachapoly_update(mbedtls_chachapoly_context *ctx, size_t len,
                                         const unsigned char *input, unsigned char *output);
__xip_text int mbedtls_chachapoly_finish(mbedtls_chachapoly_context *ctx,
                                         unsigned char mac[16]);
__xip_text int mbedtls_chachapoly_encrypt_and_tag(mbedtls_chachapoly_context *ctx,
                                                  size_t length,
                                                  const unsigned char nonce[12],
                                                  const unsigned char *aad, size_t aad_len,
                                                  const unsigned char *input,
                                                  unsigned char *output,
                                                  unsigned char tag[16]);
__xip_text int mbedtls_chachapoly_auth_decrypt(mbedtls_chachapoly_context *ctx,
                                               size_t length,
                                               const unsigned char nonce[12],
                                               const unsigned char *aad, size_t aad_len,
                                               const unsigned char tag[16],
                                               const unsigned char *input,
                                               unsigned char *output);

#include "../../../src/net/mbedtls-2.16.0/library/chacha20.c"
#include "../../../src/net/mbedtls-2.16.0/library/poly1305.c"
#include "../../../src/net/mbedtls-2.16.0/library/chachapoly.c"

#endif /* XF16CAM_ESPHOME */
