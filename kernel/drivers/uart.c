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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "scheduler.h"
#include "uart.h"
#include "user_process.h"

/*
 * Memory base address of the UART device in QEMU's RISC-V "virt" machine.
 * Can be found out by executing the command from above.
 */
#define UART_REGISTERS ((volatile unsigned char *)platform_uart_base())

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
enum {
        UART_IER_RX_AVAILABLE = (1U << 0),
        UART_IER_TX_EMPTY = (1U << 1),
};

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

enum { UART_UINT64_DECIMAL_DIGITS = 20 };

/*
 * Software receive ring buffer.
 *
 * Characters can arrive from the UART at any time. The UART interrupt handler
 * must therefore store each received character somewhere until normal kernel
 * code is ready to process it.
 *
 * This array is used as a circular, or ring, buffer:
 *
 *     write index -> position where the interrupt handler stores the next byte
 *     read index  -> position where uart_getc() reads the next byte
 *
 * When either index reaches the end of the array, it wraps back to zero.
 *
 * One array entry is intentionally left unused. This makes it possible to
 * distinguish between:
 *
 *     read index == write index      -> buffer is empty
 *     next write index == read index -> buffer is full
 *
 * This implementation stores at most UART_RX_BUFFER_SIZE - 1 characters.
 */
#define UART_RX_BUFFER_SIZE 128U

static char uart_rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t uart_rx_read_index;
static volatile uint32_t uart_rx_write_index;
static bool uart_tx_in_flight;
static uint8_t uart_interrupt_enable = UART_IER_RX_AVAILABLE;

/*
 * Return the next position in the circular buffer.
 *
 * The modulo operation wraps the index back to zero after the last array
 * element:
 *
 *     0 -> 1 -> 2 -> ... -> 127 -> 0
 */
static uint32_t uart_rx_next_index(uint32_t index) {
        return (index + UINT32_C(1)) % UART_RX_BUFFER_SIZE;
}

/*
 * Insert one received character into the ring buffer.
 *
 * This function is called from the UART interrupt handler.
 *
 * Returns:
 *
 *     true  -> the character was stored successfully
 *     false -> the buffer was full and the character was dropped
 */
static bool uart_rx_buffer_push(char character) {
        uint32_t next_write_index = uart_rx_next_index(uart_rx_write_index);

        /*
         * If advancing the write index would reach the read index, the buffer
         * is full.
         */
        if (next_write_index == uart_rx_read_index) {
                return false;
        }

        uart_rx_buffer[uart_rx_write_index] = character;
        uart_rx_write_index = next_write_index;

        return true;
}

void uart_receive_character(char character) {
        if (user_process_handle_console_control(character)) {
                return;
        }
        if (uart_rx_buffer_push(character)) {
                (void)scheduler_wake_one(SCHEDULER_WAIT_UART_RX);
        }
}

/*
 * Read one character from the UART receive ring buffer.
 *
 * This function is called from normal kernel code, not from the interrupt
 * handler.
 *
 * The received character is written to *character.
 *
 * Returns:
 *
 *     true  -> one character was available and returned
 *     false -> the buffer was empty
 */
bool uart_getc(char *character) {
        if (uart_rx_read_index == uart_rx_write_index) {
                return false;
        }

        *character = uart_rx_buffer[uart_rx_read_index];
        uart_rx_read_index = uart_rx_next_index(uart_rx_read_index);

        return true;
}

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
        while ((UART_REGISTERS[UART_LSR] & UART_LSR_THRE) == 0) {
        }

        /*
         * Write the character to the Transmit Holding Register.
         */
        UART_REGISTERS[UART_THR] = (unsigned char)c;
}

/*
 * Submit one byte without waiting in supervisor mode.
 *
 * The caller blocks its process after a successful submission (and also when
 * the device is not ready). THRE later raises an interrupt, which clears the
 * in-flight state and wakes every process sleeping on the UART wait channel.
 */
bool uart_tx_submit(char character) {
        uart_interrupt_enable |= UART_IER_TX_EMPTY;
        UART_REGISTERS[UART_IER] = uart_interrupt_enable;

        if (uart_tx_in_flight ||
            (UART_REGISTERS[UART_LSR] & UART_LSR_THRE) == 0U) {
                return false;
        }

        UART_REGISTERS[UART_THR] = (unsigned char)character;
        uart_tx_in_flight = true;
        return true;
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
 * Print a 64-bit unsigned integer in decimal.
 */
void uart_put_uint64(uint64_t value) {
        char digits[UART_UINT64_DECIMAL_DIGITS];
        size_t length = 0U;

        if (value == 0U) {
                uart_putc('0');
                return;
        }

        while (value != 0U) {
                digits[length] = (char)('0' + (value % UINT64_C(10)));
                length++;
                value /= UINT64_C(10);
        }

        while (length != 0U) {
                length--;
                uart_putc(digits[length]);
        }
}

/*
 * Enable the UART interrupt path, initially for receive data. Transmit-empty
 * interrupts are enabled only while an asynchronous user byte is outstanding.
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
void uart_interrupts_enable(void) {
        uart_interrupt_enable = UART_IER_RX_AVAILABLE;
        UART_REGISTERS[UART_IER] = uart_interrupt_enable;
}

/*
 * Handle UART receive and transmit-empty interrupt conditions.
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
 * Received bytes are retained in the software ring until a process reads the
 * console device. Line editing and echoing are owned by /bin/sh.
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
        while ((UART_REGISTERS[UART_LSR] & UART_LSR_DATA_READY) != 0U) {
                char character = (char)UART_REGISTERS[UART_RBR];
                uart_receive_character(character);
        }

        /* THRE is level-triggered. Disable it before waking writers so the
         * external interrupt cannot remain asserted with no byte pending. */
        if ((uart_interrupt_enable & UART_IER_TX_EMPTY) != 0U &&
            (UART_REGISTERS[UART_LSR] & UART_LSR_THRE) != 0U) {
                uart_interrupt_enable &= (uint8_t)~UART_IER_TX_EMPTY;
                UART_REGISTERS[UART_IER] = uart_interrupt_enable;
                uart_tx_in_flight = false;
                (void)scheduler_wake_all(SCHEDULER_WAIT_UART_TX);
        }
}
