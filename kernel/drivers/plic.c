/*
 * Platform-Level Interrupt Controller (PLIC) driver for QEMU's RISC-V
 * "virt" machine.
 *
 * External devices such as the UART generate interrupts when they require
 * CPU attention. The PLIC collects these requests, applies priorities, and
 * forwards eligible interrupts to a CPU hart.
 *
 * When the CPU receives a supervisor external interrupt, the kernel uses this
 * driver to:
 *
 * 1. Claim the interrupt and obtain its source ID.
 * 2. Call the corresponding device interrupt handler.
 * 3. Complete the interrupt after the device has been serviced.
 *
 * Example:
 *
 *     UART receives input
 *         -> PLIC forwards an external interrupt
 *         -> kernel claims UART interrupt ID 10
 *         -> UART handler reads the byte
 *         -> kernel completes the interrupt
 *
 * This file provides functions for configuring priorities, enabling interrupt
 * sources, claiming pending interrupts, and completing handled interrupts.
 *
 * Timer and software interrupts do not pass through the PLIC.
 *
 * The addresses and context IDs used here are specific to the single-hart
 * QEMU "virt" machine and should eventually be read from the device tree.
 */

#include <stdint.h>

#include "plic.h"

/*
 * Base address of the Platform-Level Interrupt Controller on QEMU's
 * RISC-V "virt" machine.
 *
 * The PLIC is accessed through memory-mapped I/O. Reading from or writing to
 * addresses inside this region communicates directly with the interrupt
 * controller.
 */
#define PLIC_BASE 0x0c000000UL

/*
 * Offset of the interrupt-priority register block.
 *
 * Each interrupt source has one 32-bit priority register:
 *
 *     priority_address = PLIC_BASE
 *                      + PLIC_PRIORITY_BASE
 *                      + interrupt_id * 4
 *
 * Interrupt source 0 is reserved. A priority of zero disables an interrupt
 * source. Larger values represent higher priorities.
 */
#define PLIC_PRIORITY_BASE 0x000000UL

/*
 * Offset of the interrupt-enable register block.
 *
 * Every PLIC context has its own set of enable bits. One bit corresponds to
 * one interrupt source:
 *
 *     bit 0  -> interrupt source 0
 *     bit 1  -> interrupt source 1
 *     ...
 *     bit 31 -> interrupt source 31
 *
 * Interrupt sources above 31 continue in the next 32-bit register.
 */
#define PLIC_ENABLE_BASE 0x002000UL

/*
 * Offset of the per-context control register block.
 *
 * Each context contains:
 *
 *     offset 0x0 -> priority threshold
 *     offset 0x4 -> claim/complete register
 */
#define PLIC_CONTEXT_BASE 0x200000UL

/*
 * Distance in bytes between the enable-register blocks of two consecutive
 * PLIC contexts.
 */
#define PLIC_ENABLE_STRIDE 0x80UL

/*
 * Distance in bytes between the control-register blocks of two consecutive
 * PLIC contexts.
 */
#define PLIC_CONTEXT_STRIDE 0x1000UL

/*
 * PLIC context used by hart 0 while running in supervisor mode.
 *
 * On QEMU's single-hart RISC-V "virt" machine, the usual mapping is:
 *
 *     context 0 -> hart 0 machine mode
 *     context 1 -> hart 0 supervisor mode
 *
 * This value is platform-specific and should eventually be obtained from the
 * device tree instead of being hard-coded.
 */
#define PLIC_S_CONTEXT 1UL

/*
 * Register offsets inside one context's control-register block.
 */
#define PLIC_THRESHOLD_OFFSET 0x0UL
#define PLIC_CLAIM_COMPLETE_OFFSET 0x4UL

/*
 * Convert a physical MMIO address into a pointer to a 32-bit PLIC register.
 *
 * volatile is required because the pointed-to memory represents hardware
 * registers. Reads and writes must not be removed, cached, or reordered like
 * accesses to ordinary memory.
 */
static inline volatile uint32_t *plic_register(uintptr_t address) {
        return (volatile uint32_t *)address;
}

/*
 * Calculate the address of the enable register containing the bit for a
 * particular interrupt source.
 *
 * Each enable register contains 32 interrupt-enable bits. Therefore:
 *
 *     interrupt IDs 0-31   -> enable word 0
 *     interrupt IDs 32-63  -> enable word 1
 *     interrupt IDs 64-95  -> enable word 2
 *
 * Dividing interrupt_id by 32 selects the correct 32-bit word. Multiplying by
 * sizeof(uint32_t) converts the word index into a byte offset.
 */
