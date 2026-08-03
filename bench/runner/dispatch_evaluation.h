#ifndef DISPATCH_EVALUATION_H
#define DISPATCH_EVALUATION_H

#include <stddef.h>
#include <stdint.h>

#include "autotune.h"
#include "runtime_dispatch.h"

int aria_run_dispatch_evaluation(const aria_autotune_config_t *config,
                                 uint8_t *input,
                                 uint8_t *output,
                                 size_t buffer_size,
                                 uint64_t target_ticks,
                                 size_t inner_max,
                                 size_t warmup,
                                 size_t outer,
                                 stat_mode_t stat_mode,
                                 double autotune_time_ms,
                                 scenario_t scenario);

#endif
