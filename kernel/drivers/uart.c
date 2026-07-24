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
 * Register index of the THR "Transmit Holding Register".
 *
 * Writing one byte to this register askas the UART to transmit the byte.
 *
 * Read register 0 -> receive a byte
 * Write register 0 -> transmit a byte
 */
enum { UART_THR = 0 };

/*
 * Register index of the LSR "Line Status Register".
 *
 * Contains several status bits describing the current state of the UART
 * f. ex.:
 *
 * - received data is availabe
 * - an overrun error occuredd
 * - a parity error occured
 * - the transmitter can accept another byte
 * - the transmitter is completely idle
 *
 * See: https://onlinedocs.microchip.com/oxy/GUID-199548F4-607C-436B-
 * 		80C7-E4F280C1CAD2-en-US-1/GUID-F8EF8569-5F58-4F15-801E-
 * 		48A11141D672.html
 */
enum { UART_LSR = 5 };

/*
 * Bit mask for the Transmit Holding Register Empty flag.
 *
 * Bit 5 of the LSR is called THRE "Transmit Holding Register Empty".
 *
 * When this bit is 1, the UART is ready to accept another byte for
 * transmission.
 */
enum { UART_LSR_THRE = (1U << 5) };

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
