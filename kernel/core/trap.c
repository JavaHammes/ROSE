#include <stdint.h>

#include "trap.h"
#include "uart.h"

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
         * __asm__ volatile(
         *   "assembly template"
         *   : output operands
         *   : input operands
         * );
         *
         * "r" means use general purpose register.
         */
        __asm__ volatile("csrw stvec, %[entry]" : : [entry] "r"(entry));
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
 *   - chagne the saved context during task switching.
 */
void trap_handler(struct trap_frame *frame) {
        (void)frame;

        uart_puts("trap\n");

        for (;;) {
                __asm__ volatile("wfi");
        }
}
