#include "bench_measure.h"
#include "runtime_dispatch.h"
#include "aria_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

typedef struct summary_stats {
  size_t n_samples;
  double ns_per_call_mean;
  double ns_per_call_trimmed_mean;
  double ns_per_call_p50;
  double ns_per_call_p95;
  double ns_per_call_p99;
  double ns_per_byte_mean_corrected;
  double ns_per_byte_trimmed_mean_corrected;
} summary_stats_t;

#define BENCH_STRINGIFY_IMPL(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_IMPL(x)

static void ensure_out_dir(void) {
#if defined(_WIN32)
  _mkdir("out");
#else
  mkdir("out", 0755);
#endif
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

static const char *build_type_name(void) {
#if defined(NDEBUG)
  return "Release";
#else
  return "Debug";
#endif
}

static const char *compiler_info(void) {
#if defined(_MSC_FULL_VER)
  return "MSVC " BENCH_STRINGIFY(_MSC_FULL_VER);
#elif defined(_MSC_VER)
  return "MSVC";
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

static const char *effective_path_for(const char *impl_name, size_t len) {
  if (!impl_name) {
    return "unknown";
  }
  if (strcmp(impl_name, "ref") == 0) {
    return "ref";
  }
  if (strcmp(impl_name, "avx2") == 0) {
    if (len < 64) {
      return "ref_fallback";
    }
    if ((len % 64) == 0) {
      return "avx2_4way";
    }
    return "avx2_4way_plus_ref_tail";
  }
  return impl_name;
}

static double percentile_from_sorted(const double *sorted, size_t count, double pct) {
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

static double mean_of_samples(const double *samples, size_t count) {
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

static double trimmed_mean_of_samples(const double *samples, size_t count) {
  double *sorted;
  double sum = 0.0;
  size_t start = 0;
  size_t end = count;
  size_t i;

  if (!samples || count == 0) {
    return 0.0;
  }

  sorted = (double *)malloc(sizeof(double) * count);
  if (!sorted) {
    return samples[0];
  }
  for (i = 0; i < count; ++i) {
    sorted[i] = samples[i];
  }
  qsort(sorted, count, sizeof(double), cmp_double);
  bench_trim_bounds(count, &start, &end);
  for (i = start; i < end; ++i) {
    sum += sorted[i];
  }
  free(sorted);
  return sum / (double)(end - start);
}

static summary_stats_t compute_summary_stats(const bench_result_t *result) {
  summary_stats_t stats;
  double *per_call = NULL;
  double *per_byte = NULL;
  double *per_call_sorted = NULL;
  size_t i;
  size_t count = 0;

  memset(&stats, 0, sizeof(stats));
  if (!result || result->samples.count == 0 || result->inner == 0 || result->qpc_freq == 0) {
    return stats;
  }

  count = result->samples.count;
  per_call = (double *)malloc(sizeof(double) * count);
  per_call_sorted = (double *)malloc(sizeof(double) * count);
  if (result->len > 0) {
    per_byte = (double *)malloc(sizeof(double) * count);
  }
  if (!per_call || !per_call_sorted || (result->len > 0 && !per_byte)) {
    free(per_call);
    free(per_call_sorted);
    free(per_byte);
    return stats;
  }

  for (i = 0; i < count; ++i) {
    const double ns_corrected = bench_ticks_to_ns(result->samples.corrected_ticks[i], result->qpc_freq);
    per_call[i] = ns_corrected / (double)result->inner;
    per_call_sorted[i] = per_call[i];
    if (per_byte) {
      per_byte[i] = ns_corrected / ((double)result->inner * (double)result->len);
    }
  }

  qsort(per_call_sorted, count, sizeof(double), cmp_double);

  stats.n_samples = count;
  stats.ns_per_call_mean = mean_of_samples(per_call, count);
  stats.ns_per_call_trimmed_mean = trimmed_mean_of_samples(per_call, count);
  stats.ns_per_call_p50 = percentile_from_sorted(per_call_sorted, count, 0.50);
  stats.ns_per_call_p95 = percentile_from_sorted(per_call_sorted, count, 0.95);
  stats.ns_per_call_p99 = percentile_from_sorted(per_call_sorted, count, 0.99);
  if (per_byte) {
    stats.ns_per_byte_mean_corrected = mean_of_samples(per_byte, count);
    stats.ns_per_byte_trimmed_mean_corrected = trimmed_mean_of_samples(per_byte, count);
  }

  free(per_call);
  free(per_call_sorted);
  free(per_byte);
  return stats;
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
                              size_t len,
                              const char *impl_name,
                              const char *scenario_name,
                              const char *effective_path,
                              stat_mode_t stat_mode,
                              const summary_stats_t *stats) {
  if (!summary_csv || !run_id || !impl_name || !scenario_name || !effective_path || !stats) {
    return;
  }

  fprintf(summary_csv, "%s,%zu,%s,%s,%s,%zu,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
          run_id,
          len,
          impl_name,
          scenario_name,
          effective_path,
          stats->n_samples,
          bench_stat_mode_name(stat_mode),
          stats->ns_per_call_mean,
          stats->ns_per_call_trimmed_mean,
          stats->ns_per_call_p50,
          stats->ns_per_call_p95,
          stats->ns_per_call_p99,
          stats->ns_per_byte_mean_corrected,
          stats->ns_per_byte_trimmed_mean_corrected);
}

static void write_run_meta(FILE *meta_json,
                           const char *run_id,
                           const char *timestamp,
                           size_t warmup,
                           size_t outer,
                           scenario_t scenario) {
  size_t trim_percent = (size_t)(BENCH_TRIM_RATIO * 100.0);

  if (!meta_json || !run_id || !timestamp) {
    return;
  }

  fprintf(meta_json, "{\n");
  fprintf(meta_json, "  \"run_id\": \"%s\",\n", run_id);
  fprintf(meta_json, "  \"benchmark_timestamp\": \"%s\",\n", timestamp);
  fprintf(meta_json, "  \"platform\": \"%s\",\n", platform_name());
  fprintf(meta_json, "  \"compiler\": \"%s\",\n", compiler_info());
  fprintf(meta_json, "  \"build_type\": \"%s\",\n", build_type_name());
  fprintf(meta_json, "  \"warmup_count\": %zu,\n", warmup);
  fprintf(meta_json, "  \"outer_count\": %zu,\n", outer);
  fprintf(meta_json, "  \"trim_ratio\": %.2f,\n", BENCH_TRIM_RATIO);
  fprintf(meta_json, "  \"trim_percent\": %zu,\n", trim_percent);
  fprintf(meta_json, "  \"scenario_list\": [\"%s\"]\n", aria_scenario_name(scenario));
  fprintf(meta_json, "}\n");
}

static scenario_t parse_scenario(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--scenario") == 0 && (i + 1) < argc) {
      const char *value = argv[i + 1];
      if (strcmp(value, "server") == 0 || strcmp(value, "highperf") == 0) {
        return SCENARIO_HIGH_PERF_SERVER;
      }
      if (strcmp(value, "lowpower") == 0 || strcmp(value, "client") == 0) {
        return SCENARIO_LOW_POWER_CLIENT;
      }
    }
  }
  return SCENARIO_HIGH_PERF_SERVER;
}

int main(int argc, char **argv) {
  static const size_t lengths[] = {
    16, 32, 64, 128, 192, 256, 320, 512, 1024, 4096, 16384
  };
  const size_t lengths_count = sizeof(lengths) / sizeof(lengths[0]);
  const size_t warmup = 200;
  const size_t outer = 21;
  const size_t inner_max = (size_t)(1u << 20);
  const size_t buffer_size = 1024 * 1024;
  const stat_mode_t stat_mode = STAT_TRIMMED_MEAN;
  uint64_t target_ticks = 0;
  const scenario_t scenario = parse_scenario(argc, argv);
  char run_id[64];
  char benchmark_timestamp[64];

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

  // const aria_impl_t *impl = aria_runtime_dispatch_scenario(0, scenario);
  // if (!impl || !impl->encrypt) {
  //   fprintf(stderr, "No ARIA implementation available.\n");
  //   free(input);
  //   free(output);
  //   return 1;
  // }
  // printf("[INFO] scenario=%s selected_impl=%s\n", aria_scenario_name(scenario), impl->name);

  aria_ctx_t ctx;
  uint8_t key[32] = {0};
  const int keybits = 128;
  aria_init(&ctx, key, keybits);

#if defined(_WIN32)
  {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    target_ticks = (uint64_t)((freq.QuadPart * 10) / 1000);
  }
#else
  target_ticks = 0;
#endif

  ensure_out_dir();
  make_run_id(run_id, sizeof(run_id));
  make_timestamp(benchmark_timestamp, sizeof(benchmark_timestamp));

  FILE *key_csv = fopen("out/keysetup.csv", "w");
  if (!key_csv) {
    fprintf(stderr, "Failed to open out/keysetup.csv.\n");
    free(input);
    free(output);
    return 1;
  }

  FILE *csv = fopen("out/results.csv", "w");
  if (!csv) {
    fprintf(stderr, "Failed to open out/results.csv.\n");
    fclose(key_csv);
    free(input);
    free(output);
    return 1;
  }

  FILE *raw_csv = fopen("out/raw_samples.csv", "w");
  if (!raw_csv) {
    fprintf(stderr, "Failed to open out/raw_samples.csv.\n");
    fclose(csv);
    fclose(key_csv);
    free(input);
    free(output);
    return 1;
  }

  FILE *summary_csv = fopen("out/summary_stats.csv", "w");
  if (!summary_csv) {
    fprintf(stderr, "Failed to open out/summary_stats.csv.\n");
    fclose(raw_csv);
    fclose(csv);
    fclose(key_csv);
    free(input);
    free(output);
    return 1;
  }

  FILE *meta_json = fopen("out/run_meta.json", "w");
  if (!meta_json) {
    fprintf(stderr, "Failed to open out/run_meta.json.\n");
    fclose(summary_csv);
    fclose(raw_csv);
    fclose(csv);
    fclose(key_csv);
    free(input);
    free(output);
    return 1;
  }

  fprintf(key_csv, "keybits,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,stat_mode,sink,scenario\n");
  fprintf(csv, "len,impl,effective_path,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,ns_per_byte,ns_per_byte_corrected,stat_mode,sink,scenario\n");
  fprintf(raw_csv, "run_id,len,impl,scenario,outer_idx,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_corrected,ns_per_call_corrected,ns_per_byte_corrected,effective_path\n");
  fprintf(summary_csv, "run_id,len,impl,scenario,effective_path,n_samples,stat_mode,ns_per_call_mean,ns_per_call_trimmed_mean,ns_per_call_p50,ns_per_call_p95,ns_per_call_p99,ns_per_byte_mean_corrected,ns_per_byte_trimmed_mean_corrected\n");
  write_run_meta(meta_json, run_id, benchmark_timestamp, warmup, outer, scenario);

  const aria_impl_t *impls[] = { &aria_ref_impl, &aria_avx2_impl };
  const size_t impls_count = sizeof(impls) / sizeof(impls[0]);

  printf("[INFO] scenario=%s\n", aria_scenario_name(scenario));
  for (size_t k = 0; k < impls_count; ++k) {
    printf("[INFO] will_measure_impl=%s\n", impls[k]->name);
  }
  
  uint64_t final_sink = 0;
  bench_result_t keysetup_result = bench_run_keysetup(aria_init, &ctx, key, keybits, outer, target_ticks, inner_max, stat_mode, warmup);
  final_sink ^= keysetup_result.sink;
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
  bench_result_cleanup(&keysetup_result);
  memset(key, 0, sizeof(key));
  aria_init(&ctx, key, keybits);
    
  for (size_t k = 0; k < impls_count; ++k) {
    const aria_impl_t *impl = impls[k];

    for (size_t i = 0; i < lengths_count; ++i) {
      const size_t len = lengths[i];
      const char *effective_path = effective_path_for(impl->name, len);
      const char *scenario_name = aria_scenario_name(scenario);
      bench_result_t result = bench_run(impl->encrypt, &ctx, input, output, len, outer, target_ticks, inner_max, stat_mode, warmup);
      summary_stats_t stats = compute_summary_stats(&result);
      final_sink ^= result.sink;

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

      for (size_t sample_idx = 0; sample_idx < result.samples.count; ++sample_idx) {
        write_raw_sample_row(raw_csv, run_id, len, impl->name, scenario_name, sample_idx, &result, effective_path);
      }
      write_summary_row(summary_csv, run_id, len, impl->name, scenario_name, effective_path, result.stat_mode, &stats);
      bench_result_cleanup(&result);
    }
  }

  fclose(meta_json);
  fclose(summary_csv);
  fclose(raw_csv);
  fclose(key_csv);
  fclose(csv);
  printf("sink=%llu\n", (unsigned long long)final_sink);
  free(input);
  free(output);
  return 0;
}
