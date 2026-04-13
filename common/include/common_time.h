#ifndef COMMON_TIME_H
#define COMMON_TIME_H

#include <stdint.h>

uint64_t time_begin(void);
uint64_t time_end(uint64_t start);
uint64_t time_frequency(void);

#endif
