#ifndef XF16CAM_ESPHOME_MBEDTLS_CONFIG_H
#define XF16CAM_ESPHOME_MBEDTLS_CONFIG_H

/*
 * mbedtls configuration for the ESPHome server. libmbedtls.a is built with
 * config-xr-mini-cliserv.h (src/net/mbedtls-2.16.0/Makefile), so any file
 * that touches mbedtls structures must see the same configuration or the
 * layouts of mbedtls_mpi and mbedtls_ecp_group differ from the library's.
 * The Noise transport cipher, ChaCha20-Poly1305, is not in that
 * configuration; xf16cam_esphome_mbedtls.c compiles it from the vendored
 * sources with the options switched on here, after the SDK configuration
 * has run its consistency check.
 */
#include "mbedtls/configs/config-xr-mini-cliserv.h"

#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#undef MBEDTLS_SELF_TEST

#endif
