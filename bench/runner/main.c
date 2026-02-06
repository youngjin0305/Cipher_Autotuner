#include "bench_measure.h"
#include "runtime_dispatch.h"
#include "aria_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static void ensure_out_dir(void) {
#if defined(_WIN32)
  _mkdir("out");
#else
  mkdir("out", 0755);
#endif
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

  const aria_impl_t *impl = aria_runtime_dispatch_scenario(0, scenario);
  if (!impl || !impl->encrypt) {
    fprintf(stderr, "No ARIA implementation available.\n");
    free(input);
    free(output);
    return 1;
  }

  aria_ctx_t ctx;
  uint8_t key[32] = {0};
  const int keybits = 128;
  aria_init(&ctx, key, keybits);

  printf("[INFO] scenario=%s selected_impl=%s\n",
         aria_scenario_name(scenario),
         impl->name);

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

  fprintf(key_csv, "keybits,impl,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,stat_mode,sink,scenario\n");
  fprintf(csv, "len,impl,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,ns_per_byte,ns_per_byte_corrected,stat_mode,sink,scenario\n");

  uint64_t final_sink = 0;
  bench_result_t keysetup_result = bench_run_keysetup(aria_init,
                                                      &ctx,
                                                      key,
                                                      keybits,
                                                      outer,
                                                      target_ticks,
                                                      inner_max,
                                                      stat_mode,
                                                      warmup);
  final_sink ^= keysetup_result.sink;
  fprintf(key_csv, "%d,%s,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%s,%llu,%s\n",
          keybits,
          impl->name,
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
  memset(key, 0, sizeof(key));
  aria_init(&ctx, key, keybits);

  for (size_t i = 0; i < lengths_count; ++i) {
    const size_t len = lengths[i];
    bench_result_t result = bench_run(impl->encrypt,
                                      &ctx,
                                      input,
                                      output,
                                      len,
                                      outer,
                                      target_ticks,
                                      inner_max,
                                      stat_mode,
                                      warmup);
    final_sink ^= result.sink;

    fprintf(csv, "%zu,%s,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%s,%llu,%s\n",
            len,
            impl->name,
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
            aria_scenario_name(scenario));
  }

  fclose(key_csv);
  fclose(csv);
  printf("sink=%llu\n", (unsigned long long)final_sink);
  free(input);
  free(output);
  return 0;
}
