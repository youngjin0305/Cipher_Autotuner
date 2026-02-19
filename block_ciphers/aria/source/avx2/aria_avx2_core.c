#include "aria_avx2_core.h"

#include <string.h>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

extern const unsigned char S[4][256];

static inline __m256i load2(const uint8_t* b0, const uint8_t* b1) {
  __m128i a = _mm_loadu_si128((const __m128i*)b0);
  __m128i b = _mm_loadu_si128((const __m128i*)b1);
  return _mm256_set_m128i(b, a);
}

static inline void store2(uint8_t* b0, uint8_t* b1, __m256i v) {
  __m128i a = _mm256_castsi256_si128(v);
  __m128i b = _mm256_extracti128_si256(v, 1);
  _mm_storeu_si128((__m128i*)b0, a);
  _mm_storeu_si128((__m128i*)b1, b);
}

static inline __m256i xor_rk_2way(__m256i st2, const uint8_t rk16[16]) {
  __m128i r = _mm_loadu_si128((const __m128i*)rk16);
  __m256i rr = _mm256_broadcastsi128_si256(r);
  return _mm256_xor_si256(st2, rr);
}

static inline __m256i lut256_32B(__m256i x, const uint8_t tab[256]) {
  const __m256i lo_mask = _mm256_set1_epi8(0x0F);

  __m256i lo = _mm256_and_si256(x, lo_mask);
  __m256i hi = _mm256_and_si256(_mm256_srli_epi16(x, 4), lo_mask);

  __m256i acc = _mm256_setzero_si256();
  for (int i = 0; i < 16; i++) {
    __m128i t128 = _mm_loadu_si128((const __m128i*)(tab + 16*i));
    __m256i t = _mm256_broadcastsi128_si256(t128);
    __m256i y = _mm256_shuffle_epi8(t, lo);

    __m256i m = _mm256_cmpeq_epi8(hi, _mm256_set1_epi8((char)i));
    acc = _mm256_xor_si256(acc, _mm256_and_si256(y, m));
  }
  return acc;
}

static inline __m256i sl_2way(__m256i x, int offset_mod4) {
  const uint8_t* T0 = S[(offset_mod4 + 0) & 3];
  const uint8_t* T1 = S[(offset_mod4 + 1) & 3];
  const uint8_t* T2 = S[(offset_mod4 + 2) & 3];
  const uint8_t* T3 = S[(offset_mod4 + 3) & 3];

  const __m256i idx = _mm256_setr_epi8(
      0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
      0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  );
  __m256i mod = _mm256_and_si256(idx, _mm256_set1_epi8(3));

  __m256i y0 = lut256_32B(x, T0);
  __m256i y1 = lut256_32B(x, T1);
  __m256i y2 = lut256_32B(x, T2);
  __m256i y3 = lut256_32B(x, T3);

  __m256i m0 = _mm256_cmpeq_epi8(mod, _mm256_set1_epi8(0));
  __m256i m1 = _mm256_cmpeq_epi8(mod, _mm256_set1_epi8(1));
  __m256i m2 = _mm256_cmpeq_epi8(mod, _mm256_set1_epi8(2));
  __m256i m3 = _mm256_cmpeq_epi8(mod, _mm256_set1_epi8(3));

  __m256i out = _mm256_setzero_si256();
  out = _mm256_xor_si256(out, _mm256_and_si256(y0, m0));
  out = _mm256_xor_si256(out, _mm256_and_si256(y1, m1));
  out = _mm256_xor_si256(out, _mm256_and_si256(y2, m2));
  out = _mm256_xor_si256(out, _mm256_and_si256(y3, m3));
  return out;
}

