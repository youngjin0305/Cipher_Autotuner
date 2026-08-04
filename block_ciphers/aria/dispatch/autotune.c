#include "autotune.h"

#include "aria_api.h"
#include "runtime_dispatch.h"
#include "policy_metrics.h"
#include "../source/linux_kernal/common/aria-avx.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define AUTOTUNE_PHASE_COARSE "coarse"
#define AUTOTUNE_PHASE_REFINED "refined"
#define AUTOTUNE_BLOCK_SIZE 16u
#define AUTOTUNE_KEY_VARIANTS 3u
#define AUTOTUNE_POLICY_CLASSES 4u
#define AUTOTUNE_INVALID_INDEX ((size_t)-1)

typedef struct aria_autotune_row {
  int key_bits;
  size_t length;
  char impl_name[32];
  char effective_path[64];
  char execution_path[160];
  size_t gfni_64way_chunk_count;
  size_t avx2_32way_chunk_count;
  size_t avx_16way_chunk_count;
  size_t ref_tail_block_count;
  char normalized_impl[32];
  double trimmed_mean_ns_per_call;
  double trimmed_mean_ns_per_byte;
  double raw_mean_ns_per_call;
  double median_ns_per_call;
  size_t sample_count;
  int is_raw_winner;
  int is_policy_winner;
  char phase[16];
} aria_autotune_row_t;

typedef struct aria_autotune_point {
  int key_bits;
  size_t length;
  int source_is_refined;
  char raw_winner_impl[32];
  char raw_effective_path[64];
  char raw_second_impl[32];
  double raw_best_ns_per_call;
  double raw_second_ns_per_call;
  double raw_margin_pct;
  int raw_low_margin;
  size_t raw_row_index;
  char policy_winner_impl[32];
  char policy_effective_path[64];
  double policy_best_ns_per_call;
  double policy_second_ns_per_call;
  double policy_margin_pct;
  int policy_low_margin;
  size_t policy_row_index;
  double normalized_best_ns[AUTOTUNE_POLICY_CLASSES];
  size_t normalized_best_row_index[AUTOTUNE_POLICY_CLASSES];
  char normalized_best_path[AUTOTUNE_POLICY_CLASSES][64];
  char normalized_best_source_impl[AUTOTUNE_POLICY_CLASSES][32];
  char basis_impl[32];
  char basis_effective_path[64];
  char note[192];
} aria_autotune_point_t;

typedef struct aria_boundary_candidate {
  int key_bits;
  size_t left_len;
  size_t right_len;
  char left_raw_winner[32];
  char right_raw_winner[32];
  char left_policy_winner[32];
  char right_policy_winner[32];
  double left_raw_margin_pct;
  double right_raw_margin_pct;
  double left_policy_margin_pct;
  double right_policy_margin_pct;
  int is_raw_boundary;
  int is_policy_boundary;
  int needs_refine;
  int refined;
  char basis_used[16];
  char note[192];
} aria_boundary_candidate_t;

typedef struct aria_policy_entry {
  int key_bits;
  size_t start_len;
  size_t end_len;
  size_t bucket_points;
  char raw_chosen_impl[32];
  char policy_chosen_impl[32];
  char representative_effective_path[64];
  char note[192];
  char source_phase[16];
  char basis_used[16];
  char policy_basis_metric[32];
  char raw_winners_in_segment[128];
  double min_margin_pct;
} aria_policy_entry_t;

typedef struct row_vec {
  aria_autotune_row_t *items;
  size_t count;
  size_t capacity;
} row_vec_t;

typedef struct point_vec {
  aria_autotune_point_t *items;
  size_t count;
  size_t capacity;
} point_vec_t;

typedef struct boundary_vec {
  aria_boundary_candidate_t *items;
  size_t count;
  size_t capacity;
} boundary_vec_t;

typedef struct policy_vec {
  aria_policy_entry_t *items;
  size_t count;
  size_t capacity;
} policy_vec_t;

typedef struct size_vec {
  size_t *items;
  size_t count;
  size_t capacity;
} size_vec_t;

static const int autotune_key_bits[AUTOTUNE_KEY_VARIANTS] = {128, 192, 256};

static const aria_impl_t *autotune_impls[] = {
  &aria_ref_impl,
  &aria_linux_aesni_avx_impl,
  &aria_linux_aesni_avx2_impl,
  &aria_linux_gfni_avx512_impl
};

static const char *policy_impl_names[AUTOTUNE_POLICY_CLASSES] = {
  "ref",
  "linux_aesni_avx",
  "linux_aesni_avx2",
  "linux_gfni_avx512"
};

static double last_autotune_duration_ms;
static aria_autotune_metrics_t last_autotune_metrics;

static double wall_time_ms(void) {
  struct timespec timestamp;
  if (timespec_get(&timestamp, TIME_UTC) != TIME_UTC) {
    return 0.0;
  }
  return (double)timestamp.tv_sec * 1000.0 + (double)timestamp.tv_nsec / 1e6;
}

static size_t autotune_impls_count(void) {
  return sizeof(autotune_impls) / sizeof(autotune_impls[0]);
}

static const char *policy_basis_name(aria_autotune_policy_basis_t basis) {
  return (basis == ARIA_POLICY_BASIS_RAW) ? "raw" : "normalized";
}

static const char *tail_policy_name(aria_autotune_tail_policy_t tail_policy) {
  return (tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE) ? "conservative" : "native";
}

