/*
 * Freestanding U-mode demonstration program.
 *
 * The build produces one small ELF for each /bin path in the initial ramfs.
 * ROSE_PROGRAM is constant for a given image, so the compiler discards the
 * other demonstrations while the sources continue sharing one runtime.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/syscall.h"
#include "user_abi.h"

/* Static analysis checks this source once outside the per-program build. */
#ifndef ROSE_PROGRAM
#define ROSE_PROGRAM USER_PROGRAM_HELLO
#endif

/* Nonzero .data and zero-initialized .bss values verify both ELF load paths. */
static volatile uint64_t user_cookie = UINT64_C(0x524f5345);
static volatile uint64_t user_counter;

/* Small libc replacements keep the user executable completely freestanding. */
static size_t string_length(const char *text) {
        size_t length = 0U;

        while (text[length] != '\0') {
                length++;
        }

        return length;
}

static void print(const char *text) {
        (void)rose_write(USER_STDOUT_FILENO, text, string_length(text));
}

static void preemption_delay(void) {
        /* Busy work makes each process live long enough for the 1 ms timer to
         * interrupt it even on a fast host. volatile prevents optimization. */
        for (volatile uint64_t remaining = UINT64_C(2000000); remaining != 0U;
             remaining--) {
        }
}

static int run_multi_demo(const char *message) {
        preemption_delay();

        for (size_t iteration = 0U; iteration < 4U; iteration++) {
                print(message);
                /* Yield guarantees visible alternation while the initial delay
                 * independently proves that timer preemption also occurred. */
                rose_yield();
        }

        return 0;
}

static int run_syscall_test(void) {
        /* Kernel text is mapped supervisor-only in this address space. Passing
         * it to write must return EFAULT without dereferencing it in S-mode. */
        const void *kernel_address =
            (const void *)(uintptr_t)UINT64_C(0x80200000);

        if (rose_write(USER_STDOUT_FILENO, kernel_address, 1U) !=
            -USER_ERROR_BAD_ADDRESS) {
                return 4;
        }
        if (rose_write(99, "x", 1U) != -USER_ERROR_BAD_FILE_DESCRIPTOR) {
                return 6;
        }
        if (rose_open("/missing", USER_OPEN_READ) != -USER_ERROR_NO_ENTRY ||
            rose_close(99) != -USER_ERROR_BAD_FILE_DESCRIPTOR) {
                return 7;
        }

        long descriptors[5];

        for (size_t index = 0U; index < 5U; index++) {
                descriptors[index] = rose_open("/etc/motd", USER_OPEN_READ);
                if (descriptors[index] != (long)(index + 3U)) {
                        return 14;
                }
        }
        if (rose_open("/etc/motd", USER_OPEN_READ) !=
            -USER_ERROR_TOO_MANY_FILES) {
                return 15;
        }
        for (size_t index = 0U; index < 5U; index++) {
                if (rose_close((int)descriptors[index]) != 0) {
                        return 16;
                }
        }

        long descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        char byte;

        if (descriptor != 3 ||
            rose_read(USER_STDOUT_FILENO, &byte, 1U) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_read((int)descriptor, (void *)kernel_address, 1U) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_close((int)descriptor) != 0) {
                return 17;
        }
        /* The raw wrapper lets this test issue a deliberately unknown number. */
        if (rose_syscall(UINT64_C(0xffff), 0U, 0U, 0U) !=
            -USER_ERROR_NOT_IMPLEMENTED) {
                return 5;
        }

        print("Syscall validation passed\n");
        return 0;
}

static int run_cat(void) {
        long descriptor = rose_open("/etc/motd", USER_OPEN_READ);

        if (descriptor < 0) {
                return 8;
        }

        char buffer[24];

        while (true) {
                long count = rose_read((int)descriptor, buffer, sizeof(buffer));

                if (count < 0) {
                        (void)rose_close((int)descriptor);
                        return 9;
                }
                if (count == 0) {
                        break;
                }
                if (rose_write(USER_STDOUT_FILENO, buffer, (size_t)count) !=
                    count) {
                        (void)rose_close((int)descriptor);
                        return 10;
                }
        }

        return rose_close((int)descriptor) == 0 ? 0 : 11;
}

static bool strings_equal(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }
        return *left == *right;
}

static const char *find_environment_value(char **environment,
                                          const char *name) {
        size_t name_length = string_length(name);

        for (size_t index = 0U; environment[index] != NULL; index++) {
                const char *entry = environment[index];
                size_t character = 0U;

                while (character < name_length &&
                       entry[character] == name[character]) {
                        character++;
                }
                if (character == name_length && entry[character] == '=') {
                        return &entry[character + 1U];
                }
        }

        return NULL;
}

