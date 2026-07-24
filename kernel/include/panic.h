#ifndef PANIC_H
#define PANIC_H

#include "trap.h"

_Noreturn void panic(const char *message);
_Noreturn void panic_trap(const char *message, const struct trap_frame *frame);

#endif