static const char *profile_name(aria_autotune_profile_t profile) {
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

static int class_index_from_impl_name(const char *impl_name) {
  size_t i;

  if (!impl_name) {
    return -1;
  }
  for (i = 0; i < AUTOTUNE_POLICY_CLASSES; ++i) {
    if (strcmp(policy_impl_names[i], impl_name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static const char *impl_name_from_class_index(int class_index) {
  if (class_index < 0 || class_index >= (int)AUTOTUNE_POLICY_CLASSES) {
    return "ref";
  }
  return policy_impl_names[class_index];
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, dst_size, "%s", src);
}

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
  copy_string(buffer, sizeof(buffer), path);

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

static int should_emit_autotune_debug(const aria_autotune_config_t *config) {
  return config && config->output_level >= ARIA_OUTPUT_LEVEL_DEBUG;
}

static int build_autotune_output_path(char *buffer,
                                      size_t buffer_size,
                                      const aria_autotune_config_t *config,
                                      const char *suffix_name) {
  if (!buffer || buffer_size == 0 || !config || !suffix_name) {
    return 0;
  }

  if (config->output_dir && config->output_dir[0] != '\0') {
    if (!ensure_directory_recursive(config->output_dir)) {
      return 0;
    }
    snprintf(buffer, buffer_size, "%s/%s", config->output_dir, suffix_name);
    return 1;
  }

  snprintf(buffer, buffer_size, "%s_%s", config->output_prefix, suffix_name);
  return 1;
}

static void append_note(char *note, size_t note_size, const char *suffix) {
  size_t used;

  if (!note || note_size == 0 || !suffix || suffix[0] == '\0') {
    return;
  }

  used = strlen(note);
  if (used > 0 && used + 1 < note_size) {
    note[used++] = ';';
    note[used] = '\0';
  }
  snprintf(note + used, note_size - used, "%s", suffix);
}

static int row_vec_push(row_vec_t *vec, const aria_autotune_row_t *item) {
  aria_autotune_row_t *next;
  size_t new_capacity;

  if (!vec || !item) {
    return 0;
  }
  if (vec->count == vec->capacity) {
    new_capacity = (vec->capacity == 0) ? 64u : (vec->capacity * 2u);
    next = (aria_autotune_row_t *)realloc(vec->items, sizeof(*vec->items) * new_capacity);
    if (!next) {
      return 0;
    }
    vec->items = next;
    vec->capacity = new_capacity;
  }
  vec->items[vec->count++] = *item;
  return 1;
}

static int point_vec_push(point_vec_t *vec, const aria_autotune_point_t *item) {
  aria_autotune_point_t *next;
  size_t new_capacity;

  if (!vec || !item) {
    return 0;
  }
  if (vec->count == vec->capacity) {
    new_capacity = (vec->capacity == 0) ? 64u : (vec->capacity * 2u);
    next = (aria_autotune_point_t *)realloc(vec->items, sizeof(*vec->items) * new_capacity);
    if (!next) {
      return 0;
    }
    vec->items = next;
    vec->capacity = new_capacity;
  }
  vec->items[vec->count++] = *item;
  return 1;
}

static int boundary_vec_push(boundary_vec_t *vec, const aria_boundary_candidate_t *item) {
  aria_boundary_candidate_t *next;
  size_t new_capacity;

  if (!vec || !item) {
    return 0;
  }
  if (vec->count == vec->capacity) {
    new_capacity = (vec->capacity == 0) ? 16u : (vec->capacity * 2u);
    next = (aria_boundary_candidate_t *)realloc(vec->items, sizeof(*vec->items) * new_capacity);
    if (!next) {
      return 0;
    }
    vec->items = next;
    vec->capacity = new_capacity;
  }
  vec->items[vec->count++] = *item;
  return 1;
}

static int policy_vec_push(policy_vec_t *vec, const aria_policy_entry_t *item) {
  aria_policy_entry_t *next;
  size_t new_capacity;

  if (!vec || !item) {
    return 0;
  }
  if (vec->count == vec->capacity) {
    new_capacity = (vec->capacity == 0) ? 16u : (vec->capacity * 2u);
    next = (aria_policy_entry_t *)realloc(vec->items, sizeof(*vec->items) * new_capacity);
    if (!next) {
      return 0;
    }
    vec->items = next;
    vec->capacity = new_capacity;
  }
  vec->items[vec->count++] = *item;
  return 1;
}

static int size_vec_push_unique(size_vec_t *vec, size_t value) {
  size_t *next;
  size_t i;
  size_t new_capacity;

  if (!vec) {
    return 0;
  }
  for (i = 0; i < vec->count; ++i) {
    if (vec->items[i] == value) {
      return 1;
    }
  }
  if (vec->count == vec->capacity) {
    new_capacity = (vec->capacity == 0) ? 64u : (vec->capacity * 2u);
    next = (size_t *)realloc(vec->items, sizeof(*vec->items) * new_capacity);
    if (!next) {
      return 0;
    }
    vec->items = next;
    vec->capacity = new_capacity;
  }
  vec->items[vec->count++] = value;
  return 1;
}

static void row_vec_free(row_vec_t *vec) {
  if (!vec) {
    return;
  }
  free(vec->items);
  vec->items = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

static void point_vec_free(point_vec_t *vec) {
  if (!vec) {
    return;
  }
  free(vec->items);
  vec->items = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

static void boundary_vec_free(boundary_vec_t *vec) {
  if (!vec) {
    return;
  }
  free(vec->items);
  vec->items = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

static void policy_vec_free(policy_vec_t *vec) {
  if (!vec) {
    return;
  }
  free(vec->items);
  vec->items = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

static void size_vec_free(size_vec_t *vec) {
  if (!vec) {
    return;
  }
  free(vec->items);
  vec->items = NULL;
  vec->count = 0;
  vec->capacity = 0;
}

static int cmp_size_t_value(const void *a, const void *b) {
  const size_t va = *(const size_t *)a;
  const size_t vb = *(const size_t *)b;
  if (va < vb) {
    return -1;
  }
  if (va > vb) {
    return 1;
  }
  return 0;
}

static int cmp_double_value(const void *a, const void *b) {
  const double va = *(const double *)a;
  const double vb = *(const double *)b;
  return va < vb ? -1 : va > vb ? 1 : 0;
}

static int cmp_point_key_len(const void *a, const void *b) {
  const aria_autotune_point_t *pa = (const aria_autotune_point_t *)a;
  const aria_autotune_point_t *pb = (const aria_autotune_point_t *)b;

  if (pa->key_bits != pb->key_bits) {
    return (pa->key_bits < pb->key_bits) ? -1 : 1;
  }
  if (pa->length < pb->length) {
    return -1;
  }
  if (pa->length > pb->length) {
    return 1;
  }
  if (pa->source_is_refined != pb->source_is_refined) {
    return (pa->source_is_refined > pb->source_is_refined) ? -1 : 1;
  }
  return 0;
}

static int cmp_policy_key_start(const void *a, const void *b) {
  const aria_policy_entry_t *pa = (const aria_policy_entry_t *)a;
  const aria_policy_entry_t *pb = (const aria_policy_entry_t *)b;

  if (pa->key_bits != pb->key_bits) {
    return (pa->key_bits < pb->key_bits) ? -1 : 1;
  }
  if (pa->start_len < pb->start_len) {
    return -1;
  }
  if (pa->start_len > pb->start_len) {
    return 1;
  }
  return 0;
}

static size_t align_up_block(size_t len) {
  size_t rem = len % AUTOTUNE_BLOCK_SIZE;
  if (rem == 0) {
    return len;
  }
  return len + (AUTOTUNE_BLOCK_SIZE - rem);
}

static size_t align_down_block(size_t len) {
  return len - (len % AUTOTUNE_BLOCK_SIZE);
}

static const aria_impl_t *find_impl_by_name(const char *name) {
  size_t i;

  for (i = 0; i < autotune_impls_count(); ++i) {
    if (strcmp(autotune_impls[i]->name, name) == 0) {
      return autotune_impls[i];
    }
  }
  return NULL;
}

static const char *effective_path_for_impl_name(const char *impl_name, size_t len) {
  const aria_impl_t *impl = find_impl_by_name(impl_name);

  if (!impl) {
    return "unknown";
  }
  if (impl->effective_path) {
    return impl->effective_path(len);
  }
  return impl->name;
}

static aria_execution_path_t execution_path_for_impl(const aria_impl_t *impl, size_t len) {
  aria_execution_path_t path = {0, 0, 0, 0};

  if (impl && impl->execution_path) {
    impl->execution_path(len, &path);
  }
  return path;
}

static void format_execution_path(const aria_execution_path_t *path,
                                  char *buffer,
                                  size_t buffer_size) {
  static const char *labels[] = {
    "gfni_64way", "avx2_32way", "avx_16way", "ref"
  };
  size_t counts[4];
  size_t used = 0;
  size_t i;

  if (!path || !buffer || buffer_size == 0) {
    return;
  }
  counts[0] = path->gfni_64way_chunk_count;
  counts[1] = path->avx2_32way_chunk_count;
  counts[2] = path->avx_16way_chunk_count;
  counts[3] = path->ref_tail_block_count;
  buffer[0] = '\0';

  for (i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
    int written;
    if (counts[i] == 0 || used >= buffer_size) {
      continue;
    }
    written = snprintf(buffer + used,
                       buffer_size - used,
                       "%s%s*%zu",
                       used == 0 ? "" : " + ",
                       labels[i],
                       counts[i]);
    if (written < 0) {
      buffer[0] = '\0';
      return;
    }
    if ((size_t)written >= buffer_size - used) {
      used = buffer_size - 1;
      break;
    }
    used += (size_t)written;
  }
  if (used == 0) {
    snprintf(buffer, buffer_size, "none");
  }
}

static const char *normalize_impl_name(const aria_autotune_config_t *config,
                                       const char *impl_name,
                                       const char *effective_path) {
  (void)impl_name;

  if (!config || !effective_path) {
    return "ref";
  }
  if (strcmp(effective_path, "ref") == 0) {
    return "ref";
  }
  if (strcmp(effective_path, "ref_fallback") == 0) {
    return config->collapse_ref_fallback ? "ref" : impl_name;
  }
  if (strcmp(effective_path, "linux_aesni_avx2") == 0) {
    return "linux_aesni_avx2";
  }
  if (strcmp(effective_path, "linux_aesni_avx2_plus_avx_tail") == 0) {
    return "linux_aesni_avx2";
  }
  if (strcmp(effective_path, "linux_aesni_avx2_plus_ref_tail") == 0 ||
      strcmp(effective_path, "linux_aesni_avx2_plus_avx_ref_tail") == 0) {
    return (config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE) ? "ref" : "linux_aesni_avx2";
  }
  if (strcmp(effective_path, "linux_aesni_avx") == 0) {
    return "linux_aesni_avx";
  }
  if (strcmp(effective_path, "linux_aesni_avx_plus_ref_tail") == 0) {
    return (config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE) ? "ref" : "linux_aesni_avx";
  }
  if (strcmp(effective_path, "linux_gfni_avx512") == 0) {
    return "linux_gfni_avx512";
  }
  if (strcmp(effective_path, "linux_gfni_avx2_32way") == 0 ||
      strcmp(effective_path, "linux_gfni_avx_16way") == 0 ||
      strcmp(effective_path, "linux_gfni_mixed_width") == 0) {
    return "linux_gfni_avx512";
  }
  if (strcmp(effective_path, "linux_gfni_plus_ref_tail") == 0) {
    return (config->tail_policy == ARIA_TAIL_POLICY_CONSERVATIVE)
               ? "ref"
               : "linux_gfni_avx512";
  }
  if (strcmp(effective_path, "mixed_effective_path") == 0) {
    return "ref";
  }
  return "ref";
}

static void reset_buffers(uint8_t *input, uint8_t *output, size_t buffer_size) {
  size_t i;

  if (!input || !output) {
    return;
  }
  for (i = 0; i < buffer_size; ++i) {
    input[i] = (uint8_t)(i & 0xFFu);
    output[i] = 0;
  }
}

static void initialize_point(aria_autotune_point_t *point, int key_bits, size_t len, int source_is_refined) {
  size_t i;

  memset(point, 0, sizeof(*point));
  point->key_bits = key_bits;
  point->length = len;
  point->source_is_refined = source_is_refined;
  point->raw_row_index = AUTOTUNE_INVALID_INDEX;
  point->policy_row_index = AUTOTUNE_INVALID_INDEX;
  for (i = 0; i < AUTOTUNE_POLICY_CLASSES; ++i) {
    point->normalized_best_ns[i] = 0.0;
    point->normalized_best_row_index[i] = AUTOTUNE_INVALID_INDEX;
    point->normalized_best_path[i][0] = '\0';
    point->normalized_best_source_impl[i][0] = '\0';
  }
}

static void compute_margin(double best_value, double second_value, double *margin_pct) {
  if (!margin_pct) {
    return;
  }
  if (best_value <= 0.0 || second_value <= 0.0) {
    *margin_pct = 0.0;
    return;
  }
  *margin_pct = ((second_value - best_value) / best_value) * 100.0;
  if (*margin_pct < 0.0) {
    *margin_pct = 0.0;
  }
}

static void initialize_basis_choice(const aria_autotune_config_t *config, aria_autotune_point_t *point) {
  if (!config || !point) {
    return;
  }

  if (config->policy_basis == ARIA_POLICY_BASIS_RAW) {
    copy_string(point->basis_impl, sizeof(point->basis_impl), point->raw_winner_impl);
    copy_string(point->basis_effective_path, sizeof(point->basis_effective_path), point->raw_effective_path);
  } else {
    copy_string(point->basis_impl, sizeof(point->basis_impl), point->policy_winner_impl);
    copy_string(point->basis_effective_path, sizeof(point->basis_effective_path), point->policy_effective_path);
  }
}

static double point_basis_margin(const aria_autotune_config_t *config, const aria_autotune_point_t *point) {
  if (!config || !point) {
    return 0.0;
  }
  return (config->policy_basis == ARIA_POLICY_BASIS_RAW) ? point->raw_margin_pct : point->policy_margin_pct;
}

static int point_basis_low_margin(const aria_autotune_config_t *config, const aria_autotune_point_t *point) {
  if (!config || !point) {
    return 0;
  }
  return (config->policy_basis == ARIA_POLICY_BASIS_RAW) ? point->raw_low_margin : point->policy_low_margin;
}

static void assign_basis_impl(const aria_autotune_config_t *config,
                              aria_autotune_point_t *point,
                              const char *impl_name,
                              const char *tag) {
  int class_index;

  if (!config || !point || !impl_name) {
    return;
  }

  copy_string(point->basis_impl, sizeof(point->basis_impl), impl_name);

  if (config->policy_basis == ARIA_POLICY_BASIS_RAW) {
    if (strcmp(impl_name, point->raw_winner_impl) == 0) {
      copy_string(point->basis_effective_path, sizeof(point->basis_effective_path), point->raw_effective_path);
    } else {
      copy_string(point->basis_effective_path,
                  sizeof(point->basis_effective_path),
                  effective_path_for_impl_name(impl_name, point->length));
    }
  } else {
    class_index = class_index_from_impl_name(impl_name);
    if (class_index >= 0 && point->normalized_best_path[class_index][0] != '\0') {
      copy_string(point->basis_effective_path,
                  sizeof(point->basis_effective_path),
                  point->normalized_best_path[class_index]);
    } else {
      copy_string(point->basis_effective_path,
                  sizeof(point->basis_effective_path),
                  effective_path_for_impl_name(impl_name, point->length));
    }
  }

  append_note(point->note, sizeof(point->note), tag);
}

static int generate_coarse_lengths(const aria_autotune_config_t *config, size_vec_t *lengths) {
  static const size_t structural_units[] = {
    ARIA_BLOCK_SIZE,
    ARIA_AESNI_PARALLEL_BLOCK_SIZE,
    ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE,
    ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE
  };
  size_t len;
  size_t i;

  if (!config || !lengths) {
    return 0;
  }

  if (!size_vec_push_unique(lengths, align_up_block(config->min_len))) {
    return 0;
  }
  if (!size_vec_push_unique(lengths, align_down_block(config->max_len))) {
    return 0;
  }

  for (len = AUTOTUNE_BLOCK_SIZE; len <= config->max_len; len *= 2u) {
    size_t mid;

    if (len >= config->min_len && len % AUTOTUNE_BLOCK_SIZE == 0) {
      if (!size_vec_push_unique(lengths, len)) {
        return 0;
      }
    }

    mid = len + (len / 2u);
    if (mid >= config->min_len &&
        mid <= config->max_len &&
        mid % AUTOTUNE_BLOCK_SIZE == 0) {
      if (!size_vec_push_unique(lengths, mid)) {
        return 0;
      }
    }

    if (len > config->max_len / 2u) {
      break;
    }
  }

  for (i = 0; i < sizeof(structural_units) / sizeof(structural_units[0]); ++i) {
    size_t unit = structural_units[i];
    size_t k;
    size_t max_multiplier;

    if (unit == 0 || unit % AUTOTUNE_BLOCK_SIZE != 0) {
      return 0;
    }

    max_multiplier = (unit == AUTOTUNE_BLOCK_SIZE)
                         ? 3u
                         : config->max_len / unit;
    for (k = 1; k <= max_multiplier; ++k) {
      size_t base = k * unit;

      if (base >= AUTOTUNE_BLOCK_SIZE) {
        size_t before = base - AUTOTUNE_BLOCK_SIZE;
        if (before >= config->min_len && before <= config->max_len) {
          if (!size_vec_push_unique(lengths, before)) {
            return 0;
          }
        }
      }
      if (base >= config->min_len && base <= config->max_len) {
        if (!size_vec_push_unique(lengths, base)) {
          return 0;
        }
      }
      if (base <= config->max_len - AUTOTUNE_BLOCK_SIZE) {
        size_t after = base + AUTOTUNE_BLOCK_SIZE;
        if (after >= config->min_len && after <= config->max_len) {
          if (!size_vec_push_unique(lengths, after)) {
            return 0;
          }
        }
      }
    }
  }

  qsort(lengths->items, lengths->count, sizeof(*lengths->items), cmp_size_t_value);
  return 1;
}

static int generate_refine_lengths(const aria_autotune_config_t *config,
                                   const boundary_vec_t *boundaries,
                                   size_vec_t *lengths) {
  size_t i;

  if (!config || !boundaries || !lengths) {
    return 0;
  }

  for (i = 0; i < boundaries->count; ++i) {
    size_t start;
    size_t end;
    size_t len;

    if (!boundaries->items[i].needs_refine) {
      continue;
    }

    start = boundaries->items[i].left_len;
    end = boundaries->items[i].right_len;

    if (config->refine_buffer > 0) {
      size_t buffer_bytes = config->refine_buffer > SIZE_MAX / config->refine_step
                                ? SIZE_MAX
                                : config->refine_buffer * config->refine_step;
      start = (start > buffer_bytes) ? (start - buffer_bytes) : config->min_len;
      end = (buffer_bytes < config->max_len - end) ? (end + buffer_bytes) : config->max_len;
    }

    start = align_up_block(start);
    end = align_down_block(end);
    for (len = start; len <= end; len += config->refine_step) {
      if (!size_vec_push_unique(lengths, len)) {
        return 0;
      }
      if (config->max_len - len < config->refine_step) {
        break;
      }
    }
  }

  qsort(lengths->items, lengths->count, sizeof(*lengths->items), cmp_size_t_value);
  return 1;
}

static int measure_points(const aria_autotune_config_t *config,
                          const size_vec_t *lengths,
                          const char *phase_name,
                          size_t outer,
                          uint8_t *input,
                          uint8_t *output,
                          size_t buffer_size,
                          uint64_t target_ticks,
                          size_t inner_max,
                          size_t warmup,
                          stat_mode_t stat_mode,
                          row_vec_t *rows,
                          point_vec_t *points) {
  size_t key_idx;
  size_t len_idx;

  if (!config || !lengths || !phase_name || !input || !output || !rows || !points) {
    return 0;
  }

  for (key_idx = 0; key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
    int key_bits = autotune_key_bits[key_idx];
    uint8_t key[32] = {0};

    for (len_idx = 0; len_idx < lengths->count; ++len_idx) {
      aria_autotune_point_t point;
      size_t impl_idx;
      double raw_second = 0.0;
      char raw_second_impl[32] = "unknown";
      double policy_second = 0.0;
      int raw_found = 0;
      int policy_class;
      size_t rows_before = rows->count;

      if (lengths->items[len_idx] > buffer_size) {
        fprintf(stderr, "autotune: length %zu exceeds buffer size %zu.\n",
                lengths->items[len_idx], buffer_size);
        return 0;
      }

      initialize_point(&point,
                       key_bits,
                       lengths->items[len_idx],
                       strcmp(phase_name, AUTOTUNE_PHASE_REFINED) == 0);

      for (impl_idx = 0; impl_idx < autotune_impls_count(); ++impl_idx) {
        const size_t rotated_impl_index =
            (impl_idx + len_idx + key_idx) % autotune_impls_count();
        const aria_impl_t *impl = autotune_impls[rotated_impl_index];
        aria_ctx_t ctx;
        bench_result_t result;
        bench_summary_stats_t stats;
        aria_autotune_row_t row;
        aria_execution_path_t execution_path;
        const char *effective_path;
        const char *normalized_impl;
        int normalized_class_index;
        size_t row_index;

        if (impl->is_supported && !impl->is_supported()) {
          continue;
        }

        memset(&ctx, 0, sizeof(ctx));
        impl->init(&ctx, key, key_bits);
        reset_buffers(input, output, buffer_size);
        result = bench_run(impl->encrypt,
                           &ctx,
                           input,
                           output,
                           point.length,
                           outer,
                           target_ticks,
                           inner_max,
                           stat_mode,
                           warmup,
                           config->trim_ratio);
        stats = bench_compute_summary_stats(&result, config->trim_ratio);
        if (!stats.valid) {
          fprintf(stderr, "autotune: invalid benchmark samples for len=%zu key_bits=%d impl=%s.\n",
                  point.length, key_bits, impl->name);
          bench_result_cleanup(&result);
          return 0;
        }

        effective_path = effective_path_for_impl_name(impl->name, point.length);
        execution_path = execution_path_for_impl(impl, point.length);
        normalized_impl = normalize_impl_name(config, impl->name, effective_path);
        normalized_class_index = class_index_from_impl_name(normalized_impl);

        memset(&row, 0, sizeof(row));
        row.key_bits = key_bits;
        row.length = point.length;
        copy_string(row.impl_name, sizeof(row.impl_name), impl->name);
        copy_string(row.effective_path, sizeof(row.effective_path), effective_path);
        format_execution_path(&execution_path, row.execution_path, sizeof(row.execution_path));
        row.gfni_64way_chunk_count = execution_path.gfni_64way_chunk_count;
        row.avx2_32way_chunk_count = execution_path.avx2_32way_chunk_count;
        row.avx_16way_chunk_count = execution_path.avx_16way_chunk_count;
        row.ref_tail_block_count = execution_path.ref_tail_block_count;
        copy_string(row.normalized_impl, sizeof(row.normalized_impl), normalized_impl);
        row.trimmed_mean_ns_per_call = stats.trimmed_mean_ns_per_call;
        row.trimmed_mean_ns_per_byte = stats.trimmed_mean_ns_per_byte;
        row.raw_mean_ns_per_call = stats.raw_mean_ns_per_call;
        row.median_ns_per_call = stats.ns_per_call_p50;
        row.sample_count = stats.n_samples;
        copy_string(row.phase, sizeof(row.phase), phase_name);

        if (!row_vec_push(rows, &row)) {
          bench_result_cleanup(&result);
          return 0;
        }

        row_index = rows->count - 1;

        if (!raw_found || row.trimmed_mean_ns_per_call < point.raw_best_ns_per_call) {
          if (raw_found) {
            raw_second = point.raw_best_ns_per_call;
            copy_string(raw_second_impl, sizeof(raw_second_impl), point.raw_winner_impl);
          }
          point.raw_best_ns_per_call = row.trimmed_mean_ns_per_call;
          copy_string(point.raw_winner_impl, sizeof(point.raw_winner_impl), row.impl_name);
          copy_string(point.raw_effective_path, sizeof(point.raw_effective_path), row.effective_path);
          point.raw_row_index = row_index;
          raw_found = 1;
        } else if (raw_second <= 0.0 || row.trimmed_mean_ns_per_call < raw_second) {
          raw_second = row.trimmed_mean_ns_per_call;
          copy_string(raw_second_impl, sizeof(raw_second_impl), row.impl_name);
        }

        if (normalized_class_index >= 0 &&
            (point.normalized_best_ns[normalized_class_index] <= 0.0 ||
             row.trimmed_mean_ns_per_call < point.normalized_best_ns[normalized_class_index])) {
          point.normalized_best_ns[normalized_class_index] = row.trimmed_mean_ns_per_call;
          point.normalized_best_row_index[normalized_class_index] = row_index;
          copy_string(point.normalized_best_path[normalized_class_index],
                      sizeof(point.normalized_best_path[normalized_class_index]),
                      row.effective_path);
          copy_string(point.normalized_best_source_impl[normalized_class_index],
                      sizeof(point.normalized_best_source_impl[normalized_class_index]),
                      row.impl_name);
        }

        bench_result_cleanup(&result);
      }

      if (!raw_found) {
        fprintf(stderr, "autotune: no supported implementations for len=%zu key_bits=%d.\n",
                point.length,
                key_bits);
        return 0;
      }

      point.raw_second_ns_per_call = raw_second;
      copy_string(point.raw_second_impl, sizeof(point.raw_second_impl), raw_second_impl);
      compute_margin(point.raw_best_ns_per_call, point.raw_second_ns_per_call, &point.raw_margin_pct);
      point.raw_low_margin = (point.raw_margin_pct < config->winner_margin_pct) ? 1 : 0;

      for (policy_class = 0; policy_class < (int)AUTOTUNE_POLICY_CLASSES; ++policy_class) {
        if (point.normalized_best_ns[policy_class] <= 0.0) {
          continue;
        }
        if (point.policy_best_ns_per_call <= 0.0 ||
            point.normalized_best_ns[policy_class] < point.policy_best_ns_per_call) {
          if (point.policy_best_ns_per_call > 0.0) {
            policy_second = point.policy_best_ns_per_call;
          }
          point.policy_best_ns_per_call = point.normalized_best_ns[policy_class];
          copy_string(point.policy_winner_impl,
                      sizeof(point.policy_winner_impl),
                      impl_name_from_class_index(policy_class));
          copy_string(point.policy_effective_path,
                      sizeof(point.policy_effective_path),
                      point.normalized_best_path[policy_class]);
          point.policy_row_index = point.normalized_best_row_index[policy_class];
        } else if (policy_second <= 0.0 ||
                   point.normalized_best_ns[policy_class] < policy_second) {
          policy_second = point.normalized_best_ns[policy_class];
        }
      }

      point.policy_second_ns_per_call = policy_second;
      compute_margin(point.policy_best_ns_per_call, point.policy_second_ns_per_call, &point.policy_margin_pct);
      point.policy_low_margin = (point.policy_margin_pct < config->winner_margin_pct) ? 1 : 0;

      if (point.raw_row_index != AUTOTUNE_INVALID_INDEX) {
        rows->items[point.raw_row_index].is_raw_winner = 1;
      }
      if (point.policy_row_index != AUTOTUNE_INVALID_INDEX) {
        rows->items[point.policy_row_index].is_policy_winner = 1;
      }

      if (strcmp(point.raw_winner_impl, point.policy_winner_impl) != 0) {
        append_note(point.note, sizeof(point.note), "normalized_policy_differs_from_raw");
      }
      if (strcmp(point.policy_effective_path, "ref_fallback") == 0 &&
          strcmp(point.policy_winner_impl, "ref") == 0) {
        append_note(point.note, sizeof(point.note), "ref_fallback_collapsed_to_ref");
      }
      if (strstr(point.policy_effective_path, "ref_tail") != NULL) {
        append_note(point.note, sizeof(point.note), "plus_ref_tail_observed");
      }

      initialize_basis_choice(config, &point);

      if (!point_vec_push(points, &point)) {
        return 0;
      }

      (void)rows_before;
    }
  }

  return 1;
}

static int build_boundary_candidates(const aria_autotune_config_t *config,
                                     const point_vec_t *coarse_points,
                                     boundary_vec_t *boundaries) {
  size_t i;

  if (!config || !coarse_points || !boundaries) {
    return 0;
  }

  for (i = 1; i < coarse_points->count; ++i) {
    const aria_autotune_point_t *left = &coarse_points->items[i - 1];
    const aria_autotune_point_t *right = &coarse_points->items[i];
    aria_boundary_candidate_t candidate;

    if (left->key_bits != right->key_bits) {
      continue;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.key_bits = left->key_bits;
    candidate.left_len = left->length;
    candidate.right_len = right->length;
    copy_string(candidate.left_raw_winner, sizeof(candidate.left_raw_winner), left->raw_winner_impl);
    copy_string(candidate.right_raw_winner, sizeof(candidate.right_raw_winner), right->raw_winner_impl);
    copy_string(candidate.left_policy_winner, sizeof(candidate.left_policy_winner), left->policy_winner_impl);
    copy_string(candidate.right_policy_winner, sizeof(candidate.right_policy_winner), right->policy_winner_impl);
    candidate.left_raw_margin_pct = left->raw_margin_pct;
    candidate.right_raw_margin_pct = right->raw_margin_pct;
    candidate.left_policy_margin_pct = left->policy_margin_pct;
    candidate.right_policy_margin_pct = right->policy_margin_pct;
    candidate.is_raw_boundary = (strcmp(left->raw_winner_impl, right->raw_winner_impl) != 0);
    candidate.is_policy_boundary = (strcmp(left->policy_winner_impl, right->policy_winner_impl) != 0);
    candidate.needs_refine = (config->policy_basis == ARIA_POLICY_BASIS_RAW)
                                 ? candidate.is_raw_boundary
                                 : candidate.is_policy_boundary;
    candidate.refined = 0;
    copy_string(candidate.basis_used, sizeof(candidate.basis_used), policy_basis_name(config->policy_basis));

    if (!candidate.is_raw_boundary && !candidate.is_policy_boundary) {
      continue;
    }

    if (candidate.is_raw_boundary && !candidate.is_policy_boundary) {
      append_note(candidate.note, sizeof(candidate.note), "raw_only_boundary_filtered_by_normalization");
    }
    if (!candidate.is_raw_boundary && candidate.is_policy_boundary) {
      append_note(candidate.note, sizeof(candidate.note), "policy_boundary_from_effective_path_change");
    }
    if (strcmp(left->policy_effective_path, right->policy_effective_path) != 0) {
      append_note(candidate.note, sizeof(candidate.note), "policy_effective_path_shift");
    }

    if (!boundary_vec_push(boundaries, &candidate)) {
      return 0;
    }
  }

  return 1;
}

static int merge_points(const point_vec_t *coarse_points,
                        const point_vec_t *refined_points,
                        point_vec_t *merged_points) {
  size_t i;

  if (!coarse_points || !refined_points || !merged_points) {
    return 0;
  }

  for (i = 0; i < coarse_points->count; ++i) {
    if (!point_vec_push(merged_points, &coarse_points->items[i])) {
      return 0;
    }
  }
  for (i = 0; i < refined_points->count; ++i) {
    if (!point_vec_push(merged_points, &refined_points->items[i])) {
      return 0;
    }
  }

  qsort(merged_points->items, merged_points->count, sizeof(*merged_points->items), cmp_point_key_len);

  if (merged_points->count > 1) {
    size_t out = 0;
    for (i = 0; i < merged_points->count; ++i) {
      if (out > 0 &&
          merged_points->items[i].key_bits == merged_points->items[out - 1].key_bits &&
          merged_points->items[i].length == merged_points->items[out - 1].length) {
        if (merged_points->items[i].source_is_refined >= merged_points->items[out - 1].source_is_refined) {
          merged_points->items[out - 1] = merged_points->items[i];
        }
      } else {
        merged_points->items[out++] = merged_points->items[i];
      }
    }
    merged_points->count = out;
  }

  return 1;
}

static void apply_margin_hold_rule(const aria_autotune_config_t *config,
                                   aria_autotune_point_t *points,
                                   size_t begin,
                                   size_t end) {
  size_t i;

  if (!config || !points || end <= begin + 1) {
    return;
  }

  for (i = begin + 1; i < end; ++i) {
    const char *prev_impl = points[i - 1].basis_impl;
    const int changed = (strcmp(points[i].basis_impl, prev_impl) != 0);
    const int low_margin = point_basis_low_margin(config, &points[i]);

    if (!changed || !low_margin) {
      continue;
    }

    if (i + 1 < end &&
        strcmp(points[i + 1].basis_impl, prev_impl) == 0) {
      assign_basis_impl(config, &points[i], prev_impl, "margin_hold");
      continue;
    }

    assign_basis_impl(config, &points[i], prev_impl, "margin_hold_prev_stable");
  }
}

static size_t run_begin_for_index(const aria_autotune_point_t *points, size_t begin, size_t index) {
  size_t pos = index;
  while (pos > begin && strcmp(points[pos - 1].basis_impl, points[index].basis_impl) == 0) {
    --pos;
  }
  return pos;
}

static size_t run_end_for_index(const aria_autotune_point_t *points, size_t end, size_t index) {
  size_t pos = index;
  while (pos + 1 < end && strcmp(points[pos + 1].basis_impl, points[index].basis_impl) == 0) {
    ++pos;
  }
  return pos;
}

static double representative_margin_for_run(const aria_autotune_config_t *config,
                                            const aria_autotune_point_t *points,
                                            size_t run_begin,
                                            size_t run_end) {
  double best = 0.0;
  size_t i;

  for (i = run_begin; i <= run_end; ++i) {
    double value = point_basis_margin(config, &points[i]);
    if (value > best) {
      best = value;
    }
  }
  return best;
}

static void absorb_run(const aria_autotune_config_t *config,
                       aria_autotune_point_t *points,
                       size_t run_begin,
                       size_t run_end,
                       const char *target_impl,
                       const char *tag) {
  size_t i;

  for (i = run_begin; i <= run_end; ++i) {
    assign_basis_impl(config, &points[i], target_impl, tag);
  }
}

static void absorb_short_runs(const aria_autotune_config_t *config,
                              aria_autotune_point_t *points,
                              size_t begin,
                              size_t end,
                              size_t min_run,
                              const char *tag) {
  int changed = 1;

  if (!config || !points || end <= begin || min_run <= 1) {
    return;
  }

  while (changed) {
    size_t i = begin;
    changed = 0;

    while (i < end) {
      size_t run_begin = i;
      size_t run_end = run_end_for_index(points, end, run_begin);
      size_t run_length = run_end - run_begin + 1;

      if (run_length < min_run) {
        const char *left_impl = (run_begin > begin) ? points[run_begin - 1].basis_impl : NULL;
        const char *right_impl = (run_end + 1 < end) ? points[run_end + 1].basis_impl : NULL;
        double run_margin = representative_margin_for_run(config, points, run_begin, run_end);

        if ((!left_impl || !right_impl) && run_margin >= config->winner_margin_pct) {
          i = run_end + 1;
          continue;
        }

        if (left_impl && right_impl && strcmp(left_impl, right_impl) == 0) {
          absorb_run(config, points, run_begin, run_end, left_impl, tag);
          changed = 1;
          break;
        }

        if (left_impl && right_impl) {
          size_t left_begin = run_begin_for_index(points, begin, run_begin - 1);
          size_t left_end = run_end_for_index(points, end, run_begin - 1);
          size_t right_begin = run_begin_for_index(points, begin, run_end + 1);
          size_t right_end = run_end_for_index(points, end, run_end + 1);
          size_t left_len = left_end - left_begin + 1;
          size_t right_len = right_end - right_begin + 1;
          double left_margin = representative_margin_for_run(config, points, left_begin, left_end);
          double right_margin = representative_margin_for_run(config, points, right_begin, right_end);
          const char *target_impl = left_impl;

          if (right_len > left_len || (right_len == left_len && right_margin > left_margin)) {
            target_impl = right_impl;
          }
          absorb_run(config, points, run_begin, run_end, target_impl, tag);
          changed = 1;
          break;
        }

        if (left_impl) {
          absorb_run(config, points, run_begin, run_end, left_impl, tag);
          changed = 1;
          break;
        }
        if (right_impl) {
          absorb_run(config, points, run_begin, run_end, right_impl, tag);
          changed = 1;
          break;
        }
      }

      i = run_end + 1;
    }
  }
}

static void apply_island_removal(const aria_autotune_config_t *config,
                                 aria_autotune_point_t *points,
                                 size_t begin,
                                 size_t end) {
  size_t i;

  if (!config || !points || end <= begin + 2) {
    return;
  }

  for (i = begin + 1; i + 1 < end; ++i) {
    if (strcmp(points[i - 1].basis_impl, points[i + 1].basis_impl) == 0 &&
        strcmp(points[i].basis_impl, points[i - 1].basis_impl) != 0) {
      assign_basis_impl(config, &points[i], points[i - 1].basis_impl, "island_removed");
    }
  }
}

static void stabilize_points(const aria_autotune_config_t *config, point_vec_t *points) {
  size_t begin = 0;

  if (!config || !points || points->count == 0) {
    return;
  }

  while (begin < points->count) {
    size_t end = begin + 1;

    while (end < points->count && points->items[end].key_bits == points->items[begin].key_bits) {
      ++end;
    }

    apply_margin_hold_rule(config, points->items, begin, end);
    absorb_short_runs(config, points->items, begin, end, config->stability_min_run, "min_stable_run");
    apply_island_removal(config, points->items, begin, end);
    absorb_short_runs(config,
                      points->items,
                      begin,
                      end,
                      config->policy_min_bucket_points,
                      "policy_bucket_simplified");

    begin = end;
  }
}

static void mark_refined_boundaries(boundary_vec_t *boundaries, const point_vec_t *refined_points) {
  size_t i;
  size_t j;

  if (!boundaries || !refined_points) {
    return;
  }

  for (i = 0; i < boundaries->count; ++i) {
    for (j = 0; j < refined_points->count; ++j) {
      if (refined_points->items[j].key_bits != boundaries->items[i].key_bits) {
        continue;
      }
      if (refined_points->items[j].length >= boundaries->items[i].left_len &&
          refined_points->items[j].length <= boundaries->items[i].right_len) {
        boundaries->items[i].refined = 1;
        break;
      }
    }
  }
}

static const char *dominant_path_or_mixed(const aria_autotune_point_t *points,
                                          size_t run_begin,
                                          size_t run_end,
                                          char *buffer,
                                          size_t buffer_size,
                                          int *is_mixed) {
  char first_path[64];
  size_t i;

  if (!points || run_begin > run_end || !buffer || buffer_size == 0) {
    return "mixed_effective_path";
  }

  copy_string(first_path, sizeof(first_path), points[run_begin].basis_effective_path);
  *is_mixed = 0;

  for (i = run_begin + 1; i <= run_end; ++i) {
    if (strcmp(points[i].basis_effective_path, first_path) != 0) {
      *is_mixed = 1;
      copy_string(buffer, buffer_size, "mixed_effective_path");
      return buffer;
    }
  }

  copy_string(buffer, buffer_size, first_path);
  return buffer;
}

static const char *dominant_raw_winner(const aria_autotune_point_t *points,
                                       size_t run_begin,
                                       size_t run_end,
                                       char *buffer,
                                       size_t buffer_size,
                                       int *is_mixed) {
  size_t counts[AUTOTUNE_POLICY_CLASSES] = {0};
  size_t i;
  size_t class_index;
  size_t best_count = 0;
  const char *best_impl = "ref";
  size_t non_zero = 0;

  if (!points || run_begin > run_end || !buffer || buffer_size == 0 || !is_mixed) {
    return "ref";
  }

  for (i = run_begin; i <= run_end; ++i) {
    const int index = class_index_from_impl_name(points[i].raw_winner_impl);
    if (index >= 0) {
      ++counts[index];
    }
  }

  for (class_index = 0; class_index < AUTOTUNE_POLICY_CLASSES; ++class_index) {
    if (counts[class_index] > 0) {
      ++non_zero;
      if (counts[class_index] > best_count) {
        best_count = counts[class_index];
        best_impl = policy_impl_names[class_index];
      }
    }
  }

  *is_mixed = (non_zero > 1) ? 1 : 0;
  copy_string(buffer, buffer_size, best_impl);
  return buffer;
}

static void summarize_raw_winners(const aria_autotune_point_t *points,
                                  size_t run_begin,
                                  size_t run_end,
                                  char *buffer,
                                  size_t buffer_size) {
  size_t counts[AUTOTUNE_POLICY_CLASSES] = {0};
  size_t i;

  if (!points || !buffer || buffer_size == 0 || run_begin > run_end) {
    return;
  }

  for (i = run_begin; i <= run_end; ++i) {
    const int index = class_index_from_impl_name(points[i].raw_winner_impl);
    if (index >= 0) {
      ++counts[index];
    }
  }

  snprintf(buffer,
           buffer_size,
           "ref:%zu|linux_aesni_avx:%zu|linux_aesni_avx2:%zu|linux_gfni_avx512:%zu",
           counts[0],
           counts[1],
           counts[2],
           counts[3]);
}

static double min_basis_margin_for_run(const aria_autotune_config_t *config,
                                       const aria_autotune_point_t *points,
                                       size_t run_begin,
                                       size_t run_end) {
  double min_margin = 0.0;
  size_t i;

  if (!config || !points || run_begin > run_end) {
    return 0.0;
  }

  for (i = run_begin; i <= run_end; ++i) {
    double margin = point_basis_margin(config, &points[i]);
    if (i == run_begin || margin < min_margin) {
      min_margin = margin;
    }
  }

  return min_margin;
}

static size_t count_raw_matches_policy(const aria_autotune_point_t *points,
                                       size_t run_begin,
                                       size_t run_end,
                                       const char *policy_impl) {
  size_t count = 0;
  size_t i;

  if (!points || !policy_impl || run_begin > run_end) {
    return 0;
  }

  for (i = run_begin; i <= run_end; ++i) {
    if (strcmp(points[i].raw_winner_impl, policy_impl) == 0) {
      ++count;
    }
  }

  return count;
}

static int build_policy(const aria_autotune_config_t *config,
                        const point_vec_t *points,
                        policy_vec_t *policies) {
  size_t begin = 0;

  if (!config || !points || !policies) {
    return 0;
  }

  while (begin < points->count) {
    size_t end = begin + 1;

    while (end < points->count && points->items[end].key_bits == points->items[begin].key_bits) {
      ++end;
    }

    if (begin < end) {
      size_t run_begin = begin;

      while (run_begin < end) {
        aria_policy_entry_t entry;
        size_t run_end = run_end_for_index(points->items, end, run_begin);
        size_t i;
        int any_refined = 0;
        int any_coarse = 0;
        int path_mixed = 0;
        int raw_mixed = 0;
        char representative_path[64];
        char raw_impl[32];

        memset(&entry, 0, sizeof(entry));
        entry.key_bits = points->items[run_begin].key_bits;
        entry.start_len = points->items[run_begin].length;
        entry.end_len = (run_end + 1 < end) ? (points->items[run_end + 1].length - AUTOTUNE_BLOCK_SIZE) : config->max_len;
        entry.bucket_points = run_end - run_begin + 1;
        copy_string(entry.policy_chosen_impl,
                    sizeof(entry.policy_chosen_impl),
                    points->items[run_begin].basis_impl);
        copy_string(entry.basis_used, sizeof(entry.basis_used), policy_basis_name(config->policy_basis));
        copy_string(entry.policy_basis_metric, sizeof(entry.policy_basis_metric),
                    "trimmed_mean_ns_per_call");

        dominant_path_or_mixed(points->items, run_begin, run_end, representative_path, sizeof(representative_path), &path_mixed);
        dominant_raw_winner(points->items, run_begin, run_end, raw_impl, sizeof(raw_impl), &raw_mixed);
        summarize_raw_winners(points->items,
                              run_begin,
                              run_end,
                              entry.raw_winners_in_segment,
                              sizeof(entry.raw_winners_in_segment));
        entry.min_margin_pct = min_basis_margin_for_run(config, points->items, run_begin, run_end);
        copy_string(entry.raw_chosen_impl, sizeof(entry.raw_chosen_impl), raw_impl);
        copy_string(entry.representative_effective_path,
                    sizeof(entry.representative_effective_path),
                    representative_path);

        for (i = run_begin; i <= run_end; ++i) {
          if (points->items[i].source_is_refined) {
            any_refined = 1;
          } else {
            any_coarse = 1;
          }
          if (points->items[i].note[0] != '\0') {
            append_note(entry.note, sizeof(entry.note), "stabilized");
            break;
          }
        }

        if (any_refined && any_coarse) {
          copy_string(entry.source_phase, sizeof(entry.source_phase), "mixed");
        } else if (any_refined) {
          copy_string(entry.source_phase, sizeof(entry.source_phase), AUTOTUNE_PHASE_REFINED);
        } else {
          copy_string(entry.source_phase, sizeof(entry.source_phase), AUTOTUNE_PHASE_COARSE);
        }

        if (path_mixed) {
          append_note(entry.note, sizeof(entry.note), "mixed_effective_path");
        }
        if (raw_mixed) {
          append_note(entry.note, sizeof(entry.note), "raw_winner_mixed");
        }
        if (count_raw_matches_policy(points->items,
                                     run_begin,
                                     run_end,
                                     entry.policy_chosen_impl) * 2 < entry.bucket_points) {
          append_note(entry.note, sizeof(entry.note), "policy_differs_from_raw_majority");
        }
        if (strcmp(entry.policy_chosen_impl, "ref") == 0 &&
            strcmp(entry.representative_effective_path, "ref_fallback") == 0) {
          append_note(entry.note, sizeof(entry.note), "collapsed_ref_fallback");
        }
        if (strstr(entry.representative_effective_path, "ref_tail") != NULL) {
          append_note(entry.note, sizeof(entry.note), "plus_ref_tail_bucket");
        }
        if (entry.note[0] == '\0') {
          append_note(entry.note, sizeof(entry.note), "stable_bucket");
        }

        if (!policy_vec_push(policies, &entry)) {
          return 0;
        }

        run_begin = run_end + 1;
      }
    }

    begin = end;
  }

  qsort(policies->items, policies->count, sizeof(*policies->items), cmp_policy_key_start);
  return 1;
}

static int validate_and_install_policy(const aria_autotune_config_t *config,
                                       const policy_vec_t *policies) {
  aria_policy_range_t *ranges;
  size_t key_idx;
  size_t i;
  int valid = 1;

  if (!config || !policies || policies->count == 0) {
    return 0;
  }

  ranges = (aria_policy_range_t *)calloc(policies->count, sizeof(*ranges));
  if (!ranges) {
    return 0;
  }

  for (i = 0; i < policies->count; ++i) {
    const aria_policy_entry_t *entry = &policies->items[i];
    const aria_impl_t *impl = find_impl_by_name(entry->policy_chosen_impl);

    if (!impl || (impl->is_supported && !impl->is_supported()) ||
        entry->start_len > entry->end_len ||
        entry->start_len % AUTOTUNE_BLOCK_SIZE != 0 ||
        entry->end_len % AUTOTUNE_BLOCK_SIZE != 0) {
      valid = 0;
      break;
    }
    ranges[i].key_bits = entry->key_bits;
    ranges[i].start_len = entry->start_len;
    ranges[i].end_len = entry->end_len;
    ranges[i].impl = impl;
  }

  for (key_idx = 0; valid && key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
    const int key_bits = autotune_key_bits[key_idx];
    size_t expected_start = config->min_len;
    int found = 0;

    for (i = 0; i < policies->count; ++i) {
      const aria_policy_entry_t *entry = &policies->items[i];
      if (entry->key_bits != key_bits) {
        continue;
      }
      found = 1;
      if (entry->start_len != expected_start) {
        valid = 0;
        break;
      }
      if (entry->end_len > SIZE_MAX - AUTOTUNE_BLOCK_SIZE) {
        valid = 0;
        break;
      }
      expected_start = entry->end_len + AUTOTUNE_BLOCK_SIZE;
    }
    if (!found || expected_start != config->max_len + AUTOTUNE_BLOCK_SIZE) {
      valid = 0;
    }
  }

  if (valid) {
    valid = aria_policy_install(ranges, policies->count);
  }
  free(ranges);

  if (!valid) {
    aria_policy_clear();
    fprintf(stderr, "autotune: generated policy is invalid or incomplete.\n");
  }
  return valid;
}

static int generate_full_search_lengths(const aria_autotune_config_t *config,
                                        size_vec_t *lengths) {
  size_t len;

  if (!config || !lengths) {
    return 0;
  }
  for (len = config->min_len; len <= config->max_len; len += AUTOTUNE_BLOCK_SIZE) {
    if (!size_vec_push_unique(lengths, len)) {
      return 0;
    }
    if (config->max_len - len < AUTOTUNE_BLOCK_SIZE) {
      break;
    }
  }
  return lengths->count > 0;
}

static double measured_time_for_impl(const row_vec_t *rows,
                                     int key_bits,
                                     size_t length,
                                     const char *impl_name) {
  size_t i;
  if (!rows || !impl_name) {
    return -1.0;
  }
  for (i = 0; i < rows->count; ++i) {
    const aria_autotune_row_t *row = &rows->items[i];
    if (row->key_bits == key_bits && row->length == length &&
        strcmp(row->impl_name, impl_name) == 0) {
      return row->trimmed_mean_ns_per_call;
    }
  }
  return -1.0;
}

static int validate_policy_against_full_search(const aria_autotune_config_t *config,
                                               uint8_t *input,
                                               uint8_t *output,
                                               size_t buffer_size,
                                               uint64_t target_ticks,
                                               size_t inner_max,
                                               size_t warmup,
                                               stat_mode_t stat_mode) {
  size_vec_t lengths = {0};
  row_vec_t rows = {0};
  point_vec_t points = {0};
  char detail_path[256];
  char summary_path[256];
  FILE *detail = NULL;
  FILE *summary = NULL;
  double *regrets = NULL;
  double *overall_regrets = NULL;
  size_t *validation_lengths = NULL;
  int *exact_matches = NULL;
  size_t *brute_thresholds = NULL;
  size_t *policy_thresholds = NULL;
  size_t overall_checked = 0;
  size_t overall_matched = 0;
  size_t overall_within_1_count = 0;
  size_t overall_within_3_count = 0;
  size_t overall_regret_count = 0;
  size_t overall_policy_boundary_count = 0;
  size_t overall_reference_boundary_count = 0;
  size_t overall_boundary_observation_count = 0;
  size_t overall_mismatched_point_count = 0;
  size_t overall_mismatched_interval_count = 0;
  size_t overall_longest_mismatched_interval_bytes = 0;
  double overall_boundary_distance_sum = 0.0;
  char overall_mismatch_ranges[2048] = "";
  size_t key_idx;
  int ok = 0;

  if (!generate_full_search_lengths(config, &lengths) ||
      !measure_points(config,
                      &lengths,
                      "brute_force",
                      config->refine_iterations,
                      input,
                      output,
                      buffer_size,
                      target_ticks,
                      inner_max,
                      warmup,
                      stat_mode,
                      &rows,
                      &points)) {
    goto cleanup;
  }
  regrets = (double *)calloc(lengths.count, sizeof(*regrets));
  overall_regrets = (double *)calloc(points.count, sizeof(*overall_regrets));
  validation_lengths = (size_t *)calloc(lengths.count, sizeof(*validation_lengths));
  exact_matches = (int *)calloc(lengths.count, sizeof(*exact_matches));
  brute_thresholds = (size_t *)calloc(lengths.count, sizeof(*brute_thresholds));
  policy_thresholds = (size_t *)calloc(lengths.count, sizeof(*policy_thresholds));
  if (!regrets || !overall_regrets || !validation_lengths || !exact_matches ||
      !brute_thresholds || !policy_thresholds) {
    fprintf(stderr, "autotune: failed to allocate policy validation workspace.\n");
    goto cleanup;
  }
  last_autotune_metrics.exhaustive_candidate_measurement_count = rows.count;
  if (rows.count > 0) {
    last_autotune_metrics.measurement_reduction_percent =
        (1.0 - (double)last_autotune_metrics.autotune_candidate_measurement_count /
                   (double)rows.count) * 100.0;
  }
  if (!build_autotune_output_path(detail_path, sizeof(detail_path), config, "policy_validation.csv") ||
      !build_autotune_output_path(summary_path, sizeof(summary_path), config, "policy_validation_summary.csv")) {
    goto cleanup;
  }

  detail = fopen(detail_path, "w");
  summary = fopen(summary_path, "w");
  if (!detail || !summary) {
    fprintf(stderr, "autotune: failed to open policy validation output: %s\n", strerror(errno));
    goto cleanup;
  }
  fprintf(detail,
          "key_bits,message_length,avx2_chunk_count,avx_chunk_count,ref_tail_block_count,avx2_effective_path,avx2_execution_path,policy_selected_implementation_raw,policy_selected_implementation_normalized,reference_winner_raw,reference_winner_normalized,exact_match,policy_trimmed_mean_ns_per_call,reference_winner_trimmed_mean_ns_per_call,policy_regret_percent,within_1_percent,within_3_percent,ref_trimmed_mean_ns_per_call,linux_aesni_avx_trimmed_mean_ns_per_call,linux_aesni_avx2_trimmed_mean_ns_per_call,linux_gfni_avx512_trimmed_mean_ns_per_call\n");
  fprintf(summary,
          "key_bits,validation_point_count,exact_policy_match_accuracy_percent,within_1_percent_accuracy,within_3_percent_accuracy,mean_policy_regret_percent,median_policy_regret_percent,p95_policy_regret_percent,max_policy_regret_percent,mean_symmetric_boundary_distance_bytes,policy_boundary_count,reference_boundary_count,mismatched_point_count,mismatched_interval_count,longest_mismatched_interval_bytes,mismatched_ranges\n");

  for (key_idx = 0; key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
    const int key_bits = autotune_key_bits[key_idx];
    size_t brute_threshold_count = 0;
    size_t policy_threshold_count = 0;
    size_t checked = 0;
    size_t matched = 0;
    size_t within_1_count = 0;
    size_t within_3_count = 0;
    size_t mismatched_point_count = 0;
    size_t mismatched_interval_count = 0;
    size_t longest_mismatched_interval_bytes = 0;
    char mismatch_ranges[512] = "";
    double boundary_distance = 0.0;
    size_t regret_count = 0;
    size_t i;

    for (i = 0; i < points.count; ++i) {
      const aria_autotune_point_t *point = &points.items[i];
      const aria_impl_t *policy_impl;
      const char *reference_basis_impl;
      const char *policy_normalized;
      double policy_time;
      double reference_time;
      double regret;
      int match;
      aria_execution_path_t avx2_execution_path;
      char avx2_execution_path_text[160];

      if (point->key_bits != key_bits) {
        continue;
      }
      policy_impl = aria_autotuned_dispatch(key_bits, point->length);
      if (!policy_impl) {
        goto cleanup;
      }
      avx2_execution_path = execution_path_for_impl(&aria_linux_aesni_avx2_impl,
                                                    point->length);
      format_execution_path(&avx2_execution_path,
                            avx2_execution_path_text,
                            sizeof(avx2_execution_path_text));
      policy_normalized = normalize_impl_name(config,
                                              policy_impl->name,
                                              effective_path_for_impl_name(policy_impl->name, point->length));
      reference_basis_impl = config->policy_basis == ARIA_POLICY_BASIS_RAW
                                 ? point->raw_winner_impl
                                 : point->policy_winner_impl;
      match = strcmp(reference_basis_impl,
                     config->policy_basis == ARIA_POLICY_BASIS_RAW
                         ? policy_impl->name
                         : policy_normalized) == 0;
      policy_time = measured_time_for_impl(&rows, key_bits, point->length, policy_impl->name);
      reference_time = config->policy_basis == ARIA_POLICY_BASIS_RAW
                           ? point->raw_best_ns_per_call
                           : point->policy_best_ns_per_call;
      if (policy_time <= 0.0 || reference_time <= 0.0 || policy_time + 1e-9 < reference_time) {
        fprintf(stderr, "autotune: inconsistent validation timing at key=%d len=%zu.\n",
                key_bits, point->length);
        goto cleanup;
      }
      regret = ((policy_time - reference_time) / reference_time) * 100.0;
      if (regret_count >= lengths.count || checked >= lengths.count) {
        goto cleanup;
      }
      regrets[regret_count++] = regret;
      validation_lengths[checked] = point->length;
      exact_matches[checked] = match;
      if (policy_time <= reference_time * 1.01) {
        within_1_count++;
      }
      if (policy_time <= reference_time * 1.03) {
        within_3_count++;
      }
      fprintf(detail, "%d,%zu,%zu,%zu,%zu,%s,%s,%s,%s,%s,%s,%d,%.6f,%.6f,%.9f,%d,%d,%.6f,%.6f,%.6f,%.6f\n",
              key_bits,
              point->length,
              avx2_execution_path.avx2_32way_chunk_count,
              avx2_execution_path.avx_16way_chunk_count,
              avx2_execution_path.ref_tail_block_count,
              effective_path_for_impl_name("linux_aesni_avx2", point->length),
              avx2_execution_path_text,
              policy_impl->name,
              policy_normalized,
              point->raw_winner_impl,
              point->policy_winner_impl,
              match,
              policy_time,
              reference_time,
              regret,
              policy_time <= reference_time * 1.01,
              policy_time <= reference_time * 1.03,
              measured_time_for_impl(&rows, key_bits, point->length, "ref"),
              measured_time_for_impl(&rows, key_bits, point->length, "linux_aesni_avx"),
              measured_time_for_impl(&rows, key_bits, point->length, "linux_aesni_avx2"),
              measured_time_for_impl(&rows, key_bits, point->length, "linux_gfni_avx512"));

      if (checked > 0) {
        const aria_autotune_point_t *previous = &points.items[i - 1];
        const char *previous_reference = config->policy_basis == ARIA_POLICY_BASIS_RAW
                                         ? previous->raw_winner_impl
                                         : previous->policy_winner_impl;
        if (strcmp(previous_reference, reference_basis_impl) != 0 &&
            brute_threshold_count < lengths.count) {
          brute_thresholds[brute_threshold_count++] = point->length;
        }
      }

      checked++;
      if (match) {
        matched++;
      }
    }
    {
      policy_mismatch_summary_t mismatch_summary;
      if (!policy_summarize_mismatches(validation_lengths,
                                       exact_matches,
                                       checked,
                                       AUTOTUNE_BLOCK_SIZE,
                                       mismatch_ranges,
                                       sizeof(mismatch_ranges),
                                       &mismatch_summary)) {
        goto cleanup;
      }
      mismatched_point_count = mismatch_summary.mismatched_point_count;
      mismatched_interval_count = mismatch_summary.mismatched_interval_count;
      longest_mismatched_interval_bytes = mismatch_summary.longest_mismatched_interval_bytes;
    }

    for (i = 0; i < aria_policy_count(); ++i) {
      const aria_policy_range_t *entry = aria_policy_entry(i);
      if (entry->key_bits == key_bits && entry->start_len != config->min_len &&
          policy_threshold_count < lengths.count) {
        policy_thresholds[policy_threshold_count++] = entry->start_len;
      }
    }
    boundary_distance = policy_mean_symmetric_boundary_distance(
        policy_thresholds,
        policy_threshold_count,
        brute_thresholds,
        brute_threshold_count,
        config->max_len - config->min_len);

    if (checked == 0 || regret_count == 0) {
      goto cleanup;
    }
    qsort(regrets, regret_count, sizeof(regrets[0]), cmp_double_value);
    {
      const double regret_mean = bench_mean_samples(regrets, regret_count);
      const double regret_median = bench_percentile_from_sorted(regrets, regret_count, 0.50);
      const double regret_p95 = bench_percentile_from_sorted(regrets, regret_count, 0.95);
      const double regret_max = regrets[regret_count - 1];
      if (overall_regret_count + regret_count > points.count) {
        goto cleanup;
      }
      memcpy(overall_regrets + overall_regret_count, regrets, regret_count * sizeof(*regrets));
      overall_regret_count += regret_count;
      overall_checked += checked;
      overall_matched += matched;
      overall_within_1_count += within_1_count;
      overall_within_3_count += within_3_count;
      overall_policy_boundary_count += policy_threshold_count;
      overall_reference_boundary_count += brute_threshold_count;
      overall_boundary_distance_sum +=
          boundary_distance * (double)(policy_threshold_count + brute_threshold_count);
      overall_boundary_observation_count += policy_threshold_count + brute_threshold_count;
      overall_mismatched_point_count += mismatched_point_count;
      overall_mismatched_interval_count += mismatched_interval_count;
      if (longest_mismatched_interval_bytes > overall_longest_mismatched_interval_bytes) {
        overall_longest_mismatched_interval_bytes = longest_mismatched_interval_bytes;
      }
      {
        const size_t used = strlen(overall_mismatch_ranges);
        const int written = snprintf(overall_mismatch_ranges + used,
                                     sizeof(overall_mismatch_ranges) - used,
                                     "%s%d:%s",
                                     used ? ";" : "",
                                     key_bits,
                                     mismatch_ranges);
        if (written < 0 || (size_t)written >= sizeof(overall_mismatch_ranges) - used) {
          goto cleanup;
        }
      }
      fprintf(summary, "%d,%zu,%.3f,%.3f,%.3f,%.9f,%.9f,%.9f,%.9f,%.3f,%zu,%zu,%zu,%zu,%zu,%s\n",
              key_bits,
              checked,
              ((double)matched * 100.0) / (double)checked,
              ((double)within_1_count * 100.0) / (double)checked,
              ((double)within_3_count * 100.0) / (double)checked,
              regret_mean,
              regret_median,
              regret_p95,
              regret_max,
              boundary_distance,
              policy_threshold_count,
              brute_threshold_count,
              mismatched_point_count,
              mismatched_interval_count,
              longest_mismatched_interval_bytes,
              mismatch_ranges);
      printf("Policy Validation key=%d exact_match=%.3f%% mean_regret=%.6f%% boundary_distance=%.3fB mismatched_ranges=%s\n",
             key_bits,
             ((double)matched * 100.0) / (double)checked,
             regret_mean,
             boundary_distance,
             mismatch_ranges);
    }
  }

  if (overall_regret_count == 0 || overall_checked == 0) {
    goto cleanup;
  }
  qsort(overall_regrets, overall_regret_count, sizeof(*overall_regrets), cmp_double_value);
  fprintf(summary, "all,%zu,%.3f,%.3f,%.3f,%.9f,%.9f,%.9f,%.9f,%.3f,%zu,%zu,%zu,%zu,%zu,%s\n",
          overall_checked,
          ((double)overall_matched * 100.0) / (double)overall_checked,
          ((double)overall_within_1_count * 100.0) / (double)overall_checked,
          ((double)overall_within_3_count * 100.0) / (double)overall_checked,
          bench_mean_samples(overall_regrets, overall_regret_count),
          bench_percentile_from_sorted(overall_regrets, overall_regret_count, 0.50),
          bench_percentile_from_sorted(overall_regrets, overall_regret_count, 0.95),
          overall_regrets[overall_regret_count - 1],
          overall_boundary_observation_count
              ? overall_boundary_distance_sum / (double)overall_boundary_observation_count
              : 0.0,
          overall_policy_boundary_count,
          overall_reference_boundary_count,
          overall_mismatched_point_count,
          overall_mismatched_interval_count,
          overall_longest_mismatched_interval_bytes,
          overall_mismatch_ranges);

  ok = 1;

cleanup:
  if (detail) fclose(detail);
  if (summary) fclose(summary);
  row_vec_free(&rows);
  point_vec_free(&points);
  size_vec_free(&lengths);
  free(regrets);
  free(overall_regrets);
  free(validation_lengths);
  free(exact_matches);
  free(brute_thresholds);
  free(policy_thresholds);
  return ok;
}

static int write_rows_csv(const char *path, const row_vec_t *rows) {
  FILE *fp;
  size_t i;

  if (!path || !rows) {
    return 0;
  }

  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }

  fprintf(fp,
          "key_bits,length,impl,effective_path,gfni_64way_chunk_count,avx2_chunk_count,avx_chunk_count,ref_tail_block_count,execution_path,normalized_impl,trimmed_mean_ns_per_call,trimmed_mean_ns_per_byte,raw_mean_ns_per_call,median_ns_per_call,sample_count,is_raw_winner,is_policy_winner,phase\n");
  for (i = 0; i < rows->count; ++i) {
    fprintf(fp, "%d,%zu,%s,%s,%zu,%zu,%zu,%zu,%s,%s,%.6f,%.6f,%.6f,%.6f,%zu,%d,%d,%s\n",
            rows->items[i].key_bits,
            rows->items[i].length,
            rows->items[i].impl_name,
            rows->items[i].effective_path,
            rows->items[i].gfni_64way_chunk_count,
            rows->items[i].avx2_32way_chunk_count,
            rows->items[i].avx_16way_chunk_count,
            rows->items[i].ref_tail_block_count,
            rows->items[i].execution_path,
            rows->items[i].normalized_impl,
            rows->items[i].trimmed_mean_ns_per_call,
            rows->items[i].trimmed_mean_ns_per_byte,
            rows->items[i].raw_mean_ns_per_call,
            rows->items[i].median_ns_per_call,
            rows->items[i].sample_count,
            rows->items[i].is_raw_winner,
            rows->items[i].is_policy_winner,
            rows->items[i].phase);
  }

  fclose(fp);
  return 1;
}

static int write_autotune_metrics_csv(const char *path) {
  FILE *fp;
  if (!path) {
    return 0;
  }
  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }
  fprintf(fp,
          "autotune_search_time_ms,autotune_end_to_end_time_ms,coarse_measurement_point_count,fine_measurement_point_count,autotune_candidate_measurement_count,exhaustive_candidate_measurement_count,measurement_reduction_percent,candidate_measurement_definition\n");
  fprintf(fp, "%.3f,%.3f,%zu,%zu,%zu,%zu,%.6f,key_bits_message_length_implementation\n",
          last_autotune_metrics.search_time_ms,
          last_autotune_metrics.end_to_end_time_ms,
          last_autotune_metrics.coarse_measurement_point_count,
          last_autotune_metrics.fine_measurement_point_count,
          last_autotune_metrics.autotune_candidate_measurement_count,
          last_autotune_metrics.exhaustive_candidate_measurement_count,
          last_autotune_metrics.measurement_reduction_percent);
  fclose(fp);
  return 1;
}

static void write_validation_check(FILE *fp, const char *name, int passed, const char *details) {
  fprintf(fp, "%s,%s,%s\n", name, passed ? "PASS" : "FAIL", details ? details : "");
}

static int write_framework_validation_csv(const char *path,
                                          const aria_autotune_config_t *config) {
  FILE *fp;
  aria_policy_range_t *copy;
  const size_t count = aria_policy_count();
  size_t i;
  int cpu_ok = 1;
  int coverage_ok = 1;
  int overlap_ok = 1;
  int alignment_ok = 1;
  int key_present[AUTOTUNE_KEY_VARIANTS] = {0};
  int fallback_without_policy;
  int fallback_outside_range;

  if (!path || !config || count == 0) {
    return 0;
  }
  copy = (aria_policy_range_t *)malloc(sizeof(*copy) * count);
  if (!copy) {
    return 0;
  }
  for (i = 0; i < count; ++i) {
    const aria_policy_range_t *entry = aria_policy_entry(i);
    copy[i] = *entry;
    if (entry->impl->is_supported && !entry->impl->is_supported()) {
      cpu_ok = 0;
    }
    if (entry->start_len % AUTOTUNE_BLOCK_SIZE != 0 ||
        entry->end_len % AUTOTUNE_BLOCK_SIZE != 0) {
      alignment_ok = 0;
    }
    if (i > 0 && copy[i - 1].key_bits == entry->key_bits) {
      if (copy[i - 1].end_len >= entry->start_len) {
        overlap_ok = 0;
      }
      if (copy[i - 1].end_len + AUTOTUNE_BLOCK_SIZE != entry->start_len) {
        coverage_ok = 0;
      }
    }
    for (size_t key_idx = 0; key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
      if (entry->key_bits == autotune_key_bits[key_idx]) {
        key_present[key_idx] = 1;
      }
    }
  }
  for (size_t key_idx = 0; key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
    size_t first = AUTOTUNE_INVALID_INDEX;
    size_t last = AUTOTUNE_INVALID_INDEX;
    for (i = 0; i < count; ++i) {
      if (copy[i].key_bits == autotune_key_bits[key_idx]) {
        if (first == AUTOTUNE_INVALID_INDEX) first = i;
        last = i;
      }
    }
    if (first == AUTOTUNE_INVALID_INDEX || copy[first].start_len != config->min_len ||
        copy[last].end_len != config->max_len) {
      coverage_ok = 0;
    }
  }

  fallback_outside_range =
      aria_autotuned_dispatch(128, config->max_len + AUTOTUNE_BLOCK_SIZE) ==
      aria_runtime_dispatch(config->max_len + AUTOTUNE_BLOCK_SIZE);
  aria_policy_clear();
  fallback_without_policy = aria_autotuned_dispatch(128, config->min_len) ==
                            aria_runtime_dispatch(config->min_len);
  if (!aria_policy_install(copy, count)) {
    free(copy);
    return 0;
  }
  free(copy);

  fp = fopen(path, "w");
  if (!fp) {
    return 0;
  }
  fprintf(fp, "validation_check,status,details\n");
  write_validation_check(fp, "cpu_feature_compatibility", cpu_ok, "installed implementations supported");
  write_validation_check(fp, "full_range_coverage", coverage_ok, "all 16-byte points covered");
  write_validation_check(fp, "no_overlapping_ranges", overlap_ok, "strictly ordered ranges");
  write_validation_check(fp, "sixteen_byte_alignment", alignment_ok, "range endpoints aligned");
  write_validation_check(fp, "fallback_without_policy", fallback_without_policy, "static heuristic fallback");
  write_validation_check(fp, "fallback_outside_policy_range", fallback_outside_range, "static heuristic fallback");
  write_validation_check(fp, "policy_generation_for_128_bit", key_present[0], "128-bit policy present");
  write_validation_check(fp, "policy_generation_for_192_bit", key_present[1], "192-bit policy present");
  write_validation_check(fp, "policy_generation_for_256_bit", key_present[2], "256-bit policy present");
  fclose(fp);
  return cpu_ok && coverage_ok && overlap_ok && alignment_ok && fallback_without_policy &&
         fallback_outside_range && key_present[0] && key_present[1] && key_present[2];
}

static int write_boundaries_csv(const char *path, const boundary_vec_t *boundaries) {
  FILE *fp;
  size_t i;

  if (!path || !boundaries) {
    return 0;
  }

  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }

  fprintf(fp,
          "key_bits,left_len,right_len,left_raw_winner,right_raw_winner,left_policy_winner,right_policy_winner,left_raw_margin_pct,right_raw_margin_pct,left_policy_margin_pct,right_policy_margin_pct,is_raw_boundary,is_policy_boundary,basis_used,refined,note\n");
  for (i = 0; i < boundaries->count; ++i) {
    fprintf(fp, "%d,%zu,%zu,%s,%s,%s,%s,%.6f,%.6f,%.6f,%.6f,%d,%d,%s,%d,%s\n",
            boundaries->items[i].key_bits,
            boundaries->items[i].left_len,
            boundaries->items[i].right_len,
            boundaries->items[i].left_raw_winner,
            boundaries->items[i].right_raw_winner,
            boundaries->items[i].left_policy_winner,
            boundaries->items[i].right_policy_winner,
            boundaries->items[i].left_raw_margin_pct,
            boundaries->items[i].right_raw_margin_pct,
            boundaries->items[i].left_policy_margin_pct,
            boundaries->items[i].right_policy_margin_pct,
            boundaries->items[i].is_raw_boundary,
            boundaries->items[i].is_policy_boundary,
            boundaries->items[i].basis_used,
            boundaries->items[i].refined,
            boundaries->items[i].note);
  }

  fclose(fp);
  return 1;
}

static int write_scan_points_csv(const char *path,
                                 const point_vec_t *coarse_points,
                                 const point_vec_t *refined_points) {
  FILE *fp;
  size_t i;

  if (!path || !coarse_points || !refined_points) {
    return 0;
  }
  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }
  fprintf(fp, "key_bits,input_len,phase\n");
  for (i = 0; i < coarse_points->count; ++i) {
    fprintf(fp, "%d,%zu,%s\n", coarse_points->items[i].key_bits,
            coarse_points->items[i].length, AUTOTUNE_PHASE_COARSE);
  }
  for (i = 0; i < refined_points->count; ++i) {
    fprintf(fp, "%d,%zu,%s\n", refined_points->items[i].key_bits,
            refined_points->items[i].length, AUTOTUNE_PHASE_REFINED);
  }
  fclose(fp);
  return 1;
}

static int write_policy_csv(const char *path, const policy_vec_t *policies) {
  FILE *fp;
  size_t i;

  if (!path || !policies) {
    return 0;
  }

  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }

  fprintf(fp,
          "key_bits,start_len,end_len,raw_chosen_impl,policy_chosen_impl,representative_effective_path,note,source_phase,basis_used,bucket_points,policy_impl,policy_basis_metric,policy_basis_source,policy_basis,evidence_points,raw_winners_in_segment,min_margin_pct,notes\n");
  for (i = 0; i < policies->count; ++i) {
    fprintf(fp, "%d,%zu,%zu,%s,%s,%s,%s,%s,%s,%zu,%s,%s,%s,%s,%zu,%s,%.6f,%s\n",
            policies->items[i].key_bits,
            policies->items[i].start_len,
            policies->items[i].end_len,
            policies->items[i].raw_chosen_impl,
            policies->items[i].policy_chosen_impl,
            policies->items[i].representative_effective_path,
            policies->items[i].note,
            policies->items[i].source_phase,
            policies->items[i].basis_used,
            policies->items[i].bucket_points,
            policies->items[i].policy_chosen_impl,
            policies->items[i].policy_basis_metric,
            policies->items[i].source_phase,
            policies->items[i].basis_used,
            policies->items[i].bucket_points,
            policies->items[i].raw_winners_in_segment,
            policies->items[i].min_margin_pct,
            policies->items[i].note);
  }

  fclose(fp);
  return 1;
}

static int write_raw_best_by_length_csv(const char *path, const point_vec_t *points) {
  FILE *fp;
  size_t i;

  if (!path || !points) {
    return 0;
  }

  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }

  fprintf(fp,
          "key_bits,input_len,raw_best_impl,raw_best_effective_path,metric,raw_best_value,second_best_impl,second_best_value,margin_pct,policy_basis_source\n");
  for (i = 0; i < points->count; ++i) {
    const aria_autotune_point_t *point = &points->items[i];

    fprintf(fp, "%d,%zu,%s,%s,trimmed_mean_ns_per_call,%.6f,%s,%.6f,%.6f,%s\n",
            point->key_bits,
            point->length,
            point->raw_winner_impl,
            point->raw_effective_path,
            point->raw_best_ns_per_call,
            point->raw_second_impl,
            point->raw_second_ns_per_call,
            point->raw_margin_pct,
            point->source_is_refined ? AUTOTUNE_PHASE_REFINED : AUTOTUNE_PHASE_COARSE);
  }

  fclose(fp);
  return 1;
}

static void write_threshold_summary_json(FILE *fp,
                                         const policy_vec_t *policies,
                                         int key_bits,
                                         int use_policy_impl) {
  const aria_policy_entry_t *entries[8];
  size_t count = 0;
  size_t i;

  for (i = 0; i < policies->count; ++i) {
    if (policies->items[i].key_bits != key_bits) {
      continue;
    }
    if (count < (sizeof(entries) / sizeof(entries[0]))) {
      entries[count] = &policies->items[i];
    }
    ++count;
  }

  if (count == 4) {
    const char *a = use_policy_impl ? entries[0]->policy_chosen_impl : entries[0]->raw_chosen_impl;
    const char *b = use_policy_impl ? entries[1]->policy_chosen_impl : entries[1]->raw_chosen_impl;
    const char *c = use_policy_impl ? entries[2]->policy_chosen_impl : entries[2]->raw_chosen_impl;
    const char *d = use_policy_impl ? entries[3]->policy_chosen_impl : entries[3]->raw_chosen_impl;

    if (strcmp(a, "ref") == 0 &&
        strcmp(b, "linux_aesni_avx") == 0 &&
        strcmp(c, "linux_aesni_avx2") == 0 &&
        strcmp(d, "linux_gfni_avx512") == 0) {
      fprintf(fp,
              "{ \"model\": \"ref->avx->avx2->gfni_avx512\", "
              "\"ref_to_avx_at\": %zu, \"avx_to_avx2_at\": %zu, "
              "\"avx2_to_gfni_avx512_at\": %zu }",
              entries[1]->start_len,
              entries[2]->start_len,
              entries[3]->start_len);
      return;
    }
  }

  if (count == 3) {
    const char *a = use_policy_impl ? entries[0]->policy_chosen_impl : entries[0]->raw_chosen_impl;
    const char *b = use_policy_impl ? entries[1]->policy_chosen_impl : entries[1]->raw_chosen_impl;
    const char *c = use_policy_impl ? entries[2]->policy_chosen_impl : entries[2]->raw_chosen_impl;

    if (strcmp(a, "ref") == 0 &&
        strcmp(b, "linux_aesni_avx") == 0 &&
        strcmp(c, "linux_aesni_avx2") == 0) {
      fprintf(fp,
              "{ \"model\": \"ref->avx->avx2\", \"ref_to_avx_at\": %zu, \"avx_to_avx2_at\": %zu }",
              entries[1]->start_len,
              entries[2]->start_len);
      return;
    }
  }

  if (count == 2) {
    const char *a = use_policy_impl ? entries[0]->policy_chosen_impl : entries[0]->raw_chosen_impl;
    const char *b = use_policy_impl ? entries[1]->policy_chosen_impl : entries[1]->raw_chosen_impl;

    if (strcmp(a, "ref") == 0 && strcmp(b, "linux_aesni_avx") == 0) {
      fprintf(fp,
              "{ \"model\": \"ref->avx\", \"ref_to_avx_at\": %zu }",
              entries[1]->start_len);
      return;
    }
    if (strcmp(a, "ref") == 0 && strcmp(b, "linux_aesni_avx2") == 0) {
      fprintf(fp,
              "{ \"model\": \"ref->avx2\", \"ref_to_avx2_at\": %zu }",
              entries[1]->start_len);
      return;
    }
    if (strcmp(a, "linux_aesni_avx") == 0 && strcmp(b, "linux_aesni_avx2") == 0) {
      fprintf(fp,
              "{ \"model\": \"avx->avx2\", \"avx_to_avx2_at\": %zu }",
              entries[1]->start_len);
      return;
    }
  }

  fprintf(fp, "\"not representable as simple thresholds\"");
}

static int write_policy_json(const char *path,
                             const aria_autotune_config_t *config,
                             const policy_vec_t *policies) {
  FILE *fp;
  size_t key_idx;
  int first_group = 1;

  if (!path || !config || !policies) {
    return 0;
  }

  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "autotune: failed to open %s: %s\n", path, strerror(errno));
    return 0;
  }

  fprintf(fp, "{\n");
  fprintf(fp, "  \"metadata\": {\n");
  fprintf(fp, "    \"profile\": \"%s\",\n", profile_name(config->profile));
  fprintf(fp, "    \"min_len\": %zu,\n", config->min_len);
  fprintf(fp, "    \"max_len\": %zu,\n", config->max_len);
  fprintf(fp, "    \"policy_basis\": \"%s\",\n", policy_basis_name(config->policy_basis));
  fprintf(fp, "    \"tail_policy\": \"%s\",\n", tail_policy_name(config->tail_policy));
  fprintf(fp, "    \"output_level\": \"%s\",\n", output_level_name(config->output_level));
  fprintf(fp, "    \"output_dir\": \"%s\",\n", (config->output_dir && config->output_dir[0] != '\0') ? config->output_dir : "");
  fprintf(fp, "    \"output_prefix\": \"%s\",\n", config->output_prefix ? config->output_prefix : "");
  fprintf(fp, "    \"collapse_ref_fallback\": %s,\n", config->collapse_ref_fallback ? "true" : "false");
  fprintf(fp, "    \"winner_margin_pct\": %.2f,\n", config->winner_margin_pct);
  fprintf(fp, "    \"stability_min_run\": %zu,\n", config->stability_min_run);
  fprintf(fp, "    \"policy_min_bucket_points\": %zu,\n", config->policy_min_bucket_points);
  fprintf(fp, "    \"coarse_grid_rule\": \"log_backbone_x1_x1.5_plus_structural_units_plus_boundaries\",\n");
  fprintf(fp, "    \"coarse_grid_structural_units\": [%u, %u, %u, %u]\n",
          (unsigned int)ARIA_BLOCK_SIZE,
          (unsigned int)ARIA_AESNI_PARALLEL_BLOCK_SIZE,
          (unsigned int)ARIA_AESNI_AVX2_PARALLEL_BLOCK_SIZE,
          (unsigned int)ARIA_GFNI_AVX512_PARALLEL_BLOCK_SIZE);
  fprintf(fp, "  },\n");
  fprintf(fp, "  \"policy\": [\n");

  for (key_idx = 0; key_idx < AUTOTUNE_KEY_VARIANTS; ++key_idx) {
    int key_bits = autotune_key_bits[key_idx];
    size_t i;
    int have_entries = 0;

    for (i = 0; i < policies->count; ++i) {
      if (policies->items[i].key_bits == key_bits) {
        have_entries = 1;
        break;
      }
    }
    if (!have_entries) {
      continue;
    }

    if (!first_group) {
      fprintf(fp, ",\n");
    }
    first_group = 0;
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"key_bits\": %d,\n", key_bits);
    fprintf(fp, "      \"basis_used\": \"%s\",\n", policy_basis_name(config->policy_basis));
    fprintf(fp, "      \"raw_threshold_summary\": ");
    write_threshold_summary_json(fp, policies, key_bits, 0);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"policy_threshold_summary\": ");
    write_threshold_summary_json(fp, policies, key_bits, 1);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"buckets\": [\n");

    {
      int first_bucket = 1;
      for (i = 0; i < policies->count; ++i) {
        if (policies->items[i].key_bits != key_bits) {
          continue;
        }
        if (!first_bucket) {
          fprintf(fp, ",\n");
        }
        first_bucket = 0;
        fprintf(fp,
                "        { \"start_len\": %zu, \"end_len\": %zu, \"raw_chosen_impl\": \"%s\", \"policy_chosen_impl\": \"%s\", \"policy_impl\": \"%s\", \"representative_effective_path\": \"%s\", \"note\": \"%s\", \"notes\": \"%s\", \"source_phase\": \"%s\", \"policy_basis_source\": \"%s\", \"basis_used\": \"%s\", \"policy_basis\": \"%s\", \"policy_basis_metric\": \"%s\", \"bucket_points\": %zu, \"evidence_points\": %zu, \"raw_winners_in_segment\": \"%s\", \"min_margin_pct\": %.6f }",
                policies->items[i].start_len,
                policies->items[i].end_len,
                policies->items[i].raw_chosen_impl,
                policies->items[i].policy_chosen_impl,
                policies->items[i].policy_chosen_impl,
                policies->items[i].representative_effective_path,
                policies->items[i].note,
                policies->items[i].note,
                policies->items[i].source_phase,
                policies->items[i].source_phase,
                policies->items[i].basis_used,
                policies->items[i].basis_used,
                policies->items[i].policy_basis_metric,
                policies->items[i].bucket_points,
                policies->items[i].bucket_points,
                policies->items[i].raw_winners_in_segment,
                policies->items[i].min_margin_pct);
      }
      fprintf(fp, "\n");
    }

    fprintf(fp, "      ]\n");
    fprintf(fp, "    }");
  }

  fprintf(fp, "\n  ]\n");
  fprintf(fp, "}\n");

  fclose(fp);
  return 1;
}

