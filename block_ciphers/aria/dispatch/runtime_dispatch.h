#ifndef ARIA_RUNTIME_DISPATCH_H
#define ARIA_RUNTIME_DISPATCH_H

#include <stddef.h>

#include "aria_api.h"

typedef enum scenario {
  SCENARIO_HIGH_PERF_SERVER = 0,
  SCENARIO_LOW_POWER_CLIENT = 1
} scenario_t;

const char *aria_scenario_name(scenario_t scenario);

const aria_impl_t *aria_runtime_dispatch(size_t len);
const aria_impl_t *aria_runtime_dispatch_scenario(size_t len, scenario_t scenario);

#endif
