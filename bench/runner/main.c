#include "common_time.h"
#include "runtime_dispatch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
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
  const size_t repeats = 2000;
  const size_t buffer_size = 1024 * 1024;

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

  ensure_out_dir();

  FILE *csv = fopen("out/results.csv", "w");
  if (!csv) {
    fprintf(stderr, "Failed to open out/results.csv.\n");
    free(input);
    free(output);
    return 1;
  }

  fprintf(csv, "len,impl,warmup,repeats,cycles,cycles_per_byte\n");

  for (size_t i = 0; i < lengths_count; ++i) {
    const size_t len = lengths[i];
    uint64_t best_cycles = UINT64_MAX;

    for (size_t w = 0; w < warmup; ++w) {
      impl->encrypt(NULL, input, output, len);
    }

    for (size_t r = 0; r < repeats; ++r) {
      uint64_t start = time_begin();
      impl->encrypt(NULL, input, output, len);
      uint64_t cycles = time_end(start);
      if (cycles < best_cycles) {
        best_cycles = cycles;
      }
    }

    double cycles_per_byte = 0.0;
    if (len > 0) {
      cycles_per_byte = (double)best_cycles / (double)len;
    }

    fprintf(csv, "%zu,%s,%zu,%zu,%llu,%.6f\n",
            len,
            impl->name,
            warmup,
            repeats,
            (unsigned long long)best_cycles,
            cycles_per_byte);
  }

  fclose(csv);
  free(input);
  free(output);
  return 0;
}
