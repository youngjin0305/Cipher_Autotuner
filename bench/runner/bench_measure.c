#include "bench_measure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
  return time_frequency();
#endif
}

static uint64_t rotl64(uint64_t v, unsigned int r) {
  return (v << r) | (v >> (64u - r));
}

static void empty_keysetup(aria_ctx_t *ctx, const uint8_t *key, int keybits) {
  (void)ctx;
  (void)key;
  (void)keybits;
}

static void empty_encrypt(const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len) {
  (void)ctx;
  (void)in;
  (void)out;
  (void)len;
}

static uint64_t measure_once(aria_encrypt_fn fn, const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len, size_t inner, uint64_t *sink) {
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

static uint64_t measure_keysetup_once(aria_keysetup_fn fn, aria_ctx_t *ctx, uint8_t *key, int keybits, size_t inner, uint64_t *sink) {
  uint64_t local_sink = *sink;
  size_t key_bytes = (size_t)keybits / 8;
  if (key_bytes == 0) {
    key_bytes = 1;
  }
  uint64_t start = qpc_now();

  for (size_t i = 0; i < inner; ++i) {
    size_t pos = i % key_bytes;
    key[pos] ^= (uint8_t)(i + 1);
    fn(ctx, key, keybits);
    local_sink ^= (uint64_t)ctx->ref_rk[0];
    local_sink = rotl64(local_sink, 7);
    local_sink ^= (uint64_t)ctx->rounds;
  }

  uint64_t end = qpc_now();
  *sink = local_sink;
  return end - start;
}

static size_t pick_inner(aria_encrypt_fn fn, const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len, uint64_t target_ticks, size_t inner_max, uint64_t *sink) {
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

static size_t pick_inner_keysetup(aria_keysetup_fn fn, aria_ctx_t *ctx, uint8_t *key, int keybits, uint64_t target_ticks, size_t inner_max, uint64_t *sink) {
  size_t inner = 1;
  unsigned int zero_hits = 0;
  if (target_ticks == 0) {
    return inner;
  }

  for (;;) {
    uint64_t ticks = measure_keysetup_once(fn, ctx, key, keybits, inner, sink);
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
      fprintf(stderr, "bench_measure: keysetup inner reached max %zu.\n",
              inner_max);
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

static int cmp_double(const void *a, const void *b) {
  const double va = *(const double *)a;
  const double vb = *(const double *)b;
  if (va < vb) {
    return -1;
  }
  if (va > vb) {
    return 1;
  }
  return 0;
}

double bench_percentile_from_sorted(const double *sorted, size_t count, double pct) {
  double pos;
  size_t lo;
  size_t hi;
  double frac;

  if (!sorted || count == 0) {
    return 0.0;
  }
  if (count == 1) {
    return sorted[0];
  }
  if (pct <= 0.0) {
    return sorted[0];
  }
  if (pct >= 1.0) {
    return sorted[count - 1];
  }

  pos = pct * (double)(count - 1);
  lo = (size_t)pos;
  hi = (lo + 1 < count) ? (lo + 1) : lo;
  frac = pos - (double)lo;
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

double bench_mean_samples(const double *samples, size_t count) {
  double sum = 0.0;
  size_t i;

  if (!samples || count == 0) {
    return 0.0;
  }

  for (i = 0; i < count; ++i) {
    sum += samples[i];
  }

  return sum / (double)count;
}

int bench_compute_trimmed_mean(const double *samples,
                               size_t count,
                               double trim_ratio,
                               size_t min_remaining,
                               double *trimmed_mean,
                               size_t *trim_count_each_side) {
  double *sorted;
  double sum = 0.0;
  size_t start = 0;
  size_t end = count;
  size_t i;

  if (!samples || !trimmed_mean || count == 0 || !isfinite(trim_ratio) ||
      trim_ratio < 0.0 || trim_ratio >= 0.5) {
    return 0;
  }

  sorted = (double *)malloc(sizeof(double) * count);
  if (!sorted) {
    return 0;
  }

  for (i = 0; i < count; ++i) {
    if (!isfinite(samples[i])) {
      free(sorted);
      return 0;
    }
    sorted[i] = samples[i];
  }

  qsort(sorted, count, sizeof(double), cmp_double);
  if (!bench_trim_bounds(count, trim_ratio, min_remaining, &start, &end)) {
    free(sorted);
    return 0;
  }

  for (i = start; i < end; ++i) {
    sum += sorted[i];
  }

  *trimmed_mean = sum / (double)(end - start);
  if (trim_count_each_side) {
    *trim_count_each_side = start;
  }
  free(sorted);
  return 1;
}

double bench_ticks_to_ns(uint64_t ticks, uint64_t freq) {
  if (freq == 0) {
    return 0.0;
  }
  return ((double)ticks * 1e9) / (double)freq;
}

int bench_trim_bounds(size_t count, double trim_ratio, size_t min_remaining,
                      size_t *start, size_t *end) {
  size_t trim;

  if (!isfinite(trim_ratio) || trim_ratio < 0.0 || trim_ratio >= 0.5 ||
      count < min_remaining) {
    return 0;
  }
  trim = (size_t)((double)count * trim_ratio);
  if (trim > count / 2 || count - (trim * 2) < min_remaining) {
    return 0;
  }

  if (start) {
    *start = trim;
  }
  if (end) {
    *end = count - trim;
  }
  return 1;
}

static double stat_ticks(const uint64_t *samples, size_t count, stat_mode_t mode,
                         double trim_ratio) {
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

  size_t start = 0;
  size_t end = count;
  if (!bench_trim_bounds(count, trim_ratio, BENCH_MIN_TRIMMED_SAMPLES, &start, &end)) {
    free(sorted);
    return 0.0;
  }
  double sum = 0.0;
  for (size_t i = start; i < end; ++i) {
    sum += (double)sorted[i];
  }
  free(sorted);
  return sum / (double)(end - start);
}

void bench_result_cleanup(bench_result_t *result) {
  if (!result) {
    return;
  }

  free(result->samples.total_ticks);
  free(result->samples.empty_ticks);
  free(result->samples.corrected_ticks);
  result->samples.count = 0;
  result->samples.total_ticks = NULL;
  result->samples.empty_ticks = NULL;
  result->samples.corrected_ticks = NULL;
}

bench_summary_stats_t bench_compute_summary_stats(const bench_result_t *result,
                                                  double trim_ratio) {
  bench_summary_stats_t stats;
  double *per_call = NULL;
  double *per_call_sorted = NULL;
  double *per_byte = NULL;
  double *per_byte_sorted = NULL;
  size_t count = 0;
  size_t i;
  double variance_sum = 0.0;

  memset(&stats, 0, sizeof(stats));
  if (!result || result->samples.count == 0 || result->inner == 0 || result->qpc_freq == 0) {
    return stats;
  }

  count = result->samples.count;
  per_call = (double *)malloc(sizeof(double) * count);
  per_call_sorted = (double *)malloc(sizeof(double) * count);
  if (result->len > 0) {
    per_byte = (double *)malloc(sizeof(double) * count);
    per_byte_sorted = (double *)malloc(sizeof(double) * count);
  }

  if (!per_call || !per_call_sorted || (result->len > 0 && (!per_byte || !per_byte_sorted))) {
    free(per_call);
    free(per_call_sorted);
    free(per_byte);
    free(per_byte_sorted);
    return stats;
  }

  for (i = 0; i < count; ++i) {
    const double ns_corrected = bench_ticks_to_ns(result->samples.corrected_ticks[i], result->qpc_freq);
    per_call[i] = ns_corrected / (double)result->inner;
    if (!isfinite(per_call[i]) || per_call[i] <= 0.0) {
      free(per_call);
      free(per_call_sorted);
      free(per_byte);
      free(per_byte_sorted);
      return stats;
    }
    per_call_sorted[i] = per_call[i];
    if (per_byte && per_byte_sorted) {
      per_byte[i] = ns_corrected / ((double)result->inner * (double)result->len);
      per_byte_sorted[i] = per_byte[i];
    }
  }

  qsort(per_call_sorted, count, sizeof(double), cmp_double);
  if (per_byte_sorted) {
    qsort(per_byte_sorted, count, sizeof(double), cmp_double);
  }

  stats.n_samples = count;
  stats.trim_ratio = trim_ratio;
  stats.raw_mean_ns_per_call = bench_mean_samples(per_call, count);
  if (!bench_compute_trimmed_mean(per_call,
                                  count,
                                  trim_ratio,
                                  BENCH_MIN_TRIMMED_SAMPLES,
                                  &stats.trimmed_mean_ns_per_call,
                                  &stats.trim_count_each_side)) {
    free(per_call);
    free(per_call_sorted);
    free(per_byte);
    free(per_byte_sorted);
    fprintf(stderr, "bench_measure: insufficient valid samples for trim_ratio=%.3f (valid=%zu).\n",
            trim_ratio, count);
    return stats;
  }
  stats.ns_per_call_min = per_call_sorted[0];
  stats.ns_per_call_max = per_call_sorted[count - 1];
  stats.ns_per_call_p50 = bench_percentile_from_sorted(per_call_sorted, count, 0.50);
  stats.ns_per_call_p95 = bench_percentile_from_sorted(per_call_sorted, count, 0.95);
  stats.ns_per_call_p99 = bench_percentile_from_sorted(per_call_sorted, count, 0.99);
  for (i = 0; i < count; ++i) {
    const double delta = per_call[i] - stats.raw_mean_ns_per_call;
    variance_sum += delta * delta;
  }
  stats.standard_deviation_ns_per_call = sqrt(variance_sum / (double)count);
  stats.iqr_ns_per_call = bench_percentile_from_sorted(per_call_sorted, count, 0.75) -
                          bench_percentile_from_sorted(per_call_sorted, count, 0.25);

  if (per_byte && per_byte_sorted) {
    stats.raw_mean_ns_per_byte = bench_mean_samples(per_byte, count);
    if (!bench_compute_trimmed_mean(per_byte,
                                    count,
                                    trim_ratio,
                                    BENCH_MIN_TRIMMED_SAMPLES,
                                    &stats.trimmed_mean_ns_per_byte,
                                    NULL)) {
      free(per_call);
      free(per_call_sorted);
      free(per_byte);
      free(per_byte_sorted);
      return stats;
    }
    stats.ns_per_byte_p50_corrected = bench_percentile_from_sorted(per_byte_sorted, count, 0.50);
  }
  stats.valid = 1;

  free(per_call);
  free(per_call_sorted);
  free(per_byte);
  free(per_byte_sorted);
  return stats;
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

bench_result_t bench_run(aria_encrypt_fn fn, const aria_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len,
                         size_t outer, uint64_t target_ticks, size_t inner_max, stat_mode_t mode,
                         size_t warmup, double trim_ratio) {
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
  result.samples.count = 0;
  result.samples.total_ticks = NULL;
  result.samples.empty_ticks = NULL;
  result.samples.corrected_ticks = NULL;

  if (!fn || !in || !out || outer == 0) {
    return result;
  }

  for (size_t i = 0; i < warmup; ++i) {
    fn(ctx, in, out, len);
  }

  result.samples.total_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  result.samples.empty_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  result.samples.corrected_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  if (!result.samples.total_ticks || !result.samples.empty_ticks || !result.samples.corrected_ticks) {
    bench_result_cleanup(&result);
    return result;
  }
  result.inner = pick_inner(fn, ctx, in, out, len, target_ticks, inner_max, &result.sink);

  {
    size_t attempts = 0;
    const size_t max_attempts = outer <= SIZE_MAX / BENCH_INVALID_SAMPLE_RETRIES
                                    ? outer * BENCH_INVALID_SAMPLE_RETRIES
                                    : SIZE_MAX;
    while (result.samples.count < outer && attempts < max_attempts) {
      const size_t i = result.samples.count;
      uint64_t t = measure_once(fn, ctx, in, out, len, result.inner, &result.sink);
      uint64_t e = measure_once(empty_encrypt, ctx, in, out, len, result.inner, &result.sink);
      attempts++;
      if (t <= e) {
        continue;
      }
      result.samples.total_ticks[i] = t;
      result.samples.empty_ticks[i] = e;
      result.samples.corrected_ticks[i] = t - e;
      result.samples.count++;
    }
  }
  if (result.samples.count < outer) {
    fprintf(stderr,
            "bench_measure: only %zu/%zu valid corrected samples for len=%zu.\n",
            result.samples.count, outer, len);
  }

  double total_stat = stat_ticks(result.samples.total_ticks, result.samples.count, mode, trim_ratio);
  double empty_stat = stat_ticks(result.samples.empty_ticks, result.samples.count, mode, trim_ratio);
  double diff_stat  = stat_ticks(result.samples.corrected_ticks, result.samples.count, mode, trim_ratio);

  result.total_ticks = (uint64_t)(total_stat + 0.5);
  result.empty_ticks = (uint64_t)(empty_stat + 0.5);
  result.corrected_ticks = (uint64_t)(diff_stat + 0.5);

  if (result.inner > 0) {
    result.ticks_per_call = total_stat / (double)result.inner;
    if (len > 0) {
      result.ticks_per_byte = total_stat / ((double)result.inner * (double)len);
    }
  }

  result.qpc_freq = qpc_freq();
  if (result.qpc_freq > 0) {
    result.ns_total = (total_stat * 1e9) / (double)result.qpc_freq;
    if (result.inner > 0) {
      result.ns_per_call = result.ns_total / (double)result.inner;
      if (len > 0) {
        result.ns_per_byte = result.ns_total / ((double)result.inner * (double)len);
      }
    }

    if (diff_stat > 0.0 && result.inner > 0 && len > 0) {
      double corrected_ns_total = (diff_stat * 1e9) / (double)result.qpc_freq;
      result.ns_per_byte_corrected = corrected_ns_total / ((double)result.inner * (double)len);
    }
  }
  return result;
}

bench_result_t bench_run_keysetup(aria_keysetup_fn fn, aria_ctx_t *ctx, uint8_t *key, int keybits, size_t outer, uint64_t target_ticks,
                                  size_t inner_max, stat_mode_t mode, size_t warmup,
                                  double trim_ratio) {
  bench_result_t result;
  result.len = 0;
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
  result.samples.count = 0;
  result.samples.total_ticks = NULL;
  result.samples.empty_ticks = NULL;
  result.samples.corrected_ticks = NULL;

  if (!fn || !ctx || !key || outer == 0) {
    return result;
  }

  for (size_t i = 0; i < warmup; ++i) {
    fn(ctx, key, keybits);
  }

  result.samples.total_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  result.samples.empty_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  result.samples.corrected_ticks = (uint64_t *)malloc(sizeof(uint64_t) * outer);
  if (!result.samples.total_ticks || !result.samples.empty_ticks || !result.samples.corrected_ticks) {
    bench_result_cleanup(&result);
    return result;
  }
  result.inner = pick_inner_keysetup(fn, ctx, key, keybits, target_ticks, inner_max, &result.sink);

  {
    size_t attempts = 0;
    const size_t max_attempts = outer <= SIZE_MAX / BENCH_INVALID_SAMPLE_RETRIES
                                    ? outer * BENCH_INVALID_SAMPLE_RETRIES
                                    : SIZE_MAX;
    while (result.samples.count < outer && attempts < max_attempts) {
      const size_t i = result.samples.count;
      uint64_t t = measure_keysetup_once(fn, ctx, key, keybits, result.inner, &result.sink);
      uint64_t e = measure_keysetup_once(empty_keysetup, ctx, key, keybits, result.inner, &result.sink);
      attempts++;
      if (t <= e) {
        continue;
      }
      result.samples.total_ticks[i] = t;
      result.samples.empty_ticks[i] = e;
      result.samples.corrected_ticks[i] = t - e;
      result.samples.count++;
    }
  }
  if (result.samples.count < outer) {
    fprintf(stderr,
            "bench_measure: only %zu/%zu valid key-setup samples for key_bits=%d.\n",
            result.samples.count, outer, keybits);
  }

  double total_stat = stat_ticks(result.samples.total_ticks, result.samples.count, mode, trim_ratio);
  double empty_stat = stat_ticks(result.samples.empty_ticks, result.samples.count, mode, trim_ratio);
  double diff_stat  = stat_ticks(result.samples.corrected_ticks, result.samples.count, mode, trim_ratio);

  result.total_ticks = (uint64_t)(total_stat + 0.5);
  result.empty_ticks = (uint64_t)(empty_stat + 0.5);
  result.corrected_ticks = (uint64_t)(diff_stat + 0.5);

  if (result.inner > 0) {
    result.ticks_per_call = total_stat / (double)result.inner;
  }

  result.qpc_freq = qpc_freq();
  if (result.qpc_freq > 0) {
    result.ns_total = (total_stat * 1e9) / (double)result.qpc_freq;
    if (result.inner > 0) {
      result.ns_per_call = result.ns_total / (double)result.inner;
    }
  }

  return result;
}
