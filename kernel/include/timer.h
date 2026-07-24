#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

uint64_t read_time(void);
void timer_schedule_next(void);

#endif
