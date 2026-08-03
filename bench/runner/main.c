#include "bench_measure.h"
#include "autotune.h"
#include "runtime_dispatch.h"
#include "aria_api.h"
#include "dispatch_evaluation.h"
#include "cpu_features.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include "common_time.h"
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define BENCH_STRINGIFY_IMPL(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_IMPL(x)

typedef enum bench_output_kind {
  BENCH_OUTPUT_RESULTS = 0,
  BENCH_OUTPUT_SUMMARY = 1,
  BENCH_OUTPUT_META = 2,
  BENCH_OUTPUT_KEYSETUP = 3,
  BENCH_OUTPUT_RAW = 4
} bench_output_kind_t;

static int ensure_directory_single(const char *path) {
  if (!path || path[0] == '\0') {
    return 1;
  }
#if defined(_WIN32)
  if (_mkdir(path) == 0 || errno == EEXIST) {
    return 1;
  }
#else
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return 1;
  }
#endif
  return 0;
}

static int ensure_directory_recursive(const char *path) {
  char buffer[512];
  size_t i;
  size_t len;

  if (!path || path[0] == '\0') {
    return 1;
  }

  len = strlen(path);
  if (len >= sizeof(buffer)) {
    return 0;
  }
  snprintf(buffer, sizeof(buffer), "%s", path);

  for (i = 1; i < len; ++i) {
    if (buffer[i] == '/' || buffer[i] == '\\') {
      char saved = buffer[i];
      buffer[i] = '\0';
      if (!ensure_directory_single(buffer)) {
        return 0;
      }
      buffer[i] = saved;
    }
  }

  return ensure_directory_single(buffer);
}

static int should_emit_benchmark_output(aria_output_level_t output_level, bench_output_kind_t kind) {
  switch (kind) {
    case BENCH_OUTPUT_RESULTS:
    case BENCH_OUTPUT_SUMMARY:
    case BENCH_OUTPUT_META:
      return 1;
    case BENCH_OUTPUT_KEYSETUP:
      return output_level >= ARIA_OUTPUT_LEVEL_DEBUG;
    case BENCH_OUTPUT_RAW:
      return output_level >= ARIA_OUTPUT_LEVEL_RAW;
    default:
      return 0;
  }
}

static int build_benchmark_output_path(char *buffer,
                                       size_t buffer_size,
                                       const aria_autotune_config_t *config,
                                       const char *filename) {
  if (!buffer || buffer_size == 0 || !config || !filename) {
    return 0;
  }

  if (config->output_dir && config->output_dir[0] != '\0') {
    if (!ensure_directory_recursive(config->output_dir)) {
      return 0;
    }
    snprintf(buffer, buffer_size, "%s/%s", config->output_dir, filename);
    return 1;
  }

  if (!ensure_directory_recursive("out")) {
    return 0;
  }
  snprintf(buffer, buffer_size, "out/%s", filename);
  return 1;
}

static void close_file_if_open(FILE **fp) {
  if (!fp || !*fp) {
    return;
  }
  fclose(*fp);
  *fp = NULL;
}

static const char *build_type_name(void) {
#if defined(NDEBUG)
  return "Release";
#else
  return "Debug";
#endif
}

static const char *compiler_name(void) {
#if defined(_MSC_FULL_VER)
  return "MSVC";
#elif defined(_MSC_VER)
  return "MSVC";
#elif defined(__clang__)
  return "Clang";
#elif defined(__GNUC__)
  return "GCC";
#else
  return "unknown";
#endif
}

static const char *compiler_version(void) {
#if defined(_MSC_FULL_VER)
  return BENCH_STRINGIFY(_MSC_FULL_VER);
#elif defined(_MSC_VER)
  return BENCH_STRINGIFY(_MSC_VER);
#elif defined(__clang_version__)
  return __clang_version__;
#elif defined(__VERSION__)
  return __VERSION__;
#else
  return "unknown";
#endif
}

static const char *platform_name(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "apple";
#else
  return "unknown";
#endif
}

static void make_run_id(char *buffer, size_t buffer_size) {
  time_t now = time(NULL);
  struct tm tm_info;

  if (!buffer || buffer_size == 0) {
    return;
  }

#if defined(_WIN32)
  localtime_s(&tm_info, &now);
#else
  localtime_r(&now, &tm_info);
#endif
  strftime(buffer, buffer_size, "run_%Y%m%d_%H%M%S", &tm_info);
}

static void make_timestamp(char *buffer, size_t buffer_size) {
  time_t now = time(NULL);
  struct tm tm_info;

  if (!buffer || buffer_size == 0) {
    return;
  }

#if defined(_WIN32)
  localtime_s(&tm_info, &now);
#else
  localtime_r(&now, &tm_info);
#endif
  strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%S", &tm_info);
}

