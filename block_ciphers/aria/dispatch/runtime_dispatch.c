#include "runtime_dispatch.h"
#include "cpu_features.h"

#include <stddef.h>

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
