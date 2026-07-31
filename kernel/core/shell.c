#include <stddef.h>
#include <stdint.h>

#include "page_allocator.h"
#include "platform.h"
#include "sbi.h"
#include "shell.h"
#include "uart.h"
#include "user_abi.h"
#include "user_process.h"
#include "virtual_memory.h"

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
static void shell_command_meminfo(int argc, char **argv);
static void shell_command_exit(int argc, char **argv);
static void shell_command_run(int argc, char **argv);
static void shell_command_runmulti(int argc, char **argv);
static void shell_command_spawn(int argc, char **argv);
static void shell_command_wait(int argc, char **argv);
static void shell_command_kill(int argc, char **argv);
static void shell_command_reap(int argc, char **argv);
static void shell_command_ps(int argc, char **argv);

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
     .handler = shell_command_info},
    {.name = "meminfo",
     .description = "Show physical page usage",
     .handler = shell_command_meminfo},
    {.name = "exit",
     .description = "Shut down the system",
     .handler = shell_command_exit},
    {.name = "run",
     .description = "Run the U-mode demo program",
     .handler = shell_command_run},
    {.name = "runmulti",
     .description = "Run two preemptively scheduled processes",
     .handler = shell_command_runmulti},
    {.name = "spawn",
     .description = "Create a process without starting it",
     .handler = shell_command_spawn},
    {.name = "wait",
     .description = "Run all ready processes until they exit",
     .handler = shell_command_wait},
    {.name = "kill",
     .description = "Terminate a ready process",
     .handler = shell_command_kill},
    {.name = "reap",
     .description = "Remove exited processes from the table",
     .handler = shell_command_reap},
    {.name = "ps",
     .description = "Show the process table",
     .handler = shell_command_ps}};

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
        uart_puts("Virtual memory: ");
        uart_puts(virtual_memory_is_enabled() ? "Sv39\n" : "disabled\n");
        uart_puts("RAM: ");
        uart_put_hex64(platform_ram_start());
        uart_puts(" - ");
        uart_put_hex64(platform_ram_end());
        uart_putc('\n');
        uart_puts("Timer frequency: ");
        uart_put_uint64(platform_timebase_frequency());
        uart_puts(" Hz\n");
        uart_puts("UART: ");
        uart_put_hex64(platform_uart_base());
        uart_puts(" (IRQ ");
        uart_put_uint64(platform_uart_interrupt());
        uart_puts(")\n");
        uart_puts("PLIC: ");
        uart_put_hex64(platform_plic_base());
        uart_putc('\n');
}

static void shell_print_page_count(const char *label, size_t count) {
        uart_puts(label);
        uart_put_uint64(count);
        uart_puts(" pages (");
        uart_put_hex64((uint64_t)count * PAGE_SIZE);
        uart_puts(" bytes)\n");
}

static void shell_command_meminfo(int argc, char **argv) {
        (void)argc;
        (void)argv;

        uart_puts("Physical memory:\n");
        shell_print_page_count("  usable: ", page_total_count());
        shell_print_page_count("  used:   ", page_used_count());
        shell_print_page_count("  free:   ", page_free_count());
}

static void shell_command_exit(int argc, char **argv) {
        (void)argc;
        (void)argv;

        uart_puts("Shutting down...\n");

        if (sbi_shutdown() != 0) {
                uart_puts("SBI shutdown request failed\n");
        }
}

static void shell_command_run(int argc, char **argv) {
        if (argc == 1) {
                user_process_run();
                return;
        }

        if (argc == 2 && string_compare(argv[1], "fault") == 0) {
                user_process_run_fault_test();
                return;
        }

        if (argc == 2 && string_compare(argv[1], "syscall") == 0) {
                uint64_t pid;

                if (!user_process_spawn(USER_PROGRAM_SYSCALL_TEST, &pid)) {
                        uart_puts("Unable to create process; run 'reap' and "
                                  "retry\n");
                        return;
                }

                (void)pid;
                (void)user_process_run_ready();
                return;
        }

        uart_puts("Usage: run [fault|syscall]\n");
}

static void shell_command_runmulti(int argc, char **argv) {
        (void)argv;

        if (argc != 1) {
                uart_puts("Usage: runmulti\n");
                return;
        }

        user_process_run_multi();
}

/* Translate a human-readable demonstration name into the selector passed in
 * the new process's initial a0 register. */
static bool shell_parse_program(const char *name, uint64_t *program) {
        if (string_compare(name, "hello") == 0) {
                *program = USER_PROGRAM_HELLO;
        } else if (string_compare(name, "fault") == 0) {
                *program = USER_PROGRAM_FAULT;
        } else if (string_compare(name, "a") == 0) {
                *program = USER_PROGRAM_MULTI_A;
        } else if (string_compare(name, "b") == 0) {
                *program = USER_PROGRAM_MULTI_B;
        } else if (string_compare(name, "syscall") == 0) {
                *program = USER_PROGRAM_SYSCALL_TEST;
        } else {
                return false;
        }

        return true;
}

/* Parse a decimal PID without libc and reject both invalid digits and uint64_t
 * overflow. The result is not modified unless the complete string is valid. */
static bool shell_parse_uint64(const char *text, uint64_t *value) {
        uint64_t result = 0U;

        if (*text == '\0') {
                return false;
        }

        while (*text != '\0') {
                if (*text < '0' || *text > '9') {
                        return false;
                }

                uint64_t digit = (uint64_t)(*text - '0');

                if (result > (UINT64_MAX - digit) / UINT64_C(10)) {
                        return false;
                }

                result = result * UINT64_C(10) + digit;
                text++;
        }

        *value = result;
        return true;
}

/* Create a READY process while leaving the shell active for more commands. */
static void shell_command_spawn(int argc, char **argv) {
        uint64_t program = USER_PROGRAM_HELLO;

        if (argc > 2 ||
            (argc == 2 && !shell_parse_program(argv[1], &program))) {
                uart_puts("Usage: spawn [hello|fault|a|b|syscall]\n");
                return;
        }

        uint64_t pid;

        if (!user_process_spawn(program, &pid)) {
                uart_puts("Unable to create process; run 'reap' and retry\n");
                return;
        }

        uart_puts("Spawned process ");
        uart_put_uint64(pid);
        uart_putc('\n');
}

/* Enter the foreground scheduler only after all desired processes are ready. */
static void shell_command_wait(int argc, char **argv) {
        (void)argv;

        if (argc != 1) {
                uart_puts("Usage: wait\n");
                return;
        }

        (void)user_process_run_ready();
}

/* Shell-side termination applies only to READY processes because the shell is
 * suspended while a process is RUNNING. */
static void shell_command_kill(int argc, char **argv) {
        uint64_t pid;

        if (argc != 2 || !shell_parse_uint64(argv[1], &pid)) {
                uart_puts("Usage: kill PID\n");
                return;
        }

        if (!user_process_kill(pid)) {
                uart_puts("No ready process with that PID\n");
                return;
        }

        uart_puts("Terminated process ");
        uart_put_uint64(pid);
        uart_putc('\n');
}

/* Reaping discards exited status records and makes fixed table slots reusable. */
static void shell_command_reap(int argc, char **argv) {
        (void)argv;

        if (argc != 1) {
                uart_puts("Usage: reap\n");
                return;
        }

        uart_puts("Reaped ");
        uart_put_uint64(user_process_reap_exited());
        uart_puts(" process(es)\n");
}

static void shell_command_ps(int argc, char **argv) {
        (void)argv;

        if (argc != 1) {
                uart_puts("Usage: ps\n");
                return;
        }

        user_process_print_table();
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
