/*
 * UART stands for: "Universal Asynchronous Receiver-Transmitter"
 * and is a physical hardware device (NOT a protocol) used to transmit and
 * receive serial data.
 * Serial data transmission is the process of sending data sequentially.
 * UARTs frame each packet of data with start and stop bits,
 * which informs receiving UARTs of when to start and stop reading data.
 *
 * They are also built into the "virt" machine.
 *
 * ```
 * # Install 'dtc' if you don't already have it.
 * # I use 'brew' for MacOS - you may need to do something else.
 * brew install dtc
 * # Use qemu to dump info about the 'virt' machine in dtb (device tree blob)
 * # format.
 * # The data in this file represents hardware components of a given
 * # machine / device / board.
 * qemu-system-riscv64 -machine virt -machine dumpdtb=riscv64-virt.dtb
 * # Convert our .dtb into a human-readable .dts (device tree source) file.
 * dtc -I dtb -O dts -o riscv64-virt.dts riscv64-virt.dtb
 * # Search for 'serial'.
 * grep serial riscv64-virt.dts
 * ```
 *
 * Source: https://twilco.github.io/riscv-from-scratch/2019/07/08/riscv-from-
 *         scratch-3.html
 */
#include "uart.h"

/*
 * Memory base address of the UART device in QEMU's RISC-V "virt" machine.
 * Can be found out by executing the command from above.
 */
#define UART_BASE 0x10000000UL

/*
 * The NS16550A UART exposes serveral registers beginning at UART_BASE.
 * Each register is located at a particular offset from the base address.
 *
 * UART_BASE + 0 -> Transmit Holding Register when writing
 * UART_BASE + 0 -> Receiver Buffer Register when reading
 * UART_BASE + 5 -> Line Status Register
 */

/*
 * NS16550A register offsets.
 *
 * Some UART registers share the same offset. Their meaning depends on whether
 * the CPU reads from or writes to the register.
 *
 * Offset 0:
 *
 *     read  -> Receiver Buffer Register
 *     write -> Transmit Holding Register
 *
 * Offset 1:
 *
 *     read/write -> Interrupt Enable Register
 *
 * Offset 5:
 *
 *     read -> Line Status Register
 */
enum { UART_RBR = 0, UART_THR = 0, UART_IER = 1, UART_LSR = 5 };

/*
 * Interrupt Enable Register bits.
 *
 * Setting UART_IER_RX_AVAILABLE enables an interrupt whenever received data
 * becomes available in the UART receive buffer.
 */
enum { UART_IER_RX_AVAILABLE = (1U << 0) };

/*
 * Line Status Register bits.
 *
 * UART_LSR_DATA_READY:
 *     At least one received byte is available in the receive buffer.
 *
 * UART_LSR_THRE:
 *     The Transmit Holding Register is empty and can accept another byte.
 */
enum { UART_LSR_DATA_READY = (1U << 0), UART_LSR_THRE = (1U << 5) };

static volatile unsigned char *const uart = (volatile unsigned char *)UART_BASE;

/*
 * Transmit one character through the UART. (Polling)
 *
 * uart_putc('A') does this:
 * 1. Read address 0x10000005.
 * 2. Check whether bit 5 is set.
 * 3. Repeat until bit 5 becomes 1.
 * 4. Write 0x41, the ASCII code for 'A', to address 0x10000000.
 * 5. QEMU displays A in the terminal.
 */
void uart_putc(char c) {
        /*
         * Wait until the UART can accept another byte.
         * (When THRE is 1)
         */
        while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
        }

        /*
         * Write the character to the Transmit Holding Register.
         */
        uart[UART_THR] = (unsigned char)c;
}

/*
 * Write a null-terminated string to the UART.
 */
void uart_puts(const char *str) {

        while (*str != '\0') {

                if (*str == '\n') {
                        /*
                         * Without '\r', a terminal may move downward but keep
                         * the current horizontal cursor position.
                         */
                        uart_putc('\r');
                }

                /*
                 * Send the current character.
                 */
                uart_putc(*str);
                str++;
        }
}

/*
 * Print one hexadecimal digit.
 */
void uart_put_hex_digit(uint8_t digit) {
        static const char digits[] = "0123456789abcdef";

        uart_putc(digits[digit & UINT8_C(0x0f)]);
}

/*
 * Print a 64-bit unsigned integer in hexadecimal
 */
void uart_put_hex64(uint64_t value) {
        uart_puts("0x");

        for (int shift = 60; shift >= 0; shift -= 4) {
                uint8_t digit =
                    (uint8_t)((value >> (unsigned int)shift) & UINT64_C(0x0f));

                uart_put_hex_digit(digit);
        }
}

/*
 * Enable UART receive interrupts.
 *
 * This configures the UART itself to raise an interrupt when received data
 * becomes available.
 *
 * The interrupt will only reach the CPU if the remaining interrupt path is
 * also configured:
 *
 * - UART interrupt source enabled in the PLIC
 * - PLIC priority greater than zero
 * - PLIC threshold permits the interrupt
 * - supervisor external interrupts enabled in sie.SEIE
 * - supervisor interrupts globally enabled in sstatus.SIE
 */
void uart_interrupts_enable(void) { uart[UART_IER] = UART_IER_RX_AVAILABLE; }

/*
 * Handle a UART receive interrupt.
 *
 * The handler drains every byte currently available in the receive buffer.
 * Reading UART_RBR removes one byte from the UART.
 *
 * Once no bytes remain:
 *
 * - UART_LSR_DATA_READY becomes zero
 * - the UART stops asserting the receive interrupt
 * - the interrupt can safely be completed in the PLIC
 *
 * The current implementation echoes each received byte back to the terminal.
 * This is useful for testing keyboard input.
 *
 */
void uart_handle_interrupt(void) {
        /*
         * Drain every byte currently available in the receive FIFO.
         *
         * Reading UART_RBR removes one received byte from the UART.
         * Once no bytes remain, UART_LSR_DATA_READY becomes zero and
         * the receive interrupt condition is cleared.
         */
        while ((uart[UART_LSR] & UART_LSR_DATA_READY) != 0U) {
                char character = (char)uart[UART_RBR];

                /*
                 * Temporary behavior: echo the received character.
                 */
                uart_putc(character);
        }
}
