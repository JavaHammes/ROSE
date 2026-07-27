#include <stddef.h>
#include <stdint.h>

#include "shell.h"
#include "uart.h"

#define SHELL_MAX_ARGUMENTS 8U

/*
 * Number of elements in a compile-time array.
 */
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/*
 * We currently can't import string.h
 */
static int string_compare(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }

        return (unsigned char)*left - (unsigned char)*right;
}

static void shell_command_help(int argc, char **argv);
static void shell_command_echo(int argc, char **argv);
static void shell_command_clear(int argc, char **argv);
static void shell_command_info(int argc, char **argv);

static const struct shell_command commands[] = {
    {.name = "help",
     .description = "Show available commands",
     .handler = shell_command_help},
    {.name = "echo",
     .description = "Print arguments",
     .handler = shell_command_echo},
    {.name = "clear",
     .description = "Clear the terminal",
     .handler = shell_command_clear},
    {.name = "info",
     .description = "Show kernel information",
     .handler = shell_command_info}};

/*
 * Print all registered shell commands.
 *
 * Because the help output is generated from the command table, it stays
 * synchronized when commands are added or removed.
 */
static void shell_command_help(int argc, char **argv) {
        (void)argc;
        (void)argv;

        uart_puts("Available commands:\n");

        for (size_t index = 0U; index < ARRAY_SIZE(commands); index++) {
                uart_puts("  ");
                uart_puts(commands[index].name);
                uart_puts(" - ");
                uart_puts(commands[index].description);
                uart_putc('\n');
        }
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

static void shell_command_clear(int argc, char **argv) {
        (void)argc;
        (void)argv;
        /*
         * ANSI escape sequence:
         *
         * ESC[2J clears the screen.
         * ESC[H moves the cursor to the upper-left corner.
         */
        uart_puts("\x1b[2J\x1b[H");
}

static void shell_command_info(int argc, char **argv) {
        (void)argc;
        (void)argv;

        uart_puts("ROSE RISC-V kernel\n");
        uart_puts("Architecture: RV64\n");
        uart_puts("Privilege mode: Supervisor\n");
}

/*
 * Split a command line into null-terminated arguments.
 *
 * The function modifies line in place by replacing spaces with '\0'.
 *
 * Example:
 *
 *     Before:
 *
 *         "echo hello world\0"
 *
 *     After:
 *
 *         "echo\0hello\0world\0"
 *
 *     argv[0] points to "echo".
 *     argv[1] points to "hello".
 *     argv[2] points to "world".
 *
 * Returns the number of parsed arguments.
 */
static int shell_parse_arguments(char *line, char **argv,
                                 size_t argv_capacity) {
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

        int argc = shell_parse_arguments(line, argv, SHELL_MAX_ARGUMENTS);

        if (argc == 0) {
                return;
        }

        /*
         * Search the command table for a matching command name.
         */
        for (size_t index = 0U; index < ARRAY_SIZE(commands); index++) {
                if (string_compare(argv[0], commands[index].name) == 0) {
                        commands[index].handler(argc, argv);
                        return;
                }
        }

        uart_puts("Unknown command: ");
        uart_puts(argv[0]);
        uart_putc('\n');
}