static inline __m256i dl_avx2_2way(__m256i in2) {
  const __m128i mA = _mm_setr_epi8(
      6, 7, 4, 5, 0, 1, 2, 3, 1, 0, 3, 2, 2, 3, 0, 1
  );
  const __m128i mB = _mm_setr_epi8(
      8, 9,10,11,11,10, 9, 8, 4, 5, 6, 7, 7, 6, 5, 4
  );
  const __m128i mC = _mm_setr_epi8(
     13,12,15,14,14,15,12,13,15,14,13,12, 9, 8,11,10
  );

  __m256i MA = _mm256_broadcastsi128_si256(mA);
  __m256i MB = _mm256_broadcastsi128_si256(mB);
  __m256i MC = _mm256_broadcastsi128_si256(mC);

  __m256i base = _mm256_shuffle_epi8(in2, MA);
  base = _mm256_xor_si256(base, _mm256_shuffle_epi8(in2, MB));
  base = _mm256_xor_si256(base, _mm256_shuffle_epi8(in2, MC));

  // T0 -> pos {0,5,11,14} with bytes (3,4,9,14)
  // T1 -> pos {1,4,10,15} with bytes (2,5,8,15)
  // T2 -> pos {2,7,9,12}  with bytes (1,6,11,12)
  // T3 -> pos {3,6,8,13}  with bytes (0,7,10,13)

#define Z ((char)0x80)

  // ---- T0 sparse masks ----
  const __m128i t0_3  = _mm_setr_epi8( 3,Z,Z,Z,Z, 3,Z,Z,Z,Z,Z, 3,Z,Z, 3,Z);
  const __m128i t0_4  = _mm_setr_epi8( 4,Z,Z,Z,Z, 4,Z,Z,Z,Z,Z, 4,Z,Z, 4,Z);
  const __m128i t0_9  = _mm_setr_epi8( 9,Z,Z,Z,Z, 9,Z,Z,Z,Z,Z, 9,Z,Z, 9,Z);
  const __m128i t0_14 = _mm_setr_epi8(14,Z,Z,Z,Z,14,Z,Z,Z,Z,Z,14,Z,Z,14,Z);

  // ---- T1 sparse masks ----
  const __m128i t1_2  = _mm_setr_epi8( Z, 2,Z,Z, 2,Z,Z,Z,Z,Z, 2,Z,Z,Z,Z, 2);
  const __m128i t1_5  = _mm_setr_epi8( Z, 5,Z,Z, 5,Z,Z,Z,Z,Z, 5,Z,Z,Z,Z, 5);
  const __m128i t1_8  = _mm_setr_epi8( Z, 8,Z,Z, 8,Z,Z,Z,Z,Z, 8,Z,Z,Z,Z, 8);
  const __m128i t1_15 = _mm_setr_epi8( Z,15,Z,Z,15,Z,Z,Z,Z,Z,15,Z,Z,Z,Z,15);

  // ---- T2 sparse masks ----
  const __m128i t2_1  = _mm_setr_epi8( Z,Z, 1,Z,Z,Z,Z, 1,Z, 1,Z,Z, 1,Z,Z,Z);
  const __m128i t2_6  = _mm_setr_epi8( Z,Z, 6,Z,Z,Z,Z, 6,Z, 6,Z,Z, 6,Z,Z,Z);
  const __m128i t2_11 = _mm_setr_epi8( Z,Z,11,Z,Z,Z,Z,11,Z,11,Z,Z,11,Z,Z,Z);
  const __m128i t2_12 = _mm_setr_epi8( Z,Z,12,Z,Z,Z,Z,12,Z,12,Z,Z,12,Z,Z,Z);

  // ---- T3 sparse masks ----
  const __m128i t3_0  = _mm_setr_epi8( Z,Z,Z, 0,Z,Z, 0,Z, 0,Z,Z,Z,Z, 0,Z,Z);
  const __m128i t3_7  = _mm_setr_epi8( Z,Z,Z, 7,Z,Z, 7,Z, 7,Z,Z,Z,Z, 7,Z,Z);
  const __m128i t3_10 = _mm_setr_epi8( Z,Z,Z,10,Z,Z,10,Z,10,Z,Z,Z,Z,10,Z,Z);
  const __m128i t3_13 = _mm_setr_epi8( Z,Z,Z,13,Z,Z,13,Z,13,Z,Z,Z,Z,13,Z,Z);

  __m256i T = _mm256_setzero_si256();

  // T0
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t0_3)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t0_4)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t0_9)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t0_14)));

  // T1
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t1_2)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t1_5)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t1_8)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t1_15)));

  // T2
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t2_1)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t2_6)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t2_11)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t2_12)));

  // T3
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t3_0)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t3_7)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t3_10)));
  T = _mm256_xor_si256(T, _mm256_shuffle_epi8(in2, _mm256_broadcastsi128_si256(t3_13)));

#undef Z

  return _mm256_xor_si256(base, T);
}

void aria_avx2_encrypt_4way(const aria_ctx_t *ctx, const uint8_t in[64], uint8_t out[64]) {
  if (!ctx || !in || !out) return;

  __m256i s01 = load2(in +  0, in + 16);
  __m256i s23 = load2(in + 32, in + 48);

  const uint8_t* e = (const uint8_t*)ctx->rk; // round key bytes
  const int R = ctx->rounds;

  for (int i = 0; i < (R / 2); i++) {
    // --- round A: t = S[j%4][ e ^ c ], DL(t,c), e+=16
    s01 = xor_rk_2way(s01, e);
    s01 = sl_2way(s01, 0);
    s01 = dl_avx2_2way(s01);

    s23 = xor_rk_2way(s23, e);
    s23 = sl_2way(s23, 0);
    s23 = dl_avx2_2way(s23);

    e += 16;

    // --- round B: t = S[(2+j)%4][ e ^ c ], DL(t,c), e+=16
    s01 = xor_rk_2way(s01, e);
    s01 = sl_2way(s01, 2);
    s01 = dl_avx2_2way(s01);

    s23 = xor_rk_2way(s23, e);
    s23 = sl_2way(s23, 2);
    s23 = dl_avx2_2way(s23);

    e += 16;
  }

  // DL(c, t);  c = e ^ t;
  s01 = dl_avx2_2way(s01);
  s23 = dl_avx2_2way(s23);

  s01 = xor_rk_2way(s01, e);
  s23 = xor_rk_2way(s23, e);

  store2(out +  0, out + 16, s01);
  store2(out + 32, out + 48, s23);
}
