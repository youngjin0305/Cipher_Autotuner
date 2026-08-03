#include "bench_measure.h"

#include <math.h>
#include <stdio.h>

static int expect_mean(const double *samples,
                       size_t count,
                       double trim_ratio,
                       int expected_success,
                       double expected_mean,
                       size_t expected_trim) {
  double actual = 0.0;
  size_t trim = 0;
  const int success = bench_compute_trimmed_mean(samples,
                                                 count,
                                                 trim_ratio,
                                                 BENCH_MIN_TRIMMED_SAMPLES,
                                                 &actual,
                                                 &trim);
  if (success != expected_success ||
      (success && (fabs(actual - expected_mean) > 1e-12 || trim != expected_trim))) {
    fprintf(stderr,
            "trimmed-mean test failed: success=%d mean=%.12f trim=%zu\n",
            success,
            actual,
            trim);
    return 0;
  }
  return 1;
}

int main(void) {
  const double normal[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  const double outliers[] = {100, 2, 3, 4, 5, 6, 7, 8, 9, 0};
  const double odd[] = {5, 1, 4, 2, 3};
  const double even[] = {6, 1, 5, 2, 4, 3};
  const double no_trim[] = {3, 1, 2};
  const double insufficient[] = {1, 2, 3, 4};
  const double nan_values[] = {1, NAN, 3, 4, 5};
  const double infinite_values[] = {1, 2, INFINITY, 4, 5};

  if (!expect_mean(normal, 10, 0.10, 1, 5.5, 1) ||
      !expect_mean(outliers, 10, 0.10, 1, 5.5, 1) ||
      !expect_mean(odd, 5, 0.20, 1, 3.0, 1) ||
      !expect_mean(even, 6, 0.20, 1, 3.5, 1) ||
      !expect_mean(no_trim, 3, 0.0, 1, 2.0, 0) ||
      !expect_mean(insufficient, 4, 0.25, 0, 0.0, 0) ||
      !expect_mean(nan_values, 5, 0.0, 0, 0.0, 0) ||
      !expect_mean(infinite_values, 5, 0.0, 0, 0.0, 0)) {
    return 1;
  }

  printf("[OK] common trimmed-mean statistics\n");
  return 0;
}
