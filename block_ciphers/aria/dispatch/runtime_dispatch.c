#include "runtime_dispatch.h"
#include "cpu_features.h"

#include <stddef.h>

extern const aria_impl_t aria_avx2_impl;

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

  if (len >= 64 && aria_cpu_has_avx2()) {
    return &aria_avx2_impl;
  }

  return &aria_ref_impl;
}

const aria_impl_t *aria_runtime_dispatch(size_t len) {
  return aria_runtime_dispatch_scenario(len, SCENARIO_HIGH_PERF_SERVER);
}
