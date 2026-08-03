#include "policy_metrics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char *message) {
  if (!condition) fprintf(stderr, "[FAIL] %s\n", message);
  return condition;
}

static int test_ranges(const size_t *lengths,
                       const int *matches,
                       size_t count,
                       const char *expected_ranges,
                       size_t points,
                       size_t intervals,
                       size_t longest) {
  char ranges[128];
  policy_mismatch_summary_t summary;
  return policy_summarize_mismatches(lengths, matches, count, 16, ranges, sizeof(ranges), &summary) &&
         strcmp(ranges, expected_ranges) == 0 &&
         summary.mismatched_point_count == points &&
         summary.mismatched_interval_count == intervals &&
         summary.longest_mismatched_interval_bytes == longest;
}

int main(void) {
  const size_t boundaries_a[] = {256, 512};
  const size_t boundaries_b[] = {240, 544};
  const size_t lengths[] = {16, 32, 48, 64, 80};
  const int none[] = {1, 1, 1, 1, 1};
  const int single[] = {1, 1, 0, 1, 1};
  const int continuous[] = {1, 0, 0, 0, 1};
  const int separated[] = {0, 1, 0, 1, 0};
  const int endpoints[] = {0, 1, 1, 1, 0};

  if (!expect(fabs(policy_mean_symmetric_boundary_distance(boundaries_a, 2,
                                                           boundaries_b, 2, 1000) - 24.0) < 1e-12,
              "symmetric boundary distance") ||
      !expect(policy_mean_symmetric_boundary_distance(NULL, 0, NULL, 0, 1000) == 0.0,
              "empty boundary sets") ||
      !expect(policy_mean_symmetric_boundary_distance(boundaries_a, 2, NULL, 0, 1000) == 1000.0,
              "one empty boundary set") ||
      !expect(test_ranges(lengths, none, 5, "none", 0, 0, 0), "no mismatch") ||
      !expect(test_ranges(lengths, single, 5, "48-48", 1, 1, 16), "single mismatch") ||
      !expect(test_ranges(lengths, continuous, 5, "32-64", 3, 1, 48), "continuous mismatch") ||
      !expect(test_ranges(lengths, separated, 5, "16-16;48-48;80-80", 3, 3, 16), "separated mismatch") ||
      !expect(test_ranges(lengths, endpoints, 5, "16-16;80-80", 2, 2, 16), "endpoint mismatch")) {
    return 1;
  }
  printf("[OK] policy boundary and mismatch metrics\n");
  return 0;
}
