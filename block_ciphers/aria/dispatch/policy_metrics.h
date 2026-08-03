#ifndef POLICY_METRICS_H
#define POLICY_METRICS_H

#include <stddef.h>

typedef struct policy_mismatch_summary {
  size_t mismatched_point_count;
  size_t mismatched_interval_count;
  size_t longest_mismatched_interval_bytes;
} policy_mismatch_summary_t;

double policy_mean_symmetric_boundary_distance(const size_t *policy_boundaries,
                                               size_t policy_count,
                                               const size_t *reference_boundaries,
                                               size_t reference_count,
                                               size_t maximum_penalty);

int policy_summarize_mismatches(const size_t *lengths,
                                const int *exact_matches,
                                size_t count,
                                size_t length_step,
                                char *ranges,
                                size_t ranges_size,
                                policy_mismatch_summary_t *summary);

#endif
