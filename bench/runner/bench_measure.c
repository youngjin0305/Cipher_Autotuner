#include "bench_measure.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include "common_time.h"
#endif

static uint64_t qpc_now(void) {
#if defined(_WIN32)
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (uint64_t)counter.QuadPart;
#else
  return time_begin();
#endif
}

static uint64_t qpc_freq(void) {
#if defined(_WIN32)
  static uint64_t freq = 0;
  if (freq == 0) {
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&counter);
    freq = (uint64_t)counter.QuadPart;
  }
  return freq;
#else
  return 0;
#endif
}

static uint64_t rotl64(uint64_t v, unsigned int r) {
  return (v << r) | (v >> (64u - r));
}

static void empty_encrypt(const aria_ctx_t *ctx,
                          const uint8_t *in,
                          uint8_t *out,
                          size_t len) {
  (void)ctx;
  (void)in;
  (void)out;
  (void)len;
}

static uint64_t measure_once(aria_encrypt_fn fn,
                             const aria_ctx_t *ctx,
                             const uint8_t *in,
                             uint8_t *out,
                             size_t len,
                             size_t inner,
                             uint64_t *sink) {
  uint64_t local_sink = *sink;
  uint64_t start = qpc_now();

  for (size_t i = 0; i < inner; ++i) {
    if (len > 0) {
      size_t pos = i % len;
      ((uint8_t *)in)[pos] ^= (uint8_t)(i + 1);
    }
    fn(ctx, in, out, len);
    if (len > 0) {
      local_sink ^= (uint64_t)out[0];
      local_sink ^= ((uint64_t)out[len / 2] << 8);
      local_sink = rotl64(local_sink, 5);
      local_sink ^= ((uint64_t)out[len - 1] << 16);
    } else {
      local_sink ^= (uint64_t)i;
    }
  }

  uint64_t end = qpc_now();
  *sink = local_sink;
  return end - start;
}

static size_t pick_inner(aria_encrypt_fn fn,
                         const aria_ctx_t *ctx,
                         const uint8_t *in,
                         uint8_t *out,
                         size_t len,
                         uint64_t target_ticks,
                         size_t inner_max,
                         uint64_t *sink) {
  size_t inner = 1;
  unsigned int zero_hits = 0;
  if (target_ticks == 0) {
    return inner;
  }

  for (;;) {
    uint64_t ticks = measure_once(fn, ctx, in, out, len, inner, sink);
    if (ticks == 0) {
      zero_hits++;
      if (inner < inner_max / 8) {
        inner *= 8;
      } else {
        inner = inner_max;
      }
      if (zero_hits > 4 && target_ticks < (uint64_t)1e6) {
        target_ticks *= 4;
      }
      continue;
    }
    if (ticks >= target_ticks) {
      return inner;
    }
    if (inner >= inner_max) {
      fprintf(stderr, "bench_measure: inner reached max %zu for len %zu.\n",
              inner_max, len);
      return inner_max;
    }
    if (inner > inner_max / 2) {
      inner = inner_max;
    } else {
      inner *= 2;
    }
  }
}

static int cmp_u64(const void *a, const void *b) {
  const uint64_t va = *(const uint64_t *)a;
  const uint64_t vb = *(const uint64_t *)b;
  if (va < vb) {
    return -1;
  }
  if (va > vb) {
    return 1;
  }
  return 0;
}

static double stat_ticks(const uint64_t *samples,
                         size_t count,
                         stat_mode_t mode) {
  if (count == 0) {
    return 0.0;
  }

  if (mode == STAT_MIN) {
    uint64_t min_val = samples[0];
    for (size_t i = 1; i < count; ++i) {
      if (samples[i] < min_val) {
        min_val = samples[i];
      }
    }
    return (double)min_val;
  }

  uint64_t *sorted = (uint64_t *)malloc(sizeof(uint64_t) * count);
  if (!sorted) {
    return (double)samples[0];
  }
  for (size_t i = 0; i < count; ++i) {
    sorted[i] = samples[i];
  }
  qsort(sorted, count, sizeof(uint64_t), cmp_u64);

  if (mode == STAT_MEDIAN) {
    double value = 0.0;
    if (count % 2 == 1) {
      value = (double)sorted[count / 2];
    } else {
      uint64_t a = sorted[(count / 2) - 1];
      uint64_t b = sorted[count / 2];
      value = ((double)a + (double)b) * 0.5;
    }
    free(sorted);
    return value;
  }

  size_t trim = count / 10;
  if (trim * 2 >= count) {
    trim = 0;
  }
  size_t start = trim;
  size_t end = count - trim;
  double sum = 0.0;
  for (size_t i = start; i < end; ++i) {
    sum += (double)sorted[i];
  }
  free(sorted);
  return sum / (double)(end - start);
}