void aria_autotune_config_apply_profile(aria_autotune_config_t *config,
                                        aria_autotune_profile_t profile) {
  if (!config) {
    return;
  }

  config->profile = profile;
  switch (profile) {
    case ARIA_AUTOTUNE_PROFILE_TEST:
      config->min_len = 16u;
      config->max_len = 512u;
      config->coarse_iterations = 15u;
      config->refine_iterations = 25u;
      break;
    case ARIA_AUTOTUNE_PROFILE_FULL:
      config->min_len = 16u;
      config->max_len = 4096u;
      config->coarse_iterations = 21u;
      config->refine_iterations = 41u;
      break;
    default:
      break;
  }
}

void aria_autotune_config_init(aria_autotune_config_t *config) {
  if (!config) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->refine_step = 16u;
  config->refine_buffer = 0u;
  config->trim_ratio = BENCH_DEFAULT_TRIM_RATIO;
  config->winner_margin_pct = 5.0;
  config->stability_min_run = 3u;
  config->policy_min_bucket_points = 3u;
  config->collapse_ref_fallback = 1;
  config->policy_basis = ARIA_POLICY_BASIS_NORMALIZED;
  config->tail_policy = ARIA_TAIL_POLICY_NATIVE;
  config->output_level = ARIA_OUTPUT_LEVEL_DEFAULT;
  config->output_dir = NULL;
  config->output_prefix = "out/autotune";
  aria_autotune_config_apply_profile(config, ARIA_AUTOTUNE_PROFILE_TEST);
}

