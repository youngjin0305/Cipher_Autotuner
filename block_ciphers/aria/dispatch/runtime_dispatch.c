#include "runtime_dispatch.h"
#include "cpu_features.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static aria_policy_range_t *installed_policy;
static size_t installed_policy_count;

static int valid_key_bits(int key_bits) {
  return key_bits == 128 || key_bits == 192 || key_bits == 256;
}

static int impl_is_available(const aria_impl_t *impl) {
  return impl && (!impl->is_supported || impl->is_supported());
}

const char *aria_scenario_name(scenario_t scenario) {
  switch (scenario) {
    case SCENARIO_HIGH_PERF_SERVER:
      return "server";
    case SCENARIO_LOW_POWER_CLIENT:
      return "lowpower";
    default:
      return "unknown";
  }
}

const aria_impl_t *aria_runtime_dispatch_scenario(size_t len, scenario_t scenario) {
  if (scenario == SCENARIO_LOW_POWER_CLIENT) {
    return &aria_ref_impl;
  }

  if (len >= 1024 &&
      (!aria_linux_gfni_avx512_impl.is_supported ||
       aria_linux_gfni_avx512_impl.is_supported())) {
    return &aria_linux_gfni_avx512_impl;
  }

  if (len >= 512 && aria_cpu_has_aesni_avx2()) {
    return &aria_linux_aesni_avx2_impl;
  }

  if (len >= 256 && aria_cpu_has_aesni_avx()) {
    return &aria_linux_aesni_avx_impl;
  }

  return &aria_ref_impl;
}

const aria_impl_t *aria_runtime_dispatch(size_t len) {
  return aria_runtime_dispatch_scenario(len, SCENARIO_HIGH_PERF_SERVER);
}

int aria_policy_install(const aria_policy_range_t *entries, size_t count) {
  aria_policy_range_t *replacement;
  size_t i;

  if (!entries || count == 0 || count > SIZE_MAX / sizeof(*replacement)) {
    return 0;
  }

  for (i = 0; i < count; ++i) {
    const aria_policy_range_t *entry = &entries[i];

    if (!valid_key_bits(entry->key_bits) ||
        entry->start_len == 0 ||
        entry->start_len > entry->end_len ||
        entry->start_len % ARIA_BLOCK_SIZE != 0 ||
        entry->end_len % ARIA_BLOCK_SIZE != 0 ||
        !impl_is_available(entry->impl)) {
      return 0;
    }
    if (i > 0) {
      const aria_policy_range_t *previous = &entries[i - 1];
      if (entry->key_bits < previous->key_bits ||
          (entry->key_bits == previous->key_bits &&
           (previous->end_len > SIZE_MAX - ARIA_BLOCK_SIZE ||
            entry->start_len != previous->end_len + ARIA_BLOCK_SIZE))) {
        return 0;
      }
    }
  }

  replacement = (aria_policy_range_t *)malloc(count * sizeof(*replacement));
  if (!replacement) {
    return 0;
  }
  memcpy(replacement, entries, count * sizeof(*replacement));
  free(installed_policy);
  installed_policy = replacement;
  installed_policy_count = count;
  return 1;
}

void aria_policy_clear(void) {
  free(installed_policy);
  installed_policy = NULL;
  installed_policy_count = 0;
}

size_t aria_policy_count(void) {
  return installed_policy_count;
}

const aria_policy_range_t *aria_policy_entry(size_t index) {
  return (index < installed_policy_count) ? &installed_policy[index] : NULL;
}

const aria_impl_t *aria_autotuned_dispatch(int key_bits, size_t len) {
  size_t i;

  for (i = 0; i < installed_policy_count; ++i) {
    const aria_policy_range_t *entry = &installed_policy[i];
    if (entry->key_bits == key_bits && len >= entry->start_len && len <= entry->end_len) {
      return impl_is_available(entry->impl) ? entry->impl : aria_runtime_dispatch(len);
    }
  }

  /* A missing/out-of-range policy must never make encryption unavailable. */
  return aria_runtime_dispatch(len);
}
