#ifndef BENCH_MEASURE_H
#define BENCH_MEASURE_H

#include <stddef.h>
#include <stdint.h>

#include "aria_api.h"

typedef enum stat_mode {
  STAT_MIN = 0,
  STAT_MEDIAN = 1,
  STAT_TRIMMED_MEAN = 2
} stat_mode_t;

#define BENCH_TRIM_RATIO 0.10

typedef void (*aria_keysetup_fn)(aria_ctx_t *ctx, const uint8_t *key, int keybits);

typedef struct bench_samples {
  size_t count;
  uint64_t *total_ticks;
  uint64_t *empty_ticks;
  uint64_t *corrected_ticks;
} bench_samples_t;

typedef struct bench_result {
  size_t len;
  size_t inner;
  size_t outer;
  uint64_t total_ticks;
  uint64_t empty_ticks;
  uint64_t corrected_ticks;
  uint64_t qpc_freq;
  double ns_total;
  double ns_per_call;
  double ns_per_byte;
  double ns_per_byte_corrected;
  double ticks_per_call;
  double ticks_per_byte;
  stat_mode_t stat_mode;
  uint64_t sink;
  bench_samples_t samples;
} bench_result_t;

typedef struct bench_summary_stats {
  size_t n_samples;
  double ns_per_call_mean;
  double ns_per_call_trimmed_mean;
  double ns_per_call_min;
  double ns_per_call_max;
  double ns_per_call_p50;
  double ns_per_call_p95;
  double ns_per_call_p99;
  double ns_per_byte_mean_corrected;
  double ns_per_byte_trimmed_mean_corrected;
  double ns_per_byte_p50_corrected;
} bench_summary_stats_t;

const char *bench_stat_mode_name(stat_mode_t mode);
double bench_ticks_to_ns(uint64_t ticks, uint64_t freq);
void bench_trim_bounds(size_t count, size_t *start, size_t *end);
void bench_result_cleanup(bench_result_t *result);
bench_summary_stats_t bench_compute_summary_stats(const bench_result_t *result);

bench_result_t bench_run(aria_encrypt_fn fn,
                         const aria_ctx_t *ctx,
                         const uint8_t *in,
                         uint8_t *out,
                         size_t len,
                         size_t outer,
                         uint64_t target_ticks,
                         size_t inner_max,
                         stat_mode_t mode,
                         size_t warmup);

bench_result_t bench_run_keysetup(aria_keysetup_fn fn,
                                  aria_ctx_t *ctx,
                                  uint8_t *key,
                                  int keybits,
                                  size_t outer,
                                  uint64_t target_ticks,
                                  size_t inner_max,
                                  stat_mode_t mode,
                                  size_t warmup);

#endif
