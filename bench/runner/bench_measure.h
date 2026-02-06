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
  double ticks_per_call;
  double ticks_per_byte;
  stat_mode_t stat_mode;
  uint64_t sink;
} bench_result_t;

const char *bench_stat_mode_name(stat_mode_t mode);

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