const char *bench_stat_mode_name(stat_mode_t mode) {
  switch (mode) {
    case STAT_MIN:
      return "min";
    case STAT_MEDIAN:
      return "median";
    case STAT_TRIMMED_MEAN:
      return "trimmed_mean";
    default:
      return "unknown";
  }
}

bench_result_t bench_run(aria_encrypt_fn fn,
                         const aria_ctx_t *ctx,
                         const uint8_t *in,
                         uint8_t *out,
                         size_t len,
                         size_t outer,
                         uint64_t target_ticks,
                         size_t inner_max,
                         stat_mode_t mode,
                         size_t warmup) {
  bench_result_t result;
  result.len = len;
  result.inner = 1;
  result.outer = outer;
  result.total_ticks = 0;
  result.empty_ticks = 0;
  result.corrected_ticks = 0;
  result.qpc_freq = 0;
  result.ns_total = 0.0;
  result.ns_per_call = 0.0;
  result.ns_per_byte = 0.0;
  result.ns_per_byte_corrected = 0.0;
  result.ticks_per_call = 0.0;
  result.ticks_per_byte = 0.0;
  result.stat_mode = mode;
  result.sink = 0;

  if (!fn || !in || !out || outer == 0) {
    return result;
  }

  for (size_t i = 0; i < warmup; ++i) {
    fn(ctx, in, out, len);
  }

  uint64_t *samples = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  uint64_t *empty_samples = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  if (!samples || !empty_samples) {
    free(samples);
    free(empty_samples);
    return result;
  }

  result.inner = pick_inner(fn, ctx, in, out, len, target_ticks, inner_max, &result.sink);

  for (size_t i = 0; i < outer; ++i) {
    samples[i] = measure_once(fn, ctx, in, out, len, result.inner, &result.sink);
  }

  double ticks_stat = stat_ticks(samples, outer, mode);
  for (size_t i = 0; i < outer; ++i) {
    empty_samples[i] = measure_once(empty_encrypt, ctx, in, out, len, result.inner, &result.sink);
  }
  double empty_stat = stat_ticks(empty_samples, outer, mode);
  free(samples);
  free(empty_samples);

  result.total_ticks = (uint64_t)(ticks_stat + 0.5);
  result.empty_ticks = (uint64_t)(empty_stat + 0.5);
  if (ticks_stat > empty_stat) {
    result.corrected_ticks = (uint64_t)(ticks_stat - empty_stat + 0.5);
  } else {
    result.corrected_ticks = 0;
  }
  if (result.inner > 0) {
    result.ticks_per_call = ticks_stat / (double)result.inner;
    if (len > 0) {
      result.ticks_per_byte = ticks_stat / ((double)result.inner * (double)len);
    }
  }

  result.qpc_freq = qpc_freq();
  if (result.qpc_freq > 0) {
    result.ns_total = (ticks_stat * 1e9) / (double)result.qpc_freq;
    if (result.inner > 0) {
      result.ns_per_call = result.ns_total / (double)result.inner;
      if (len > 0) {
        result.ns_per_byte = result.ns_total / ((double)result.inner * (double)len);
      }
    }
    if (result.corrected_ticks > 0 && result.inner > 0 && len > 0) {
      double corrected_ns_total = ((double)result.corrected_ticks * 1e9) / (double)result.qpc_freq;
      result.ns_per_byte_corrected = corrected_ns_total / ((double)result.inner * (double)len);
    }
  }
  return result;
}