int aria_autotune_run(const aria_autotune_config_t *config,
                      uint8_t *input,
                      uint8_t *output,
                      size_t buffer_size,
                      uint64_t target_ticks,
                      size_t inner_max,
                      size_t warmup,
                      stat_mode_t stat_mode) {
  row_vec_t coarse_rows = {0};
  row_vec_t refined_rows = {0};
  point_vec_t coarse_points = {0};
  point_vec_t refined_points = {0};
  point_vec_t merged_points = {0};
  boundary_vec_t boundaries = {0};
  policy_vec_t policies = {0};
  size_vec_t coarse_lengths = {0};
  size_vec_t refine_lengths = {0};
  char coarse_csv[256];
  char refined_csv[256];
  char boundaries_csv[256];
  char scan_points_csv[256];
  char raw_best_csv[256];
  char policy_csv[256];
  char policy_json[256];
  char metrics_csv[256];
  char framework_validation_csv[256];
  int ok = 0;
  const double started_at_ms = wall_time_ms();

  last_autotune_duration_ms = 0.0;
  memset(&last_autotune_metrics, 0, sizeof(last_autotune_metrics));

  if (!config || !input || !output) {
    return 0;
  }
  if (config->min_len == 0 || config->max_len < config->min_len) {
    fprintf(stderr, "autotune: invalid length range.\n");
    return 0;
  }
  if (config->min_len % AUTOTUNE_BLOCK_SIZE != 0 ||
      config->max_len % AUTOTUNE_BLOCK_SIZE != 0 ||
      config->refine_step % AUTOTUNE_BLOCK_SIZE != 0) {
    fprintf(stderr, "autotune: lengths and steps must be multiples of %u.\n", AUTOTUNE_BLOCK_SIZE);
    return 0;
  }
  if (config->refine_step == 0 ||
      config->coarse_iterations == 0 ||
      config->refine_iterations == 0 ||
      config->stability_min_run == 0 ||
      config->policy_min_bucket_points == 0) {
    fprintf(stderr, "autotune: steps, iterations, and policy run sizes must be non-zero.\n");
    return 0;
  }
  if (!isfinite(config->winner_margin_pct) || config->winner_margin_pct < 0.0) {
    fprintf(stderr, "autotune: winner margin must be a non-negative finite value.\n");
    return 0;
  }
  if (!isfinite(config->trim_ratio) || config->trim_ratio < 0.0 || config->trim_ratio >= 0.5 ||
      config->coarse_iterations - 2u * (size_t)((double)config->coarse_iterations * config->trim_ratio) < BENCH_MIN_TRIMMED_SAMPLES ||
      config->refine_iterations - 2u * (size_t)((double)config->refine_iterations * config->trim_ratio) < BENCH_MIN_TRIMMED_SAMPLES) {
    fprintf(stderr, "autotune: trim ratio leaves fewer than %u samples.\n",
            BENCH_MIN_TRIMMED_SAMPLES);
    return 0;
  }
  if (config->max_len > buffer_size) {
    fprintf(stderr, "autotune: max_len=%zu exceeds buffer_size=%zu.\n",
            config->max_len,
            buffer_size);
    return 0;
  }

  if (!build_autotune_output_path(raw_best_csv, sizeof(raw_best_csv), config, "raw_best_by_length.csv") ||
      !build_autotune_output_path(scan_points_csv, sizeof(scan_points_csv), config, "autotune_scan_points.csv") ||
      !build_autotune_output_path(policy_csv, sizeof(policy_csv), config, "autotune_policy.csv") ||
      !build_autotune_output_path(policy_json, sizeof(policy_json), config, "autotune_policy.json") ||
      !build_autotune_output_path(metrics_csv, sizeof(metrics_csv), config, "autotune_metrics.csv")) {
    fprintf(stderr, "autotune: failed to build policy output path.\n");
    goto cleanup;
  }

  if (should_emit_autotune_debug(config)) {
    if (!build_autotune_output_path(coarse_csv, sizeof(coarse_csv), config, "autotune_coarse.csv") ||
        !build_autotune_output_path(refined_csv, sizeof(refined_csv), config, "autotune_refined.csv") ||
        !build_autotune_output_path(boundaries_csv, sizeof(boundaries_csv), config, "autotune_boundaries.csv")) {
      fprintf(stderr, "autotune: failed to build debug output path.\n");
      goto cleanup;
    }
  }

  if (!generate_coarse_lengths(config, &coarse_lengths)) {
    fprintf(stderr, "autotune: failed to generate coarse lengths.\n");
    goto cleanup;
  }

  if (!measure_points(config,
                      &coarse_lengths,
                      AUTOTUNE_PHASE_COARSE,
                      config->coarse_iterations,
                      input,
                      output,
                      buffer_size,
                      target_ticks,
                      inner_max,
                      warmup,
                      stat_mode,
                      &coarse_rows,
                      &coarse_points)) {
    goto cleanup;
  }
  last_autotune_metrics.coarse_measurement_point_count = coarse_points.count;

  if (!build_boundary_candidates(config, &coarse_points, &boundaries)) {
    goto cleanup;
  }

  if (!generate_refine_lengths(config, &boundaries, &refine_lengths)) {
    fprintf(stderr, "autotune: failed to generate refine lengths.\n");
    goto cleanup;
  }

  if (refine_lengths.count > 0 &&
      !measure_points(config,
                      &refine_lengths,
                      AUTOTUNE_PHASE_REFINED,
                      config->refine_iterations,
                      input,
                      output,
                      buffer_size,
                      target_ticks,
                      inner_max,
                      warmup,
                      stat_mode,
                      &refined_rows,
                      &refined_points)) {
    goto cleanup;
  }
  last_autotune_metrics.fine_measurement_point_count = refined_points.count;
  last_autotune_metrics.autotune_candidate_measurement_count = coarse_rows.count + refined_rows.count;

  mark_refined_boundaries(&boundaries, &refined_points);

  if (!merge_points(&coarse_points, &refined_points, &merged_points)) {
    goto cleanup;
  }

  stabilize_points(config, &merged_points);

  if (!build_policy(config, &merged_points, &policies)) {
    goto cleanup;
  }

  if (!validate_and_install_policy(config, &policies)) {
    goto cleanup;
  }
  if (started_at_ms > 0.0) {
    last_autotune_metrics.search_time_ms = wall_time_ms() - started_at_ms;
  }

  if ((should_emit_autotune_debug(config) &&
       (!write_rows_csv(coarse_csv, &coarse_rows) ||
        !write_rows_csv(refined_csv, &refined_rows) ||
        !write_boundaries_csv(boundaries_csv, &boundaries))) ||
      !write_scan_points_csv(scan_points_csv, &coarse_points, &refined_points) ||
      !write_raw_best_by_length_csv(raw_best_csv, &merged_points) ||
      !write_policy_csv(policy_csv, &policies) ||
      !write_policy_json(policy_json, config, &policies)) {
    goto cleanup;
  }

  if (started_at_ms > 0.0) {
    last_autotune_duration_ms = wall_time_ms() - started_at_ms;
    last_autotune_metrics.end_to_end_time_ms = last_autotune_duration_ms;
  }

  if (!validate_policy_against_full_search(config,
                                           input,
                                           output,
                                           buffer_size,
                                           target_ticks,
                                           inner_max,
                                           warmup,
                                           stat_mode)) {
    goto cleanup;
  }
  if (!write_autotune_metrics_csv(metrics_csv)) {
    goto cleanup;
  }
  if (!build_autotune_output_path(framework_validation_csv,
                                  sizeof(framework_validation_csv),
                                  config,
                                  "framework_validation_summary.csv") ||
      !write_framework_validation_csv(framework_validation_csv, config)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (last_autotune_duration_ms == 0.0 && started_at_ms > 0.0) {
    last_autotune_duration_ms = wall_time_ms() - started_at_ms;
  }
  if (!ok) {
    aria_policy_clear();
  }
  row_vec_free(&coarse_rows);
  row_vec_free(&refined_rows);
  point_vec_free(&coarse_points);
  point_vec_free(&refined_points);
  point_vec_free(&merged_points);
  boundary_vec_free(&boundaries);
  policy_vec_free(&policies);
  size_vec_free(&coarse_lengths);
  size_vec_free(&refine_lengths);
  return ok;
}

double aria_autotune_last_duration_ms(void) {
  return last_autotune_duration_ms;
}

const aria_autotune_metrics_t *aria_autotune_last_metrics(void) {
  return &last_autotune_metrics;
}
