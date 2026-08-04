#include "dispatch_evaluation.h"

#include "aria_api.h"
#include "runtime_dispatch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EVALUATION_KEY_COUNT 3u
#define EVALUATION_LENGTH_COUNT 11u
#define EVALUATION_IMPL_COUNT 4u

typedef enum evaluation_mode {
  EVALUATION_DIRECT_REFERENCE = 0,
  EVALUATION_BEST_FIXED = 1,
  EVALUATION_STATIC_HEURISTIC = 2,
  EVALUATION_AUTOTUNED_POLICY = 3,
  EVALUATION_MODE_COUNT = 4
} evaluation_mode_t;

typedef struct direct_measurement {
  int valid;
  int key_bits;
  size_t length;
  const aria_impl_t *impl;
  size_t inner_iterations;
  bench_summary_stats_t stats;
} direct_measurement_t;

typedef struct evaluation_total {
  double trimmed_mean_sum;
  double throughput_sum;
  double log_speedup_sum;
  double dispatch_overhead_sum;
  double max_dispatch_overhead;
  size_t count;
} evaluation_total_t;

static const int evaluation_key_bits_values[EVALUATION_KEY_COUNT] = {128, 192, 256};
static const size_t evaluation_lengths[EVALUATION_LENGTH_COUNT] = {
  16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096
};
static const aria_impl_t *evaluation_impls[EVALUATION_IMPL_COUNT] = {
  &aria_ref_impl,
  &aria_linux_aesni_avx_impl,
  &aria_linux_aesni_avx2_impl,
  &aria_linux_gfni_avx512_impl
};

/* Evaluation is intentionally single-threaded. */
static int evaluation_key_bits;
static scenario_t evaluation_scenario;

static const char *profile_name(aria_autotune_profile_t profile) {
  return profile == ARIA_AUTOTUNE_PROFILE_FULL ? "full" : "test";
}

static const char *policy_basis_name(aria_autotune_policy_basis_t basis) {
  return basis == ARIA_POLICY_BASIS_RAW ? "raw" : "normalized";
}

static const char *evaluation_mode_name(evaluation_mode_t mode) {
  switch (mode) {
    case EVALUATION_DIRECT_REFERENCE:
      return "direct_reference";
    case EVALUATION_BEST_FIXED:
      return "best_fixed_implementation";
    case EVALUATION_STATIC_HEURISTIC:
      return "static_heuristic_dispatch";
    case EVALUATION_AUTOTUNED_POLICY:
      return "autotuned_policy_dispatch";
    default:
      return "unknown";
  }
}

static void static_heuristic_encrypt(const aria_ctx_t *ctx,
                                     const uint8_t *input,
                                     uint8_t *output,
                                     size_t len) {
  aria_runtime_dispatch_scenario(len, evaluation_scenario)->encrypt(ctx, input, output, len);
}

static void autotuned_policy_encrypt(const aria_ctx_t *ctx,
                                     const uint8_t *input,
                                     uint8_t *output,
                                     size_t len) {
  aria_autotuned_dispatch(evaluation_key_bits, len)->encrypt(ctx, input, output, len);
}

static int output_path(char *path,
                       size_t path_size,
                       const aria_autotune_config_t *config,
                       const char *filename) {
  int written;
  if (!path || path_size == 0 || !config || !filename) {
    return 0;
  }
  written = config->output_dir && config->output_dir[0] != '\0'
                ? snprintf(path, path_size, "%s/%s", config->output_dir, filename)
                : snprintf(path, path_size, "out/%s", filename);
  return written >= 0 && (size_t)written < path_size;
}

static direct_measurement_t *find_direct_measurement(direct_measurement_t *measurements,
                                                     size_t count,
                                                     int key_bits,
                                                     size_t length,
                                                     const aria_impl_t *impl) {
  size_t i;
  for (i = 0; i < count; ++i) {
    if (measurements[i].valid && measurements[i].key_bits == key_bits &&
        measurements[i].length == length && measurements[i].impl == impl) {
      return &measurements[i];
    }
  }
  return NULL;
}

