#include "panic.h"
#include "interrupt.h"
#include "trap.h"
#include "uart.h"

/*
 * Halt the current hart permanently.
 *
 * Note: _Noreturn tells the compiler that execution can never
 * continue past a call to this function.
 */
static _Noreturn void halt(void) {
        for (;;) {
                __asm__ volatile("wfi");
        }
}

/*
 * Print one named register or CSR value.
 */
static void print_register(const char *name, uint64_t value) {
        uart_puts("  ");
        uart_puts(name);
        uart_puts(": ");
        uart_put_hex64(value);
        uart_puts("\n");
}

/*
 * Stop the kernel because of a fatal error that is not a
 * specific trap frame.
 */
_Noreturn void panic(const char *message) {
        global_interrupts_disable();

        uart_puts("\n*** KERNEL PANIC ***\n");
        uart_puts(message);
        uart_puts("\n");

        halt();
}

/*
 * Stop the kernel because of a fatal trap.
 *
 * scause: identifies the interrupt or exception
 * sepc: address of the interrupted or faulting instruction
 * stval: additional trap-specific information
 * sstatus: supervisor status at the time of the trap
 */
_Noreturn void panic_trap(const char *message, const struct trap_frame *frame) {
        global_interrupts_disable();

        uart_puts("\n*** KERNEL PANIC ***\n");
        uart_puts(message);
        uart_puts("\n");

        print_register("scause ", frame->scause);
        print_register("sepc   ", frame->sepc);
        print_register("stval  ", frame->stval);
        print_register("sstatus", frame->sstatus);

        halt();
}
