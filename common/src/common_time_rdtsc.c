#include "common_time.h"

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(__rdtsc)

static __forceinline uint64_t rdtsc_now(void) {
  return __rdtsc();
}
#elif defined(__i386__) || defined(__x86_64__)
static inline uint64_t rdtsc_now(void) {
  unsigned int lo = 0;
  unsigned int hi = 0;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | (uint64_t)lo;
}
#else
#error "RDTSC is not supported on this architecture."
#endif

uint64_t time_begin(void) {
  return rdtsc_now();
}

uint64_t time_end(uint64_t start) {
  uint64_t end = rdtsc_now();
  return end - start;
}
