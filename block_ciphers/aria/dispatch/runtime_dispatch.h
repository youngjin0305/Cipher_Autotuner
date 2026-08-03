#ifndef ARIA_RUNTIME_DISPATCH_H
#define ARIA_RUNTIME_DISPATCH_H

#include <stddef.h>

#include "aria_api.h"

typedef enum scenario {
  SCENARIO_HIGH_PERF_SERVER = 0,
  SCENARIO_LOW_POWER_CLIENT = 1
} scenario_t;

typedef struct aria_policy_range {
  int key_bits;
  size_t start_len;
  size_t end_len;
  const aria_impl_t *impl;
} aria_policy_range_t;

const char *aria_scenario_name(scenario_t scenario);

const aria_impl_t *aria_runtime_dispatch(size_t len);
const aria_impl_t *aria_runtime_dispatch_scenario(size_t len, scenario_t scenario);

/* The installed policy owns no external memory; entries are copied on success. */
int aria_policy_install(const aria_policy_range_t *entries, size_t count);
void aria_policy_clear(void);
size_t aria_policy_count(void);
const aria_policy_range_t *aria_policy_entry(size_t index);
const aria_impl_t *aria_autotuned_dispatch(int key_bits, size_t len);

#endif
