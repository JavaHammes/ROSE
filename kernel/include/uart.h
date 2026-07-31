#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>

void uart_putc(char c);
void uart_puts(const char *str);
void uart_put_hex_digit(uint8_t digit);
void uart_put_hex64(uint64_t value);
void uart_put_uint64(uint64_t value);

/* Submit one byte for interrupt-driven user output without polling. */
bool uart_tx_submit(char character);

bool uart_getc(char *character);

void uart_interrupts_enable(void);
void uart_handle_interrupt(void);

#endif
