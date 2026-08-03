#ifndef ARIA_AUTOTUNE_H
#define ARIA_AUTOTUNE_H

#include <stddef.h>
#include <stdint.h>

#include "bench_measure.h"

typedef enum aria_autotune_policy_basis {
  ARIA_POLICY_BASIS_NORMALIZED = 0,
  ARIA_POLICY_BASIS_RAW = 1
} aria_autotune_policy_basis_t;

typedef enum aria_autotune_tail_policy {
  ARIA_TAIL_POLICY_NATIVE = 0,
  ARIA_TAIL_POLICY_CONSERVATIVE = 1
} aria_autotune_tail_policy_t;

typedef enum aria_autotune_profile {
  ARIA_AUTOTUNE_PROFILE_TEST = 0,
  ARIA_AUTOTUNE_PROFILE_FULL = 1
} aria_autotune_profile_t;

typedef enum aria_output_level {
  ARIA_OUTPUT_LEVEL_DEFAULT = 0,
  ARIA_OUTPUT_LEVEL_DEBUG = 1,
  ARIA_OUTPUT_LEVEL_RAW = 2
} aria_output_level_t;

typedef struct aria_autotune_config {
  size_t min_len;
  size_t max_len;
  size_t refine_step;
  size_t refine_buffer;
  size_t coarse_iterations;
  size_t refine_iterations;
  double trim_ratio;
  double winner_margin_pct;
  size_t stability_min_run;
  size_t policy_min_bucket_points;
  int collapse_ref_fallback;
  aria_autotune_policy_basis_t policy_basis;
  aria_autotune_tail_policy_t tail_policy;
  aria_autotune_profile_t profile;
  aria_output_level_t output_level;
  const char *output_dir;
  const char *output_prefix;
} aria_autotune_config_t;

typedef struct aria_autotune_metrics {
  double search_time_ms;
  double end_to_end_time_ms;
  size_t coarse_measurement_point_count;
  size_t fine_measurement_point_count;
  size_t autotune_candidate_measurement_count;
  size_t exhaustive_candidate_measurement_count;
  double measurement_reduction_percent;
} aria_autotune_metrics_t;

void aria_autotune_config_apply_profile(aria_autotune_config_t *config,
                                        aria_autotune_profile_t profile);
void aria_autotune_config_init(aria_autotune_config_t *config);

int aria_autotune_run(const aria_autotune_config_t *config,
                      uint8_t *input,
                      uint8_t *output,
                      size_t buffer_size,
                      uint64_t target_ticks,
                      size_t inner_max,
                      size_t warmup,
                      stat_mode_t stat_mode);

double aria_autotune_last_duration_ms(void);
const aria_autotune_metrics_t *aria_autotune_last_metrics(void);

#endif