static const char *effective_path_for(const aria_impl_t *impl, size_t len) {
  if (!impl) {
    return "unknown";
  }
  if (impl->effective_path) {
    return impl->effective_path(len);
  }
  if (impl->name) {
    return impl->name;
  }
  return "unknown";
}

static void write_raw_sample_row(FILE *raw_csv,
                                 const char *run_id,
                                 size_t len,
                                 const char *impl_name,
                                 const char *scenario_name,
                                 size_t outer_idx,
                                 const bench_result_t *result,
                                 const char *effective_path) {
  uint64_t total_ticks;
  uint64_t empty_ticks;
  uint64_t corrected_ticks;
  double ns_total;
  double ns_corrected;
  double ns_per_call_corrected = 0.0;
  double ns_per_byte_corrected = 0.0;

  if (!raw_csv || !run_id || !impl_name || !scenario_name || !result || !effective_path) {
    return;
  }

  total_ticks = result->samples.total_ticks[outer_idx];
  empty_ticks = result->samples.empty_ticks[outer_idx];
  corrected_ticks = result->samples.corrected_ticks[outer_idx];
  ns_total = bench_ticks_to_ns(total_ticks, result->qpc_freq);
  ns_corrected = bench_ticks_to_ns(corrected_ticks, result->qpc_freq);

  if (result->inner > 0) {
    ns_per_call_corrected = ns_corrected / (double)result->inner;
    if (len > 0) {
      ns_per_byte_corrected = ns_corrected / ((double)result->inner * (double)len);
    }
  }

  fprintf(raw_csv, "%s,%zu,%s,%s,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%s\n",
          run_id,
          len,
          impl_name,
          scenario_name,
          outer_idx,
          result->inner,
          (unsigned long long)total_ticks,
          (unsigned long long)empty_ticks,
          (unsigned long long)corrected_ticks,
          (unsigned long long)result->qpc_freq,
          ns_total,
          ns_corrected,
          ns_per_call_corrected,
          ns_per_byte_corrected,
          effective_path);
}

static void write_summary_row(FILE *summary_csv,
                              const char *run_id,
                              int key_bits,
                              size_t len,
                              const char *impl_name,
                              const char *scenario_name,
                              const char *effective_path,
                              stat_mode_t stat_mode,
                              size_t warmup_count,
                              size_t outer_count,
                              int support_ok,
                              const bench_summary_stats_t *stats) {
  if (!summary_csv || !run_id || !impl_name || !scenario_name || !effective_path || !stats) {
    return;
  }

  fprintf(summary_csv, "%s,%d,%zu,%s,%s,%s,%zu,%s,%.6f,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%zu,%d\n",
          run_id,
          key_bits,
          len,
          impl_name,
          scenario_name,
          effective_path,
          stats->n_samples,
          bench_stat_mode_name(stat_mode),
          stats->trim_ratio,
          stats->trim_count_each_side,
          stats->raw_mean_ns_per_call,
          stats->trimmed_mean_ns_per_call,
          stats->ns_per_call_p50,
          stats->standard_deviation_ns_per_call,
          stats->iqr_ns_per_call,
          stats->ns_per_call_min,
          stats->ns_per_call_max,
          stats->raw_mean_ns_per_byte,
          stats->trimmed_mean_ns_per_byte,
          warmup_count,
          outer_count,
          support_ok);
}

static const char *autotune_policy_basis_name(aria_autotune_policy_basis_t basis) {
  return (basis == ARIA_POLICY_BASIS_RAW) ? "raw" : "normalized";
}

static const char *autotune_tail_policy_name(aria_autotune_tail_policy_t tail_policy) {
  return (tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE) ? "conservative" : "native";
}

static const char *autotune_profile_name(aria_autotune_profile_t profile) {
  switch (profile) {
    case ARIA_AUTOTUNE_PROFILE_TEST:
      return "test";
    case ARIA_AUTOTUNE_PROFILE_FULL:
      return "full";
    default:
      return "unknown";
  }
}

static const char *output_level_name(aria_output_level_t output_level) {
  switch (output_level) {
    case ARIA_OUTPUT_LEVEL_DEFAULT:
      return "default";
    case ARIA_OUTPUT_LEVEL_DEBUG:
      return "debug";
    case ARIA_OUTPUT_LEVEL_RAW:
      return "raw";
    default:
      return "unknown";
  }
}

