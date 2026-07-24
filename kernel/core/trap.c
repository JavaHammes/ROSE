#include <stdint.h>

#include "timer.h"
#include "trap.h"
#include "uart.h"

/*
 * __asm__ volatile(
 *   "assembly template"
 *   : output operands
 *   : input operands
 * );
 *
 * "r" means use general purpose register.
 */

/*
 * sstatus.SSIE - Supervisor Interrupt Enable
 *
 * This is the global interrupt-enable bit for supervisor mode.
 *
 * When SIE = 0: No s. interrupts while cpu in s mode.
 * WHen SIE = 1: S interrupts are taken when corresponding inttertups source
 *     		 is also enabled in the sie CSR.
 */
#define SSTATUS_SIE (1UL << 1)

/*
 * STIE - Supervisor Timer Interrupts Enable
 *
 * An interrupt is only enabled when:
 *  1. its corresponding bit in sie is enabled
 *  2. the global sstatus.SIE bit is enabled
 */
#define SIE_STIE (1UL << 5)

/*
 * Bit 63 of scause indicates whether the trap was caused by an interrupt.
 */
#define SCAUSE_INTERRUPT_BIT (UINT64_C(1) << 63)

/*
 * The remaining bits of scause contain the interrupt or exception code.
 */
#define SCAUSE_CODE_MASK (~SCAUSE_INTERRUPT_BIT)

/*
 * Supervisor timer interrupt code.
 * scause[62:0] = 5
 */
#define SCAUSE_SUPERVISOR_TIMER 5UL

/*
 * Assembly trap-routine defined in arch/riscv64/trap.S
 */
extern void trap_entry(void);

/*
 * Configure Supervisor-mode trap handling.
 * This function writes the address of trap_entry into the stvec CSR.
 * We use direct mode because  it is simpler for now.
 */
void trap_init(void) {
        uintptr_t entry = (uintptr_t)trap_entry;
        // uintptr_t stvec_value = entry | 1u; (would set to vectored mode)

        /*
         * Set the Supervisor trap-vector register.
         */
        __asm__ volatile("csrw stvec, %[entry]" : : [entry] "r"(entry));

        /*
         * Enable the timer interrupt.
         * sie = sie | interrupt_mask;
         */
        uintptr_t interrupt_mask = SIE_STIE;
        __asm__ volatile("csrs sie, %0" : : "r"(interrupt_mask));
}

/*
 * Globally enable interrupts while executing in S-mode.
 * sstatus = sstatus | SSTATUS_SIE;
 */
void interrupts_enable(void) {
        __asm__ volatile("csrs sstatus, %[status]"
                         :
                         : [status] "r"(SSTATUS_SIE));
}

/*
 * Globally disable Supervisor interrupts.
 *
 * csrrc clears the selected bits:
 *
 *     sstatus = sstatus & ~SSTATUS_SIE;
 */
void interrupts_disable(void) {
        __asm__ volatile("csrc sstatus, %[mask]"
                         :
                         : [mask] "r"(SSTATUS_SIE)
                         : "memory");
}

/*
 * High-level Supervisor-mode trap handler.
 *
 * trap_entry saves all general-purpose registers and relevant Supervisor CSRs
 * into a struct trap_frame, then passes the address of that frame in a0.
 *
 * The handler inspects frame->scause to determine whether the traps was:
 *   - an exception
 *   - an interrupt
 *
 * The handler may modify the saved frame before returning. F. ex.:
 *   - advance frame-sepc after handling and ecall
 *   - place a syscall result in frame->a0
 *   - change the saved context during task switching.
 */
void trap_handler(struct trap_frame *frame) {
        uint64_t scause = frame->scause;

        /*
         * If interrupt:
         */
        if ((scause & SCAUSE_INTERRUPT_BIT) != 0U) {
                uint64_t interrupt_code = scause & SCAUSE_CODE_MASK;

                switch (interrupt_code) {
                case SCAUSE_SUPERVISOR_TIMER:
                        uart_puts("Timer interrupt handled\n");

                        /*
                         * Schedule the next deadline before returning.
                         */
                        timer_schedule_next();
                        return;
                default:
                        uart_puts("Unhandled interrupt\n");
                        break;
                }
                /*
                 * If exception:
                 */
        } else {
                uart_puts("Unhandled exception\n");
        }

        /*
         * Stop on unhandled traps.
         */
        for (;;) {
                __asm__ volatile("wfi");
        }
}
