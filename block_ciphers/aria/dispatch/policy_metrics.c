#include "policy_metrics.h"

#include <stdio.h>
#include <string.h>

static size_t nearest_distance(const size_t *reference, size_t count, size_t value) {
  size_t best = (size_t)-1;
  size_t i;
  for (i = 0; i < count; ++i) {
    const size_t distance = reference[i] > value ? reference[i] - value : value - reference[i];
    if (distance < best) {
      best = distance;
    }
  }
  return best;
}

double policy_mean_symmetric_boundary_distance(const size_t *policy_boundaries,
                                               size_t policy_count,
                                               const size_t *reference_boundaries,
                                               size_t reference_count,
                                               size_t maximum_penalty) {
  double sum = 0.0;
  size_t i;
  if (policy_count == 0 && reference_count == 0) {
    return 0.0;
  }
  if (policy_count == 0 || reference_count == 0 || !policy_boundaries || !reference_boundaries) {
    return (double)maximum_penalty;
  }
  for (i = 0; i < policy_count; ++i) {
    sum += (double)nearest_distance(reference_boundaries, reference_count, policy_boundaries[i]);
  }
  for (i = 0; i < reference_count; ++i) {
    sum += (double)nearest_distance(policy_boundaries, policy_count, reference_boundaries[i]);
  }
  return sum / (double)(policy_count + reference_count);
}

int policy_summarize_mismatches(const size_t *lengths,
                                const int *exact_matches,
                                size_t count,
                                size_t length_step,
                                char *ranges,
                                size_t ranges_size,
                                policy_mismatch_summary_t *summary) {
  size_t i = 0;
  size_t used = 0;
  if (!lengths || !exact_matches || count == 0 || length_step == 0 || !ranges ||
      ranges_size == 0 || !summary) {
    return 0;
  }
  memset(summary, 0, sizeof(*summary));
  ranges[0] = '\0';
  while (i < count) {
    size_t begin;
    size_t end;
    size_t interval_bytes;
    int written;
    if (exact_matches[i]) {
      ++i;
      continue;
    }
    begin = i;
    while (i + 1 < count && !exact_matches[i + 1] &&
           lengths[i + 1] == lengths[i] + length_step) {
      ++i;
    }
    end = i;
    summary->mismatched_interval_count++;
    summary->mismatched_point_count += end - begin + 1;
    interval_bytes = lengths[end] - lengths[begin] + length_step;
    if (interval_bytes > summary->longest_mismatched_interval_bytes) {
      summary->longest_mismatched_interval_bytes = interval_bytes;
    }
    written = snprintf(ranges + used,
                       ranges_size - used,
                       "%s%zu-%zu",
                       used ? ";" : "",
                       lengths[begin],
                       lengths[end]);
    if (written < 0 || (size_t)written >= ranges_size - used) {
      return 0;
    }
    used += (size_t)written;
    ++i;
  }
  if (used == 0) {
    if (ranges_size < 5) {
      return 0;
    }
    memcpy(ranges, "none", 5);
  }
  return 1;
}