static void write_run_meta(FILE *meta_json,
                           const char *run_id,
                           const char *timestamp,
                           int argc,
                           char **argv,
                           size_t warmup,
                           size_t outer,
                           scenario_t scenario,
                           int autotune_enabled,
                           const aria_autotune_config_t *autotune_config,
                           const aria_autotune_metrics_t *autotune_metrics) {
  int argi;

  if (!meta_json || !run_id || !timestamp || !autotune_config) {
    return;
  }

  fprintf(meta_json, "{\n");
  fprintf(meta_json, "  \"run_id\": \"%s\",\n", run_id);
  fprintf(meta_json, "  \"command_line\": [");
  for (argi = 0; argi < argc; ++argi) {
    fprintf(meta_json, "%s\"%s\"", (argi == 0) ? "" : ", ", argv[argi] ? argv[argi] : "");
  }
  fprintf(meta_json, "],\n");
  fprintf(meta_json, "  \"benchmark_timestamp\": \"%s\",\n", timestamp);
  fprintf(meta_json, "  \"timestamp\": \"%s\",\n", timestamp);
  fprintf(meta_json, "  \"git_commit\": null,\n");
  fprintf(meta_json, "  \"operating_system\": \"%s\",\n", platform_name());
  fprintf(meta_json, "  \"platform\": \"%s\",\n", platform_name());
  fprintf(meta_json, "  \"cpu_model\": \"unknown\",\n");
  fprintf(meta_json, "  \"cpu_features\": { \"aesni_avx\": %s, \"aesni_avx2\": %s, \"gfni_avx512\": %s },\n",
          aria_cpu_has_aesni_avx() ? "true" : "false",
          aria_cpu_has_aesni_avx2() ? "true" : "false",
          aria_cpu_has_gfni_avx512() ? "true" : "false");
  fprintf(meta_json, "  \"compiler\": \"%s\",\n", compiler_name());
  fprintf(meta_json, "  \"compiler_version\": \"%s\",\n", compiler_version());
  fprintf(meta_json, "  \"compiler_flags\": \"unknown\",\n");
  fprintf(meta_json, "  \"build_type\": \"%s\",\n", build_type_name());
  fprintf(meta_json, "  \"warmup_count\": %zu,\n", warmup);
  fprintf(meta_json, "  \"outer_count\": %zu,\n", outer);
  fprintf(meta_json, "  \"outer_samples\": %zu,\n", outer);
  fprintf(meta_json, "  \"inner_iterations\": \"adaptive_per_measurement\",\n");
  fprintf(meta_json, "  \"trim_ratio\": %.6f,\n", autotune_config->trim_ratio);
  fprintf(meta_json, "  \"trim_count_rule\": \"floor(valid_outer_samples * trim_ratio) from each side\",\n");
  fprintf(meta_json, "  \"minimum_trimmed_samples\": %u,\n", BENCH_MIN_TRIMMED_SAMPLES);
  fprintf(meta_json, "  \"stat_mode\": \"%s\",\n", bench_stat_mode_name(STAT_TRIMMED_MEAN));
  fprintf(meta_json, "  \"empty_loop_correction\": \"per_outer_sample_before_trimming\",\n");
  fprintf(meta_json, "  \"scenario\": \"%s\",\n", aria_scenario_name(scenario));
  fprintf(meta_json, "  \"scenario_list\": [\"%s\"],\n", aria_scenario_name(scenario));
  fprintf(meta_json, "  \"autotune_enabled\": %s,\n", autotune_enabled ? "true" : "false");
  if (autotune_metrics) {
    fprintf(meta_json, "  \"autotune_search_time_ms\": %.3f,\n", autotune_metrics->search_time_ms);
    fprintf(meta_json, "  \"autotune_end_to_end_time_ms\": %.3f,\n", autotune_metrics->end_to_end_time_ms);
    fprintf(meta_json, "  \"coarse_measurement_point_count\": %zu,\n", autotune_metrics->coarse_measurement_point_count);
    fprintf(meta_json, "  \"fine_measurement_point_count\": %zu,\n", autotune_metrics->fine_measurement_point_count);
    fprintf(meta_json, "  \"autotune_candidate_measurement_count\": %zu,\n", autotune_metrics->autotune_candidate_measurement_count);
    fprintf(meta_json, "  \"exhaustive_candidate_measurement_count\": %zu,\n", autotune_metrics->exhaustive_candidate_measurement_count);
    fprintf(meta_json, "  \"measurement_reduction_percent\": %.6f,\n", autotune_metrics->measurement_reduction_percent);
  } else {
    fprintf(meta_json, "  \"autotune_search_time_ms\": null,\n");
    fprintf(meta_json, "  \"autotune_end_to_end_time_ms\": null,\n");
  }
  fprintf(meta_json, "  \"autotune_profile\": \"%s\",\n", autotune_profile_name(autotune_config->profile));
  fprintf(meta_json, "  \"output_level\": \"%s\",\n", output_level_name(autotune_config->output_level));
  fprintf(meta_json, "  \"output_dir\": \"%s\",\n",
          (autotune_config->output_dir && autotune_config->output_dir[0] != '\0') ? autotune_config->output_dir : "");
  fprintf(meta_json, "  \"output_prefix\": \"%s\",\n",
          autotune_config->output_prefix ? autotune_config->output_prefix : "");
  fprintf(meta_json, "  \"key_sizes\": [128, 192, 256],\n");
  fprintf(meta_json, "  \"message_lengths\": [16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096],\n");
  fprintf(meta_json, "  \"min_len\": %zu,\n", autotune_config->min_len);
  fprintf(meta_json, "  \"max_len\": %zu,\n", autotune_config->max_len);
  fprintf(meta_json, "  \"length_step\": 16,\n");
  fprintf(meta_json, "  \"input_lengths\": { \"benchmark\": [16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096], \"autotune_min\": %zu, \"autotune_max\": %zu },\n",
          autotune_config->min_len,
          autotune_config->max_len);
  fprintf(meta_json, "  \"coarse_grid_rule\": \"log_backbone_x1_x1.5_plus_structural_units_plus_boundaries\",\n");
  fprintf(meta_json, "  \"coarse_grid_structural_units\": [16, 256, 512, 1024],\n");
  fprintf(meta_json, "  \"implementations\": [\"ref\", \"linux_aesni_avx\", \"linux_aesni_avx2\", \"linux_gfni_avx512\"],\n");
  fprintf(meta_json, "  \"candidate_implementations\": { \"ref\": \"active\", \"linux_aesni_avx\": \"%s\", \"linux_aesni_avx2\": \"%s\", \"linux_gfni_avx512\": \"%s\" },\n",
          aria_cpu_has_aesni_avx() ? "active" : "unsupported_cpu_feature",
          aria_cpu_has_aesni_avx2() ? "active" : "unsupported_cpu_feature",
          aria_cpu_has_gfni_avx512() ? "active" : "unsupported_cpu_feature");
  fprintf(meta_json, "  \"policy_basis\": \"%s\",\n", autotune_policy_basis_name(autotune_config->policy_basis));
  fprintf(meta_json, "  \"tail_policy\": \"%s\",\n", autotune_tail_policy_name(autotune_config->tail_policy));
  fprintf(meta_json, "  \"validation_candidate_order\": \"deterministic_rotating\",\n");
  fprintf(meta_json, "  \"random_seed\": null,\n");
  fprintf(meta_json, "  \"normalization_rules\": {\n");
  fprintf(meta_json, "    \"ref_fallback\": \"%s\",\n", autotune_config->collapse_ref_fallback ? "ref" : "impl");
  fprintf(meta_json, "    \"linux_aesni_avx_plus_ref_tail\": \"%s\",\n",
          autotune_config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE ? "ref" : "linux_aesni_avx");
  fprintf(meta_json, "    \"linux_aesni_avx2_plus_ref_tail\": \"%s\",\n",
          autotune_config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE ? "ref" : "linux_aesni_avx2");
  fprintf(meta_json, "    \"linux_gfni_avx_16way\": \"linux_gfni_avx512\",\n");
  fprintf(meta_json, "    \"linux_gfni_avx2_32way\": \"linux_gfni_avx512\",\n");
  fprintf(meta_json, "    \"linux_gfni_mixed_width\": \"linux_gfni_avx512\",\n");
  fprintf(meta_json, "    \"linux_gfni_plus_ref_tail\": \"%s\",\n",
          autotune_config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE
              ? "ref"
              : "linux_gfni_avx512");
  fprintf(meta_json, "    \"mixed_effective_path\": \"ref\"\n");
  fprintf(meta_json, "  },\n");
  fprintf(meta_json, "  \"stabilization_params\": { \"winner_margin_pct\": %.2f, \"stability_min_run\": %zu, \"policy_min_bucket_points\": %zu },\n",
          autotune_config->winner_margin_pct,
          autotune_config->stability_min_run,
          autotune_config->policy_min_bucket_points);
  fprintf(meta_json, "  \"emitted_files\": {\n");
  fprintf(meta_json, "    \"results_csv\": %s,\n",
          should_emit_benchmark_output(autotune_config->output_level, BENCH_OUTPUT_RESULTS) ? "true" : "false");
  fprintf(meta_json, "    \"summary_stats_csv\": %s,\n",
          should_emit_benchmark_output(autotune_config->output_level, BENCH_OUTPUT_SUMMARY) ? "true" : "false");
  fprintf(meta_json, "    \"run_meta_json\": true,\n");
  fprintf(meta_json, "    \"keysetup_csv\": %s,\n",
          should_emit_benchmark_output(autotune_config->output_level, BENCH_OUTPUT_KEYSETUP) ? "true" : "false");
  fprintf(meta_json, "    \"raw_samples_csv\": %s\n",
          should_emit_benchmark_output(autotune_config->output_level, BENCH_OUTPUT_RAW) ? "true" : "false");
  fprintf(meta_json, "  }\n");
  fprintf(meta_json, "}\n");
}

