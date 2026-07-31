#include "interrupt.h"

/*
 * Globally enable interrupts while executing in S-mode.
 *
 * csrs sets the selected bits.
 *
 */
void global_interrupts_enable(void) {
        __asm__ volatile("csrs sstatus, %[status]"
                         :
                         : [status] "r"(SSTATUS_SIE));
}

/*
 * Globally disable Supervisor interrupts.
 *
 * csrc clears the selected bits:
 *
 *     sstatus = sstatus & ~SSTATUS_SIE;
 */
void global_interrupts_disable(void) {
        __asm__ volatile("csrc sstatus, %[mask]"
                         :
                         : [mask] "r"(SSTATUS_SIE)
                         : "memory");
}

/*
 * Enable timer interrupts.
 */
void timer_interrupts_enable(void) {
        __asm__ volatile("csrs sie, %[status]" : : [status] "r"(SIE_STIE));
}

/*
 * Disable timer interrupts.
 */
void timer_interrupts_disable(void) {
        __asm__ volatile("csrc sie, %[status]" : : [status] "r"(SIE_STIE));
}

/*
 * Enable external interrupts.
 */
void external_interrupts_enable(void) {
        __asm__ volatile("csrs sie, %[status]" : : [status] "r"(SIE_SEIE));
}

/*
 * Disable external interrupts.
 */
void external_interrupts_disable(void) {
        __asm__ volatile("csrc sie, %[status]" : : [status] "r"(SIE_SEIE));
}
