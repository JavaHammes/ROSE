#include <stddef.h>

#include "shell.h"
#include "terminal.h"
#include "uart.h"

#define TERMINAL_LINE_SIZE 128U

static char terminal_line[TERMINAL_LINE_SIZE];
static size_t terminal_line_length;

static void terminal_print_prompt(void) { uart_puts("rose> "); }

static void terminal_submit_line(void) {
        /*
         * Convert the current input into a null-terminated C string.
         */
        terminal_line[terminal_line_length] = '\0';

        uart_puts("\n");

        if (terminal_line_length != 0U) {
                shell_execute(terminal_line);
        }

        terminal_line_length = 0U;
        terminal_print_prompt();
}

static void terminal_handle_backspace(void) {
        if (terminal_line_length == 0U) {
                return;
        }

        terminal_line_length--;

        /*
         * Move one position left, overwrite the character with a space, and
         * move left again.
         */
        uart_puts("\b \b");
}

static void terminal_handle_character(char character) {
        switch (character) {
        case '\r':
        case '\n':
                terminal_submit_line();
                break;

        case '\b':
        case 0x7f:
                terminal_handle_backspace();
                break;

        default:
                /*
                 * Ignore non-printable ASCII characters for now.
                 */
                if (character < ' ' || character > '~') {
                        break;
                }

                /*
                 * Leave one byte for the terminating null character.
                 */
                if (terminal_line_length >= TERMINAL_LINE_SIZE - 1U) {
                        break;
                }

                terminal_line[terminal_line_length] = character;
                terminal_line_length++;

                /*
                 * Echo the typed character.
                 */
                uart_putc(character);
                break;
        }
}

void terminal_init(void) {
        terminal_line_length = 0U;

        uart_puts("\nROSE kernel terminal\n");
        terminal_print_prompt();
}

void terminal_poll(void) {
        char character;

        /*
         * Process every character currently present in the UART software
         * receive buffer.
         */
        while (uart_getc(&character)) {
                terminal_handle_character(character);
        }
}
