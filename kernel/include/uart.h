#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_putc(char c);
void uart_puts(const char *str);
void uart_put_hex_digit(uint8_t digit);
void uart_put_hex64(uint64_t value);

#endif
