#include "bench_measure.h"
#include "runtime_dispatch.h"

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

int main(void) {
  static const size_t lengths[] = {
    16, 32, 64, 128, 192, 256, 320, 512, 1024, 4096, 16384
  };
  const size_t lengths_count = sizeof(lengths) / sizeof(lengths[0]);
  const size_t warmup = 200;
  const size_t outer = 21;
  const size_t inner_max = (size_t)(1u << 20);
  const size_t buffer_size = 1024 * 1024;
  const stat_mode_t stat_mode = STAT_MEDIAN;
  uint64_t target_ticks = 0;

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

  const aria_impl_t *impl = aria_runtime_dispatch(0);
  if (!impl || !impl->encrypt) {
    fprintf(stderr, "No ARIA implementation available.\n");
    free(input);
    free(output);
    return 1;
  }

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

  FILE *csv = fopen("out/results.csv", "w");
  if (!csv) {
    fprintf(stderr, "Failed to open out/results.csv.\n");
    free(input);
    free(output);
    return 1;
  }

  fprintf(csv, "len,impl,outer,inner,total_ticks,empty_ticks,corrected_ticks,qpc_freq,ns_total,ns_per_call,ns_per_byte,ns_per_byte_corrected,stat_mode,sink\n");

  uint64_t final_sink = 0;

  for (size_t i = 0; i < lengths_count; ++i) {
    const size_t len = lengths[i];
    bench_result_t result = bench_run(impl->encrypt,
                                      NULL,
                                      input,
                                      output,
                                      len,
                                      outer,
                                      target_ticks,
                                      inner_max,
                                      stat_mode,
                                      warmup);
    final_sink ^= result.sink;

    fprintf(csv, "%zu,%s,%zu,%zu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%s,%llu\n",
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
            (unsigned long long)result.sink);
  }

  fclose(csv);
  printf("sink=%llu\n", (unsigned long long)final_sink);
  free(input);
  free(output);
  return 0;
}