static int parse_size_value(const char *option, const char *value, size_t *out) {
  char *endptr = NULL;
  unsigned long long parsed = 0;

  if (!option || !value || !out) {
    return 0;
  }

  errno = 0;
  parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || endptr == value || (endptr && *endptr != '\0') || parsed > SIZE_MAX) {
    fprintf(stderr, "Invalid value for %s: %s\n", option, value);
    return 0;
  }

  *out = (size_t)parsed;
  return 1;
}

static int parse_double_value(const char *option, const char *value, double *out) {
  char *endptr = NULL;
  double parsed = 0.0;

  if (!option || !value || !out) {
    return 0;
  }

  errno = 0;
  parsed = strtod(value, &endptr);
  if (errno != 0 || endptr == value || (endptr && *endptr != '\0')) {
    fprintf(stderr, "Invalid value for %s: %s\n", option, value);
    return 0;
  }

  *out = parsed;
  return 1;
}

static int parse_cli_options(int argc,
                             char **argv,
                             scenario_t *scenario,
                             int *enable_autotune,
                             aria_autotune_config_t *autotune_config) {
  int i;

  if (!scenario || !enable_autotune || !autotune_config) {
    return 0;
  }

  *scenario = SCENARIO_HIGH_PERF_SERVER;
  *enable_autotune = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--scenario") == 0) {
      if ((i + 1) >= argc) {
        fprintf(stderr, "Missing value for --scenario.\n");
        return 0;
      }
      ++i;
      if (strcmp(argv[i], "server") == 0 || strcmp(argv[i], "highperf") == 0) {
        *scenario = SCENARIO_HIGH_PERF_SERVER;
      } else if (strcmp(argv[i], "lowpower") == 0 || strcmp(argv[i], "client") == 0) {
        *scenario = SCENARIO_LOW_POWER_CLIENT;
      } else {
        fprintf(stderr, "Unknown scenario: %s\n", argv[i]);
        return 0;
      }
      continue;
    }

    if (strcmp(argv[i], "--autotune") == 0) {
      *enable_autotune = 1;
      continue;
    }

    if (strcmp(argv[i], "--policy-collapse-ref-fallback") == 0) {
      autotune_config->collapse_ref_fallback = 1;
      continue;
    }

    if (strcmp(argv[i], "--no-policy-collapse-ref-fallback") == 0) {
      autotune_config->collapse_ref_fallback = 0;
      continue;
    }

    if ((i + 1) >= argc) {
      fprintf(stderr, "Missing value for option: %s\n", argv[i]);
      return 0;
    }

    if (strcmp(argv[i], "--output-level") == 0) {
      if (strcmp(argv[i + 1], "default") == 0) {
        autotune_config->output_level = ARIA_OUTPUT_LEVEL_DEFAULT;
      } else if (strcmp(argv[i + 1], "debug") == 0) {
        autotune_config->output_level = ARIA_OUTPUT_LEVEL_DEBUG;
      } else if (strcmp(argv[i + 1], "raw") == 0) {
        autotune_config->output_level = ARIA_OUTPUT_LEVEL_RAW;
      } else {
        fprintf(stderr, "Unknown output level: %s\n", argv[i + 1]);
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--output-dir") == 0 || strcmp(argv[i], "--autotune-output-dir") == 0) {
      autotune_config->output_dir = argv[i + 1];
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--autotune-profile") == 0) {
      if (strcmp(argv[i + 1], "test") == 0) {
        aria_autotune_config_apply_profile(autotune_config, ARIA_AUTOTUNE_PROFILE_TEST);
      } else if (strcmp(argv[i + 1], "full") == 0) {
        aria_autotune_config_apply_profile(autotune_config, ARIA_AUTOTUNE_PROFILE_FULL);
      } else {
        fprintf(stderr, "Unknown autotune profile: %s\n", argv[i + 1]);
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--autotune-min-len") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->min_len)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--autotune-max-len") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->max_len)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--refine-step") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->refine_step)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--coarse-iterations") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->coarse_iterations)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--refine-iterations") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->refine_iterations)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--winner-margin-pct") == 0) {
      if (!parse_double_value(argv[i], argv[i + 1], &autotune_config->winner_margin_pct)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--trim-ratio") == 0) {
      if (!parse_double_value(argv[i], argv[i + 1], &autotune_config->trim_ratio)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--stability-min-run") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->stability_min_run)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--policy-min-bucket-points") == 0) {
      if (!parse_size_value(argv[i], argv[i + 1], &autotune_config->policy_min_bucket_points)) {
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--policy-basis") == 0) {
      if (strcmp(argv[i + 1], "raw") == 0) {
        autotune_config->policy_basis = ARIA_POLICY_BASIS_RAW;
      } else if (strcmp(argv[i + 1], "normalized") == 0) {
        autotune_config->policy_basis = ARIA_POLICY_BASIS_NORMALIZED;
      } else {
        fprintf(stderr, "Unknown policy basis: %s\n", argv[i + 1]);
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--tail-policy") == 0) {
      if (strcmp(argv[i + 1], "native") == 0) {
        autotune_config->tail_policy = ARIA_TAIL_POLICY_NATIVE;
      } else if (strcmp(argv[i + 1], "conservative") == 0) {
        autotune_config->tail_policy = ARIA_TAIL_POLICY_CONSERVATIVE;
      } else {
        fprintf(stderr, "Unknown tail policy: %s\n", argv[i + 1]);
        return 0;
      }
      ++i;
      continue;
    }
    if (strcmp(argv[i], "--autotune-output-prefix") == 0) {
      autotune_config->output_prefix = argv[i + 1];
      ++i;
      continue;
    }

    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    return 0;
  }

  if (autotune_config->trim_ratio < 0.0 || autotune_config->trim_ratio >= 0.5 ||
      autotune_config->trim_ratio != autotune_config->trim_ratio) {
    fprintf(stderr, "Invalid --trim-ratio: must be finite and in [0, 0.5).\n");
    return 0;
  }

  return 1;
}

int main(int argc, char **argv) {
  static const size_t lengths[] = {
    16, 32, 64, 128, 192, 256, 320, 512, 1024, 2048, 4096
  };
  const size_t lengths_count = sizeof(lengths) / sizeof(lengths[0]);
  const size_t warmup = 200;
  const size_t outer = 21;
  const size_t inner_max = (size_t)(1u << 20);
  const size_t buffer_size = 1024 * 1024;
  const stat_mode_t stat_mode = STAT_TRIMMED_MEAN;
  uint64_t target_ticks = 0;
  scenario_t scenario = SCENARIO_HIGH_PERF_SERVER;
  int enable_autotune = 0;
  aria_autotune_config_t autotune_config;
  char run_id[64];
  char benchmark_timestamp[64];
  char key_csv_path[512];
  char csv_path[512];
  char raw_csv_path[512];
  char summary_csv_path[512];
  char meta_json_path[512];
  int exit_code = 0;

  FILE *key_csv = NULL;
  FILE *csv = NULL;
  FILE *raw_csv = NULL;
  FILE *summary_csv = NULL;
  FILE *meta_json = NULL;

  aria_autotune_config_init(&autotune_config);
  if (!parse_cli_options(argc, argv, &scenario, &enable_autotune, &autotune_config)) {
    return 1;
  }

  uint8_t *input = (uint8_t *)malloc(buffer_size);
  uint8_t *output = (uint8_t *)malloc(buffer_size);
  if (!input || !output) {
    fprintf(stderr, "Failed to allocate buffers.\n");
    free(input);
    free(output);
    return 1;
  }

  for (size_t i = 0; i < buffer_size; ++i) {
    input[i] = (uint8_t)(i & 0xFFu);
  }

  aria_ctx_t ctx;
  uint8_t key[32] = {0};
  const int keybits = 128;
  aria_ref_impl.init(&ctx, key, keybits);

#if defined(_WIN32)
  {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    target_ticks = (uint64_t)((freq.QuadPart * 10) / 1000);
  }
#else
  {
    const uint64_t freq = time_frequency();
    target_ticks = (freq > 0) ? (uint64_t)((freq * 10) / 1000) : 0;
  }
#endif

  make_run_id(run_id, sizeof(run_id));
  make_timestamp(benchmark_timestamp, sizeof(benchmark_timestamp));

  if (should_emit_benchmark_output(autotune_config.output_level, BENCH_OUTPUT_KEYSETUP)) {
    if (!build_benchmark_output_path(key_csv_path, sizeof(key_csv_path), &autotune_config, "keysetup.csv")) {
      fprintf(stderr, "Failed to build keysetup output path.\n");
      free(input);
      free(output);
      return 1;
    }
    key_csv = fopen(key_csv_path, "w");
    if (!key_csv) {
      fprintf(stderr, "Failed to open %s.\n", key_csv_path);
      free(input);
      free(output);
      return 1;
    }
    fprintf(key_csv, "keybits,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,stat_mode,sink,scenario\n");
  }

  if (should_emit_benchmark_output(autotune_config.output_level, BENCH_OUTPUT_RESULTS)) {
    if (!build_benchmark_output_path(csv_path, sizeof(csv_path), &autotune_config, "results.csv")) {
      fprintf(stderr, "Failed to build results output path.\n");
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    csv = fopen(csv_path, "w");
    if (!csv) {
      fprintf(stderr, "Failed to open %s.\n", csv_path);
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    fprintf(csv, "len,impl,effective_path,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,ns_per_byte,ns_per_byte_corrected,stat_mode,sink,scenario\n");
  }

  if (should_emit_benchmark_output(autotune_config.output_level, BENCH_OUTPUT_RAW)) {
    if (!build_benchmark_output_path(raw_csv_path, sizeof(raw_csv_path), &autotune_config, "raw_samples.csv")) {
      fprintf(stderr, "Failed to build raw samples output path.\n");
      close_file_if_open(&csv);
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    raw_csv = fopen(raw_csv_path, "w");
    if (!raw_csv) {
      fprintf(stderr, "Failed to open %s.\n", raw_csv_path);
      close_file_if_open(&csv);
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    fprintf(raw_csv, "run_id,len,impl,scenario,outer_idx,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_corrected,ns_per_call_corrected,ns_per_byte_corrected,effective_path\n");
  }

  if (should_emit_benchmark_output(autotune_config.output_level, BENCH_OUTPUT_SUMMARY)) {
    if (!build_benchmark_output_path(summary_csv_path, sizeof(summary_csv_path), &autotune_config, "summary_stats.csv")) {
      fprintf(stderr, "Failed to build summary output path.\n");
      close_file_if_open(&raw_csv);
      close_file_if_open(&csv);
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    summary_csv = fopen(summary_csv_path, "w");
    if (!summary_csv) {
      fprintf(stderr, "Failed to open %s.\n", summary_csv_path);
      close_file_if_open(&raw_csv);
      close_file_if_open(&csv);
      close_file_if_open(&key_csv);
      free(input);
      free(output);
      return 1;
    }
    fprintf(summary_csv, "run_id,key_bits,len,impl,scenario,effective_path,valid_outer_samples,stat_mode,trim_ratio,trim_count_each_side,raw_mean_ns_per_call,trimmed_mean_ns_per_call,median_ns_per_call,standard_deviation_ns_per_call,iqr_ns_per_call,min_ns_per_call,max_ns_per_call,raw_mean_ns_per_byte,trimmed_mean_ns_per_byte,warmup_iterations,outer_samples_requested,support_ok\n");
  }

  if (!build_benchmark_output_path(meta_json_path, sizeof(meta_json_path), &autotune_config, "run_meta.json")) {
    fprintf(stderr, "Failed to build run_meta output path.\n");
    close_file_if_open(&summary_csv);
    close_file_if_open(&raw_csv);
    close_file_if_open(&csv);
    close_file_if_open(&key_csv);
    free(input);
    free(output);
    return 1;
  }
  meta_json = fopen(meta_json_path, "w");
  if (!meta_json) {
    fprintf(stderr, "Failed to open %s.\n", meta_json_path);
    close_file_if_open(&summary_csv);
    close_file_if_open(&raw_csv);
    close_file_if_open(&csv);
    close_file_if_open(&key_csv);
    free(input);
    free(output);
    return 1;
  }
  write_run_meta(meta_json,
                 run_id,
                 benchmark_timestamp,
                 argc,
                 argv,
                 warmup,
                 outer,
                 scenario,
                 enable_autotune,
                 &autotune_config,
                 NULL);

  {
    const aria_impl_t *impls[] = {
      &aria_ref_impl,
      &aria_linux_aesni_avx_impl,
      &aria_linux_aesni_avx2_impl,
      &aria_linux_gfni_avx512_impl
    };
    const size_t impls_count = sizeof(impls) / sizeof(impls[0]);
    uint64_t final_sink = 0;

    printf("[INFO] scenario=%s\n", aria_scenario_name(scenario));
    for (size_t k = 0; k < impls_count; ++k) {
      if (impls[k]->is_supported && !impls[k]->is_supported()) {
        printf("[INFO] skipping_impl=%s (unsupported)\n", impls[k]->name);
        continue;
      }
      printf("[INFO] will_measure_impl=%s\n", impls[k]->name);
    }

    {
      bench_result_t keysetup_result = bench_run_keysetup(aria_ref_impl.init,
                                                          &ctx,
                                                          key,
                                                          keybits,
                                                          outer,
                                                          target_ticks,
                                                          inner_max,
                                                          stat_mode,
                                                          warmup,
                                                          autotune_config.trim_ratio);
      final_sink ^= keysetup_result.sink;
      if (key_csv) {
        fprintf(key_csv, "%d,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%s,%llu,%s\n",
                keybits,
                keysetup_result.outer,
                keysetup_result.inner,
                (unsigned long long)keysetup_result.total_ticks,
                (unsigned long long)keysetup_result.empty_ticks,
                (unsigned long long)keysetup_result.corrected_ticks,
                (unsigned long long)keysetup_result.qpc_freq,
                keysetup_result.ns_total,
                keysetup_result.ns_per_call,
                bench_stat_mode_name(keysetup_result.stat_mode),
                (unsigned long long)keysetup_result.sink,
                aria_scenario_name(scenario));
      }
      bench_result_cleanup(&keysetup_result);
      memset(key, 0, sizeof(key));
      aria_ref_impl.init(&ctx, key, keybits);
    }

    for (size_t k = 0; k < impls_count; ++k) {
      const aria_impl_t *impl = impls[k];
      if (impl->is_supported && !impl->is_supported()) {
        continue;
      }

      impl->init(&ctx, key, keybits);

      for (size_t i = 0; i < lengths_count; ++i) {
        const size_t len = lengths[i];
        const char *effective_path = effective_path_for(impl, len);
        const char *scenario_name = aria_scenario_name(scenario);
        bench_result_t result = bench_run(impl->encrypt,
                                          &ctx,
                                          input,
                                          output,
                                          len,
                                          outer,
                                          target_ticks,
                                          inner_max,
                                          stat_mode,
                                          warmup,
                                          autotune_config.trim_ratio);
        bench_summary_stats_t stats = bench_compute_summary_stats(&result,
                                                                  autotune_config.trim_ratio);
        final_sink ^= result.sink;

        if (csv) {
          fprintf(csv, "%zu,%s,%s,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%s,%llu,%s\n",
                  len,
                  impl->name,
                  effective_path,
                  result.outer,
                  result.inner,
                  (unsigned long long)result.total_ticks,
                  (unsigned long long)result.empty_ticks,
                  (unsigned long long)result.corrected_ticks,
                  (unsigned long long)result.qpc_freq,
                  result.ns_total,
                  result.ns_per_call,
                  result.ns_per_byte,
                  result.ns_per_byte_corrected,
                  bench_stat_mode_name(result.stat_mode),
                  (unsigned long long)result.sink,
                  scenario_name);
        }

        for (size_t sample_idx = 0; sample_idx < result.samples.count; ++sample_idx) {
          write_raw_sample_row(raw_csv, run_id, len, impl->name, scenario_name, sample_idx, &result, effective_path);
        }
        write_summary_row(summary_csv,
                          run_id,
                          keybits,
                          len,
                          impl->name,
                          scenario_name,
                          effective_path,
                          result.stat_mode,
                          warmup,
                          outer,
                          1,
                          &stats);
        bench_result_cleanup(&result);
      }
    }

    close_file_if_open(&meta_json);
    close_file_if_open(&summary_csv);
    close_file_if_open(&raw_csv);
    close_file_if_open(&key_csv);
    close_file_if_open(&csv);

    if (enable_autotune) {
      printf("[INFO] autotune enabled, output_prefix=%s output_dir=%s output_level=%s profile=%s basis=%s tail_policy=%s collapse_ref_fallback=%s margin=%.2f min_run=%zu min_bucket=%zu range=%zu..%zu\n",
             autotune_config.output_prefix,
             (autotune_config.output_dir && autotune_config.output_dir[0] != '\0') ? autotune_config.output_dir : "",
             output_level_name(autotune_config.output_level),
             autotune_profile_name(autotune_config.profile),
             autotune_policy_basis_name(autotune_config.policy_basis),
             autotune_tail_policy_name(autotune_config.tail_policy),
             autotune_config.collapse_ref_fallback ? "on" : "off",
             autotune_config.winner_margin_pct,
             autotune_config.stability_min_run,
             autotune_config.policy_min_bucket_points,
             autotune_config.min_len,
             autotune_config.max_len);
      if (!aria_autotune_run(&autotune_config,
                             input,
                             output,
                             buffer_size,
                             target_ticks,
                             inner_max,
                             warmup,
                             stat_mode)) {
        fprintf(stderr, "Failed to complete autotune run.\n");
        exit_code = 1;
      } else if (!aria_run_dispatch_evaluation(&autotune_config,
                                                input,
                                                output,
                                                buffer_size,
                                                target_ticks,
                                                inner_max,
                                                warmup,
                                                outer,
                                                stat_mode,
                                                aria_autotune_last_duration_ms(),
                                                scenario)) {
        fprintf(stderr, "Failed to complete dispatch evaluation.\n");
        exit_code = 1;
      }
    }

    if (enable_autotune && aria_policy_count() > 0) {
      meta_json = fopen(meta_json_path, "w");
      if (!meta_json) {
        fprintf(stderr, "Failed to update %s with autotune metrics.\n", meta_json_path);
        exit_code = 1;
      } else {
        write_run_meta(meta_json,
                       run_id,
                       benchmark_timestamp,
                       argc,
                       argv,
                       warmup,
                       outer,
                       scenario,
                       enable_autotune,
                       &autotune_config,
                       aria_autotune_last_metrics());
        close_file_if_open(&meta_json);
      }
    }

    printf("sink=%llu\n", (unsigned long long)final_sink);
  }

  free(input);
  free(output);
  return exit_code;
}
