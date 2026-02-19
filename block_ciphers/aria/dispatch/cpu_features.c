#include "cpu_features.h"

#if defined(_MSC_VER)
  #include <intrin.h>
#endif


// ---- CPU feature detection (AVX2) ----
// 1) CPUID.(EAX=1): OSXSAVE(bit 27), AVX(bit 28)
// 2) XGETBV(XCR0): XMM(bit1) + YMM(bit2) enabled
// 3) CPUID.(EAX=7, ECX=0): AVX2(bit 5 of EBX)
int aria_cpu_has_avx2(void) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int cpuInfo[4] = {0, 0, 0, 0};
  __cpuid(cpuInfo, 1);

  const int ecx = cpuInfo[2];
  const int osxsave = (ecx >> 27) & 1;
  const int avx     = (ecx >> 28) & 1;
  if (!osxsave || !avx) return 0;

  // XGETBV(0): XCR0
  unsigned __int64 xcr0 = _xgetbv(0);
  const int xmm_enabled = ((xcr0 & 0x2) != 0);
  const int ymm_enabled = ((xcr0 & 0x4) != 0);
  if (!xmm_enabled || !ymm_enabled) return 0;

  __cpuidex(cpuInfo, 7, 0);
  const int ebx = cpuInfo[1];
  const int avx2 = (ebx >> 5) & 1;
  return avx2 ? 1 : 0;
#else
  return 0;
#endif
}