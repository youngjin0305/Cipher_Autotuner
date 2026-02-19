#ifndef ARIA_AVX2_CORE_H
#define ARIA_AVX2_CORE_H

#include <stdint.h>
#include "aria_api.h"

#if defined(_MSC_VER)
  #include <intrin.h>
#endif
#include <immintrin.h>

void aria_avx2_encrypt_4way(const aria_ctx_t *ctx, const uint8_t in[64], uint8_t out[64]);

#endif