static int measure_direct_candidates(const aria_autotune_config_t *config,
                                     uint8_t *input,
                                     uint8_t *output,
                                     uint64_t target_ticks,
                                     size_t inner_max,
                                     size_t warmup,
                                     size_t outer,
                                     stat_mode_t stat_mode,
                                     direct_measurement_t *measurements,
                                     size_t capacity,
                                     size_t *measurement_count) {
  size_t key_index;
  size_t out = 0;

  for (key_index = 0; key_index < EVALUATION_KEY_COUNT; ++key_index) {
    const int key_bits = evaluation_key_bits_values[key_index];
    uint8_t key[32] = {0};
    size_t length_index;

    for (length_index = 0; length_index < EVALUATION_LENGTH_COUNT; ++length_index) {
      const size_t length = evaluation_lengths[length_index];
      size_t impl_index;
      if (length < config->min_len || length > config->max_len) {
        continue;
      }
      for (impl_index = 0; impl_index < EVALUATION_IMPL_COUNT; ++impl_index) {
        const aria_impl_t *impl = evaluation_impls[impl_index];
        aria_ctx_t ctx;
        bench_result_t result;
        bench_summary_stats_t stats;

        if ((impl->is_supported && !impl->is_supported()) || out >= capacity) {
          continue;
        }
        memset(&ctx, 0, sizeof(ctx));
        impl->init(&ctx, key, key_bits);
        result = bench_run(impl->encrypt,
                           &ctx,
                           input,
                           output,
                           length,
                           outer,
                           target_ticks,
                           inner_max,
                           stat_mode,
                           warmup,
                           config->trim_ratio);
        stats = bench_compute_summary_stats(&result, config->trim_ratio);
        if (!stats.valid) {
          bench_result_cleanup(&result);
          return 0;
        }
        measurements[out].valid = 1;
        measurements[out].key_bits = key_bits;
        measurements[out].length = length;
        measurements[out].impl = impl;
        measurements[out].inner_iterations = result.inner;
        measurements[out].stats = stats;
        out++;
        bench_result_cleanup(&result);
      }
    }
  }
  *measurement_count = out;
  return out > 0;
}

static const aria_impl_t *select_best_fixed(direct_measurement_t *measurements,
                                            size_t measurement_count) {
  const aria_impl_t *best = NULL;
  double best_macro_average = 0.0;
  size_t impl_index;

  for (impl_index = 0; impl_index < EVALUATION_IMPL_COUNT; ++impl_index) {
    const aria_impl_t *impl = evaluation_impls[impl_index];
    double sum = 0.0;
    size_t count = 0;
    size_t i;
    if (impl->is_supported && !impl->is_supported()) {
      continue;
    }
    for (i = 0; i < measurement_count; ++i) {
      if (measurements[i].valid && measurements[i].impl == impl) {
        sum += measurements[i].stats.trimmed_mean_ns_per_call;
        count++;
      }
    }
    if (count > 0 && (!best || sum / (double)count < best_macro_average)) {
      best = impl;
      best_macro_average = sum / (double)count;
    }
  }
  return best;
}

static int measure_dispatched(const aria_autotune_config_t *config,
                              evaluation_mode_t mode,
                              const aria_impl_t *selected_impl,
                              int key_bits,
                              size_t length,
                              uint8_t *input,
                              uint8_t *output,
                              uint64_t target_ticks,
                              size_t inner_max,
                              size_t warmup,
                              size_t outer,
                              stat_mode_t stat_mode,
                              bench_summary_stats_t *stats,
                              size_t *inner_iterations) {
  aria_ctx_t ctx;
  uint8_t key[32] = {0};
  aria_encrypt_fn encrypt = mode == EVALUATION_STATIC_HEURISTIC
                                ? static_heuristic_encrypt
                                : autotuned_policy_encrypt;
  bench_result_t result;

  memset(&ctx, 0, sizeof(ctx));
  selected_impl->init(&ctx, key, key_bits);
  evaluation_key_bits = key_bits;
  result = bench_run(encrypt,
                     &ctx,
                     input,
                     output,
                     length,
                     outer,
                     target_ticks,
                     inner_max,
                     stat_mode,
                     warmup,
                     config->trim_ratio);
  *stats = bench_compute_summary_stats(&result, config->trim_ratio);
  *inner_iterations = result.inner;
  bench_result_cleanup(&result);
  return stats->valid;
}

