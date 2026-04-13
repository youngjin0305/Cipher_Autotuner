#include "cpu_features.h"

#include <stddef.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

static int aria_x86_get_features(int *aesni, int *avx, int *avx2)
{
  int have_aesni = 0;
  int have_avx = 0;
  int have_avx2 = 0;

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
      have_avx2 = (cpu_info[1] >> 5) & 1;
    }
  }
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  unsigned int max_leaf = __get_cpuid_max(0, NULL);

  if (max_leaf >= 1 && __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    unsigned int xcr0_lo = 0;
    unsigned int xcr0_hi = 0;

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
    have_avx2 = (int)((ebx >> 5) & 1u);
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
  return (have_aesni || have_avx || have_avx2) ? 1 : 0;
}

int aria_cpu_has_aesni_avx(void)
{
  int aesni = 0;
  int avx = 0;
  int avx2 = 0;

  aria_x86_get_features(&aesni, &avx, &avx2);
  (void)avx2;
  return (aesni && avx) ? 1 : 0;
}

int aria_cpu_has_avx2(void)
{
  int aesni = 0;
  int avx = 0;
  int avx2 = 0;

  aria_x86_get_features(&aesni, &avx, &avx2);
  (void)aesni;
  return avx2 ? 1 : 0;
}

int aria_cpu_has_aesni_avx2(void)
{
  int aesni = 0;
  int avx = 0;
  int avx2 = 0;

  aria_x86_get_features(&aesni, &avx, &avx2);
  return (aesni && avx && avx2) ? 1 : 0;
}