static uintptr_t plic_enable_address(uint32_t interrupt_id) {
        uintptr_t word_offset =
            ((uintptr_t)interrupt_id / 32U) * sizeof(uint32_t);

        return PLIC_BASE + PLIC_ENABLE_BASE +
               PLIC_S_CONTEXT * PLIC_ENABLE_STRIDE + word_offset;
}

/*
 * Calculate the address of a register inside the selected supervisor
 * context's control-register block.
 */
static uintptr_t plic_context_address(uintptr_t register_offset) {
        return PLIC_BASE + PLIC_CONTEXT_BASE +
               PLIC_S_CONTEXT * PLIC_CONTEXT_STRIDE + register_offset;
}

/*
 * Set the priority of one interrupt source.
 *
 * A priority of zero disables the interrupt source globally. Any non-zero
 * priority makes the source eligible for delivery, provided that it is also
 * enabled for the selected context and exceeds the context's threshold.
 */
void plic_set_priority(uint32_t interrupt_id, uint32_t priority) {
        uintptr_t address = PLIC_BASE + PLIC_PRIORITY_BASE +
                            (uintptr_t)interrupt_id * sizeof(uint32_t);

        *plic_register(address) = priority;
}

/*
 * Enable an interrupt source for the hart 0 supervisor-mode context.
 *
 * The PLIC stores enable states as a bit field. First, locate the 32-bit word
 * containing the interrupt's enable bit. Then construct the corresponding bit
 * mask and set that bit without changing the other interrupt-enable bits.
 */
void plic_enable(uint32_t interrupt_id) {
        uintptr_t address = plic_enable_address(interrupt_id);

        /*
         * interrupt_id % 32 selects the bit position inside the selected
         * 32-bit enable register.
         */
        uint32_t bit = UINT32_C(1) << (interrupt_id % UINT32_C(32));

        *plic_register(address) |= bit;
}

/*
 * Disable an interrupt source for the hart 0 supervisor-mode context.
 *
 * The complement of the bit mask contains a zero at the desired interrupt's
 * position and ones everywhere else. ANDing with it clears only that bit.
 */
void plic_disable(uint32_t interrupt_id) {
        uintptr_t address = plic_enable_address(interrupt_id);
        uint32_t bit = UINT32_C(1) << (interrupt_id % UINT32_C(32));

        *plic_register(address) &= ~bit;
}

/*
 * Claim the highest-priority pending interrupt for this context.
 *
 * Reading the claim register performs two operations:
 *
 * 1. It returns the ID of the highest-priority pending enabled interrupt.
 * 2. It marks that interrupt as claimed by this context.
 *
 * A return value of zero means that no interrupt is currently available.
 */
uint32_t plic_claim(void) {
        uintptr_t address = plic_context_address(PLIC_CLAIM_COMPLETE_OFFSET);

        return *plic_register(address);
}

/*
 * Inform the PLIC that an interrupt has finished being serviced.
 *
 * The kernel must write the same interrupt ID that was previously returned by
 * plic_claim(). This allows the PLIC to deliver the interrupt again if the
 * device later raises another request.
 *
 * The device's own interrupt condition should normally be cleared before this
 * function is called.
 */
void plic_complete(uint32_t interrupt_id) {
        uintptr_t address = plic_context_address(PLIC_CLAIM_COMPLETE_OFFSET);

        *plic_register(address) = interrupt_id;
}

/*
 * Initialize the PLIC for the current single-hart supervisor-mode kernel.
 *
 * This function configures the context threshold and enables the UART0
 * interrupt source.
 */
void plic_init(void) {
        /*
         * The threshold blocks interrupts whose priority is less than or equal
         * to the threshold value.
         *
         * Setting the threshold to zero permits every enabled interrupt with a
         * non-zero priority to be delivered to this context.
         */
        uintptr_t threshold_address =
            plic_context_address(PLIC_THRESHOLD_OFFSET);

        *plic_register(threshold_address) = UINT32_C(0);

        /*
         * Give UART0 the lowest active priority.
         *
         * Priority zero would disable the source, so priority one is the
         * smallest usable value.
         */
        plic_set_priority(PLIC_IRQ_UART0, UINT32_C(1));

        /*
         * Enable UART0 for hart 0's supervisor-mode PLIC context.
         */
        plic_enable(PLIC_IRQ_UART0);
}
