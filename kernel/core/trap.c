#include <stdint.h>

#include "trap.h"

extern void trap_entry(void);

void trap_init(void) {
        uintptr_t entry = (uintptr_t)trap_entry;

        /*
         * stvec[1:0] = 0 selects direct mode.
         */
        __asm__ volatile("csrw stvec, %0" : : "r"(entry) : "memory");
}
