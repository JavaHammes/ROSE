#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

uint64_t read_time(void);
uint64_t timer_monotonic_nanoseconds(void);
void timer_schedule_next(void);

#endif
