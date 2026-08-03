#ifndef ARIA_LINUX_AVX_H
#define ARIA_LINUX_AVX_H

#include <stddef.h>
#include <stdint.h>

#include "aria_api.h"

#define ARIA_AESNI_PARALLEL_BLOCKS 16u
#define ARIA_AESNI_PARALLEL_BLOCK_SIZE  (ARIA_BLOCK_SIZE * ARIA_AESNI_PARALLEL_BLOCKS)

#define ARIA_AESNI_AVX2_PARALLEL_BLOCKS 32u
#define ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE  (ARIA_BLOCK_SIZE * ARIA_AESNI_AVX2_PARALLEL_BLOCKS)

#define ARIA_GFNI_AVX512_PARALLEL_BLOCKS 64u
#define ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE  (ARIA_BLOCK_SIZE * ARIA_GFNI_AVX512_PARALLEL_BLOCKS)

void aria_aesni_avx_encrypt_16way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_aesni_avx_decrypt_16way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_aesni_avx_ctr_crypt_16way(const void *ctx, uint8_t *dst, const uint8_t *src,
                                    uint8_t *keystream, uint8_t *iv);
void aria_aesni_avx_gfni_encrypt_16way(const void *ctx, uint8_t *dst, const uint8_t *src);

void aria_aesni_avx2_encrypt_32way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_aesni_avx2_decrypt_32way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_aesni_avx2_ctr_crypt_32way(const void *ctx, uint8_t *dst, const uint8_t *src,
                                     uint8_t *keystream, uint8_t *iv);
void aria_aesni_avx2_gfni_encrypt_32way(const void *ctx, uint8_t *dst, const uint8_t *src);

void aria_gfni_avx512_encrypt_64way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_gfni_avx512_decrypt_64way(const void *ctx, uint8_t *dst, const uint8_t *src);
void aria_gfni_avx512_ctr_crypt_64way(const void *ctx, uint8_t *dst, const uint8_t *src,
                                      uint8_t *keystream, uint8_t *iv);

void aria_linux_aesni_init(aria_ctx_t *ctx, const uint8_t *key, int keybits);

#endif
