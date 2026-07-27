#include <stddef.h>
#include <stdint.h>

#include "shell.h"
#include "uart.h"

#define SHELL_MAX_ARGUMENTS 8U

/*
 * We currently can't import string.h
 */
static int strcmp(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }

        return (unsigned char)*left - (unsigned char)*right;
}

static void shell_command_help(void) {
        uart_puts("Available commands:\n");
        uart_puts("  help        Show this help message\n");
        uart_puts("  echo TEXT   Print TEXT\n");
        uart_puts("  clear       Clear the terminal\n");
        uart_puts("  info        Show kernel information\n");
}

static void shell_command_echo(int argc, char **argv) {
        for (int index = 1; index < argc; index++) {
                uart_puts(argv[index]);

                if (index + 1 < argc) {
                        uart_putc(' ');
                }
        }

        uart_putc('\n');
}

static void shell_command_clear(void) {
        /*
         * ANSI escape sequence:
         *
         * ESC[2J clears the screen.
         * ESC[H moves the cursor to the upper-left corner.
         */
        uart_puts("\x1b[2J\x1b[H");
}

static void shell_command_info(void) {
        uart_puts("ROSE RISC-V kernel\n");
        uart_puts("Architecture: RV64\n");
        uart_puts("Privilege mode: Supervisor\n");
}

static int shell_parse_arguments(
        char *line,
        char **argv,
        size_t argv_capacity
) {
        size_t argc = 0U;
        char *cursor = line;

        while (*cursor != '\0') {
                /*
                 * Skip spaces between arguments.
                 */
                while (*cursor == ' ') {
                        cursor++;
                }

                if (*cursor == '\0') {
                        break;
                }

                if (argc >= argv_capacity) {
                        break;
                }

                argv[argc] = cursor;
                argc++;

                /*
                 * Find the end of this argument.
                 */
                while (*cursor != '\0' && *cursor != ' ') {
                        cursor++;
                }

                if (*cursor == '\0') {
                        break;
                }

                /*
                 * Replace the separator with a null terminator so each argv
                 * entry points to an independent C string.
                 */
                *cursor = '\0';
                cursor++;
        }

        return (int)argc;
}

void shell_execute(char *line) {
        char *argv[SHELL_MAX_ARGUMENTS];

        int argc = shell_parse_arguments(
                line,
                argv,
                SHELL_MAX_ARGUMENTS
        );

        if (argc == 0) {
                return;
        }

        if (strcmp(argv[0], "help") == 0) {
                shell_command_help();
                return;
        }

        if (strcmp(argv[0], "echo") == 0) {
                shell_command_echo(argc, argv);
                return;
        }

        if (strcmp(argv[0], "clear") == 0) {
                shell_command_clear();
                return;
        }

        if (strcmp(argv[0], "info") == 0) {
                shell_command_info();
                return;
        }

        uart_puts("Unknown command: ");
        uart_puts(argv[0]);
        uart_putc('\n');
}
