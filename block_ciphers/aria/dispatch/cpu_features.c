#include "cpu_features.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct aria_x86_feature_cache {
  int aesni;
  int avx;
  int avx2;
  int gfni_avx512;
} aria_x86_feature_cache_t;

static aria_x86_feature_cache_t aria_cached_features;
/* 0: uninitialized, 1: initialization in progress, 2: ready. */
static atomic_int aria_feature_cache_state = ATOMIC_VAR_INIT(0);

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

static int aria_x86_get_features(int *aesni, int *avx, int *avx2, int *gfni_avx512)
{
  int have_aesni = 0;
  int have_avx = 0;
  int have_avx2 = 0;
  int have_gfni_avx512 = 0;

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int cpu_info[4] = {0, 0, 0, 0};
  unsigned __int64 xcr0 = 0;

  __cpuid(cpu_info, 1);
  have_aesni = (cpu_info[2] >> 25) & 1;
  if (((cpu_info[2] >> 27) & 1) == 0 || ((cpu_info[2] >> 28) & 1) == 0) {
    have_avx = 0;
    have_avx2 = 0;
  } else {
    xcr0 = _xgetbv(0);
    have_avx = ((xcr0 & 0x6) == 0x6) ? 1 : 0;
    if (have_avx) {
      __cpuidex(cpu_info, 7, 0);
      const int have_avx512f = (cpu_info[1] >> 16) & 1;
      const int have_avx512vl = (cpu_info[1] >> 31) & 1;
      const int have_gfni = (cpu_info[2] >> 8) & 1;
      have_avx2 = (cpu_info[1] >> 5) & 1;
      have_gfni_avx512 = have_avx2 && have_avx512f && have_avx512vl && have_gfni &&
                         ((xcr0 & 0xe6u) == 0xe6u);
    }
  }
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  unsigned int xcr0_lo = 0;
  unsigned int xcr0_hi = 0;
  unsigned int max_leaf = __get_cpuid_max(0, NULL);

  if (max_leaf >= 1 && __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    have_aesni = (int)((ecx >> 25) & 1u);
    if (((ecx >> 27) & 1u) != 0u && ((ecx >> 28) & 1u) != 0u) {
      __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0"
                           : "=a"(xcr0_lo), "=d"(xcr0_hi)
                           : "c"(0));
      if ((xcr0_lo & 0x6u) == 0x6u) {
        have_avx = 1;
      }
    }
  }

  if (have_avx && max_leaf >= 7 && __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    const uint64_t xcr0 = ((uint64_t)xcr0_hi << 32) | (uint64_t)xcr0_lo;
    const int have_avx512f = (int)((ebx >> 16) & 1u);
    const int have_avx512vl = (int)((ebx >> 31) & 1u);
    const int have_gfni = (int)((ecx >> 8) & 1u);
    have_avx2 = (int)((ebx >> 5) & 1u);
    have_gfni_avx512 = have_avx2 && have_avx512f && have_avx512vl && have_gfni &&
                       ((xcr0 & UINT64_C(0xe6)) == UINT64_C(0xe6));
  }
#endif

  if (aesni) {
    *aesni = have_aesni;
  }
  if (avx) {
    *avx = have_avx;
  }
  if (avx2) {
    *avx2 = have_avx2;
  }
  if (gfni_avx512) {
    *gfni_avx512 = have_gfni_avx512;
  }
  return (have_aesni || have_avx || have_avx2 || have_gfni_avx512) ? 1 : 0;
}

static const aria_x86_feature_cache_t *aria_cpu_features_cached(void)
{
  int state = atomic_load_explicit(&aria_feature_cache_state, memory_order_acquire);

  if (state != 2) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&aria_feature_cache_state,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
      aria_x86_get_features(&aria_cached_features.aesni,
                            &aria_cached_features.avx,
                            &aria_cached_features.avx2,
                            &aria_cached_features.gfni_avx512);
      atomic_store_explicit(&aria_feature_cache_state, 2, memory_order_release);
    } else {
      while (atomic_load_explicit(&aria_feature_cache_state,
                                  memory_order_acquire) != 2) {
        /* Feature detection is short and occurs only once per process. */
      }
    }
  }
  return &aria_cached_features;
}

int aria_cpu_has_aesni_avx(void)
{
  const aria_x86_feature_cache_t *features = aria_cpu_features_cached();
  return (features->aesni && features->avx) ? 1 : 0;
}

int aria_cpu_has_avx2(void)
{
  return aria_cpu_features_cached()->avx2 ? 1 : 0;
}

int aria_cpu_has_aesni_avx2(void)
{
  const aria_x86_feature_cache_t *features = aria_cpu_features_cached();
  return (features->aesni && features->avx && features->avx2) ? 1 : 0;
}

int aria_cpu_has_gfni_avx512(void)
{
  return aria_cpu_features_cached()->gfni_avx512 ? 1 : 0;
}