static int run_arguments_environment_test(int argc, char **argv,
                                          char **environment) {
        if (argc != 3 || argv == NULL ||
            !strings_equal(argv[0], "/bin/args-env") ||
            !strings_equal(argv[1], "alpha") ||
            !strings_equal(argv[2], "beta") || argv[3] != NULL) {
                return 23;
        }

        const char *test_value =
            find_environment_value(environment, "ROSE_TEST");
        const char *home = find_environment_value(environment, "HOME");

        if (test_value == NULL || !strings_equal(test_value, "passed") ||
            home == NULL || !strings_equal(home, "/")) {
                return 24;
        }

        print("Program arguments and environment passed\n");
        return 0;
}

static int run_filesystem_test(void) {
        struct user_file_status status;
        if (rose_stat("/etc/motd", &status) != 0 ||
            status.type != USER_FILE_REGULAR || status.size == 0U) {
                return 18;
        }
        if (rose_mkdir("/tmp") != 0) {
                return 19;
        }
        if (rose_stat("/tmp", &status) != 0 ||
            status.type != USER_FILE_DIRECTORY) {
                return 19;
        }

        long descriptor = rose_open("/tmp/state", USER_OPEN_READ |
                                                      USER_OPEN_WRITE |
                                                      USER_OPEN_CREATE |
                                                      USER_OPEN_TRUNCATE);
        static const char payload[] = "persistent ext2\n";
        if (descriptor < 0 ||
            rose_write((int)descriptor, payload, sizeof(payload) - 1U) !=
                (long)(sizeof(payload) - 1U) ||
            rose_lseek((int)descriptor, 0, USER_SEEK_END) !=
                (long)(sizeof(payload) - 1U) ||
            rose_lseek((int)descriptor, -(int64_t)(sizeof(payload) - 1U),
                       USER_SEEK_CURRENT) != 0 ||
            rose_stat("/tmp/state", &status) != 0 ||
            status.size != sizeof(payload) - 1U) {
                return 20;
        }

        char buffer[sizeof(payload)];
        long count = rose_read((int)descriptor, buffer, sizeof(payload) - 1U);
        buffer[sizeof(payload) - 1U] = '\0';
        if (count != (long)(sizeof(payload) - 1U) ||
            !strings_equal(buffer, payload) || rose_close((int)descriptor) != 0) {
                return 21;
        }

        descriptor = rose_open("/tmp", USER_OPEN_READ | USER_OPEN_DIRECTORY);
        bool found = false;
        struct user_directory_entry entry;
        long directory_result;
        while ((directory_result =
                    rose_read_directory((int)descriptor, &entry)) > 0) {
                if (strings_equal(entry.name, "state")) {
                        found = true;
                }
        }
        if (descriptor < 0 || directory_result != 0 || !found ||
            rose_close((int)descriptor) != 0 ||
            rose_unlink("/tmp") != -USER_ERROR_NOT_EMPTY ||
            rose_unlink("/tmp/state") != 0 || rose_unlink("/tmp") != 0) {
                return 22;
        }

        print("Filesystem mutation passed\n");
        return 0;
}

static int run_console_read(void) {
        char character;

        print("Console reader waiting\n");
        if (rose_read(USER_STDIN_FILENO, &character, 1U) != 1) {
                return 12;
        }

        print("Console read: ");
        if (rose_write(USER_STDOUT_FILENO, &character, 1U) != 1) {
                return 13;
        }
        print("\n");
        return 0;
}

/* Referenced by the assembly entry point, so this must retain external linkage.
 */
int user_main(int argc, char **argv,
              char **environment) { // NOLINT(misc-use-internal-linkage)
        /* Every address space starts from the original ELF contents. A failure
         * here catches missing .data copies, missing BSS zeroing, or leaked
         * writable pages between processes. */
        if (user_cookie != UINT64_C(0x524f5345) || user_counter != 0U) {
                return 3;
        }

        user_counter = 1U;

        switch (ROSE_PROGRAM) {
        case USER_PROGRAM_HELLO:
                print("Hello from U-mode C\n");
                return 0;

        case USER_PROGRAM_FAULT: {
                // NOLINTBEGIN(performance-no-int-to-ptr)
                volatile const uint64_t *kernel_text =
                    (volatile const uint64_t *)(uintptr_t)UINT64_C(0x80200000);
                // NOLINTEND(performance-no-int-to-ptr)

                /* This supervisor-only access must terminate the process. */
                // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
                return (int)*kernel_text;
        }

        case USER_PROGRAM_MULTI_A:
                return run_multi_demo("Process A: running\n");

        case USER_PROGRAM_MULTI_B:
                return run_multi_demo("Process B: running\n");

        case USER_PROGRAM_SYSCALL_TEST:
                return run_syscall_test();

        case USER_PROGRAM_CAT:
                return run_cat();

        case USER_PROGRAM_CONSOLE_READ:
                return run_console_read();

        case USER_PROGRAM_INIT:
                print("ROSE init: writable disk root online\n");
                return 0;

        case USER_PROGRAM_FS_TEST:
                return run_filesystem_test();

        case USER_PROGRAM_ARGUMENTS_ENVIRONMENT:
                return run_arguments_environment_test(argc, argv, environment);

        default:
                return 2;
        }
}
