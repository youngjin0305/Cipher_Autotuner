#include "common_time.h"

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>

static uint64_t monotonic_now_ns(void) {
  struct timespec ts;
#if defined(__linux__) && defined(CLOCK_MONOTONIC_RAW)
  const clockid_t clock_id = CLOCK_MONOTONIC_RAW;
#else
  const clockid_t clock_id = CLOCK_MONOTONIC;
#endif

  if (clock_gettime(clock_id, &ts) != 0) {
    return 0;
  }

  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

uint64_t time_begin(void) {
  return monotonic_now_ns();
}

uint64_t time_end(uint64_t start) {
  uint64_t end = monotonic_now_ns();
  return (end > start) ? (end - start) : 0;
}

uint64_t time_frequency(void) {
  return 1000000000ull;
}
#else
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

uint64_t time_frequency(void) {
  return 0;
}
#endif