static void print_installed_policy(void) {
  size_t i;
  printf("\nGenerated Policy (%zu ranges)\n", aria_policy_count());
  for (i = 0; i < aria_policy_count(); ++i) {
    const aria_policy_range_t *entry = aria_policy_entry(i);
    printf("  key=%d len=%zu..%zu impl=%s\n",
           entry->key_bits, entry->start_len, entry->end_len, entry->impl->name);
  }
}

int aria_run_dispatch_evaluation(const aria_autotune_config_t *config,
                                 uint8_t *input,
                                 uint8_t *output,
                                 size_t buffer_size,
                                 uint64_t target_ticks,
                                 size_t inner_max,
                                 size_t warmup,
                                 size_t outer,
                                 stat_mode_t stat_mode,
                                 double autotune_time_ms,
                                 scenario_t scenario) {
  direct_measurement_t direct[EVALUATION_KEY_COUNT * EVALUATION_LENGTH_COUNT * EVALUATION_IMPL_COUNT] = {{0}};
  size_t direct_count = 0;
  const aria_impl_t *best_fixed;
  evaluation_total_t totals[EVALUATION_MODE_COUNT] = {{0}};
  char detail_path[512];
  char summary_path[512];
  FILE *detail = NULL;
  FILE *summary = NULL;
  size_t key_index;

  if (!config || !input || !output || config->max_len > buffer_size || aria_policy_count() == 0 ||
      !output_path(detail_path, sizeof(detail_path), config, "dispatch_evaluation.csv") ||
      !output_path(summary_path, sizeof(summary_path), config, "dispatch_evaluation_summary.csv")) {
    return 0;
  }
  evaluation_scenario = scenario;
  if (!measure_direct_candidates(config, input, output, target_ticks, inner_max, warmup, outer,
                                 stat_mode, direct, sizeof(direct) / sizeof(direct[0]), &direct_count)) {
    return 0;
  }
  best_fixed = select_best_fixed(direct, direct_count);
  if (!best_fixed) {
    return 0;
  }

  detail = fopen(detail_path, "w");
  summary = fopen(summary_path, "w");
  if (!detail || !summary) {
    fprintf(stderr, "evaluation: failed to open output: %s\n", strerror(errno));
    if (detail) fclose(detail);
    if (summary) fclose(summary);
    return 0;
  }
  fprintf(detail,
          "scenario,profile,dispatch_mode,key_bits,message_length,selected_implementation,policy_basis,warmup_iterations,inner_iterations,outer_samples_requested,outer_samples_valid,trim_ratio,trim_count_each_side,stat_mode,empty_loop_correction,trimmed_mean_ns_per_call,raw_mean_ns_per_call,median_ns_per_call,standard_deviation_ns_per_call,iqr_ns_per_call,trimmed_mean_ns_per_byte,throughput_bytes_per_sec,throughput_mib_per_sec,speedup_vs_direct_ref,dispatch_overhead_ns_per_call\n");

  printf("\nDispatch Evaluation (Best Fixed Implementation=%s)\n", best_fixed->name);
  for (key_index = 0; key_index < EVALUATION_KEY_COUNT; ++key_index) {
    const int key_bits = evaluation_key_bits_values[key_index];
    size_t length_index;
    for (length_index = 0; length_index < EVALUATION_LENGTH_COUNT; ++length_index) {
      const size_t length = evaluation_lengths[length_index];
      direct_measurement_t *direct_ref;
      evaluation_mode_t mode;
      if (length < config->min_len || length > config->max_len) {
        continue;
      }
      direct_ref = find_direct_measurement(direct, direct_count, key_bits, length, &aria_ref_impl);
      if (!direct_ref) {
        fclose(detail);
        fclose(summary);
        return 0;
      }
      for (mode = EVALUATION_DIRECT_REFERENCE; mode < EVALUATION_MODE_COUNT; ++mode) {
        const aria_impl_t *selected = mode == EVALUATION_DIRECT_REFERENCE
                                          ? &aria_ref_impl
                                          : mode == EVALUATION_BEST_FIXED
                                                ? best_fixed
                                                : mode == EVALUATION_STATIC_HEURISTIC
                                                      ? aria_runtime_dispatch_scenario(length, scenario)
                                                      : aria_autotuned_dispatch(key_bits, length);
        direct_measurement_t *selected_direct = find_direct_measurement(direct, direct_count,
                                                                         key_bits, length, selected);
        bench_summary_stats_t stats;
        size_t inner_iterations;
        double throughput_bytes;
        double throughput_mib;
        double speedup;
        double dispatch_overhead = 0.0;

        if (!selected_direct) {
          fclose(detail);
          fclose(summary);
          return 0;
        }
        if (mode == EVALUATION_DIRECT_REFERENCE || mode == EVALUATION_BEST_FIXED) {
          stats = selected_direct->stats;
          inner_iterations = selected_direct->inner_iterations;
        } else if (!measure_dispatched(config, mode, selected, key_bits, length, input, output,
                                       target_ticks, inner_max, warmup, outer, stat_mode,
                                       &stats, &inner_iterations)) {
          fclose(detail);
          fclose(summary);
          return 0;
        } else {
          dispatch_overhead = stats.trimmed_mean_ns_per_call -
                              selected_direct->stats.trimmed_mean_ns_per_call;
        }
        throughput_bytes = ((double)length * 1e9) / stats.trimmed_mean_ns_per_call;
        throughput_mib = throughput_bytes / (1024.0 * 1024.0);
        speedup = direct_ref->stats.trimmed_mean_ns_per_call / stats.trimmed_mean_ns_per_call;

        totals[mode].trimmed_mean_sum += stats.trimmed_mean_ns_per_call;
        totals[mode].throughput_sum += throughput_mib;
        totals[mode].log_speedup_sum += log(speedup);
        totals[mode].dispatch_overhead_sum += dispatch_overhead;
        if (totals[mode].count == 0 || dispatch_overhead > totals[mode].max_dispatch_overhead) {
          totals[mode].max_dispatch_overhead = dispatch_overhead;
        }
        totals[mode].count++;

        fprintf(detail,
                "%s,%s,%s,%d,%zu,%s,%s,%zu,%zu,%zu,%zu,%.6f,%zu,trimmed_mean,per_outer_sample,%.6f,%.6f,%.6f,%.6f,%.6f,%.9f,%.3f,%.6f,%.9f,%.6f\n",
                aria_scenario_name(scenario), profile_name(config->profile), evaluation_mode_name(mode),
                key_bits, length, selected->name, policy_basis_name(config->policy_basis), warmup,
                inner_iterations, outer, stats.n_samples, stats.trim_ratio, stats.trim_count_each_side,
                stats.trimmed_mean_ns_per_call, stats.raw_mean_ns_per_call, stats.ns_per_call_p50,
                stats.standard_deviation_ns_per_call, stats.iqr_ns_per_call,
                stats.trimmed_mean_ns_per_byte, throughput_bytes, throughput_mib, speedup,
                dispatch_overhead);
      }
    }
  }

  fprintf(summary,
          "scenario,profile,dispatch_mode,point_count,trim_ratio,macro_avg_trimmed_mean_ns_per_call,macro_avg_throughput_mib_per_sec,geomean_speedup_vs_direct_ref,macro_avg_dispatch_overhead_ns,max_dispatch_overhead_ns,end_to_end_autotune_time_ms\n");
  for (int mode = EVALUATION_DIRECT_REFERENCE; mode < EVALUATION_MODE_COUNT; ++mode) {
    const evaluation_total_t *total = &totals[mode];
    fprintf(summary, "%s,%s,%s,%zu,%.6f,%.6f,%.6f,%.9f,%.6f,%.6f,%.3f\n",
            aria_scenario_name(scenario), profile_name(config->profile),
            evaluation_mode_name((evaluation_mode_t)mode), total->count,
            config->trim_ratio,
            total->trimmed_mean_sum / (double)total->count,
            total->throughput_sum / (double)total->count,
            exp(total->log_speedup_sum / (double)total->count),
            total->dispatch_overhead_sum / (double)total->count,
            total->max_dispatch_overhead,
            mode == EVALUATION_AUTOTUNED_POLICY ? autotune_time_ms : 0.0);
  }

  fclose(detail);
  fclose(summary);
  print_installed_policy();
  return 1;
}
