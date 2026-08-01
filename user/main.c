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

/* Linked only into /bin/sh; constant program selection removes the reference
 * from every other independently linked user image. */
int rose_shell_main(char **environment);

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

/* PID 1 remains resident while the interactive shell runs. Keeping init as
 * the shell's parent gives the process hierarchy a stable userspace root and
 * lets init reap the shell before returning control to the kernel. */
static int run_init(void) {
        print("ROSE init: writable disk root online\n");

        char *arguments[] = {"/bin/sh", NULL};
        char *environment[] = {
            "HOME=/", "PATH=/bin:/sbin", "TERM=rose", NULL};
        long shell_pid = rose_spawn("/bin/sh", arguments, environment);

        if (shell_pid < 0) {
                print("ROSE init: unable to start /bin/sh\n");
                return 1;
        }

        int status = 0;
        if (rose_waitpid(shell_pid, &status, 0U) != shell_pid ||
            !USER_WAIT_STATUS_EXITED(status)) {
                print("ROSE init: unable to reap /bin/sh\n");
                return 1;
        }

        return (int)USER_WAIT_STATUS_EXIT_CODE(status);
}

static void preemption_delay(void) {
        /* Busy work makes each process live long enough for the 1 ms timer to
         * interrupt it even on a fast host. volatile prevents optimization. */
        for (volatile uint64_t remaining = UINT64_C(2000000); remaining != 0U;
             remaining--) {
        }
}

static bool strings_equal(const char *left, const char *right);

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

        char current_directory[64];
        if (rose_getcwd(current_directory, sizeof(current_directory)) != 2 ||
            !strings_equal(current_directory, "/") ||
            rose_getcwd(current_directory, 1U) != -USER_ERROR_RANGE ||
            rose_getcwd((char *)kernel_address,
                        sizeof(current_directory)) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_chdir((const char *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_chdir("/missing") != -USER_ERROR_NO_ENTRY ||
            rose_chdir("/etc/motd") != -USER_ERROR_NOT_DIRECTORY ||
            rose_chdir("/dev/console") != -USER_ERROR_NOT_DIRECTORY ||
            rose_chdir("/etc//./") != 0 ||
            rose_getcwd(current_directory, sizeof(current_directory)) != 5 ||
            !strings_equal(current_directory, "/etc")) {
                return 39;
        }

        struct user_file_status descriptor_status;
        long original = rose_open("motd", USER_OPEN_READ);
        long duplicate = rose_dup((int)original);
        long replacement = rose_open("./motd", USER_OPEN_READ);
        char characters[5];

        if (original != 3 || duplicate != 4 || replacement != 5 ||
            rose_stat("motd", &descriptor_status) != 0 ||
            descriptor_status.type != USER_FILE_REGULAR ||
            rose_fstat((int)original, &descriptor_status) != 0 ||
            descriptor_status.type != USER_FILE_REGULAR ||
            descriptor_status.size == 0U ||
            rose_fstat(99, &descriptor_status) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_fstat((int)original,
                       (struct user_file_status *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_read((int)replacement, &characters[0], 1U) != 1 ||
            characters[0] != 'W' ||
            rose_dup2((int)original, (int)replacement) != replacement ||
            rose_dup2((int)original, 7) != 7 ||
            rose_dup2((int)original, (int)original) != original ||
            rose_dup(99) != -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_dup2(99, 6) != -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_dup2((int)original, 8) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_read((int)original, &characters[0], 1U) != 1 ||
            rose_read((int)duplicate, &characters[1], 1U) != 1 ||
            rose_read((int)replacement, &characters[2], 1U) != 1 ||
            rose_read(7, &characters[3], 1U) != 1 ||
            characters[0] != 'W' || characters[1] != 'e' ||
            characters[2] != 'l' || characters[3] != 'c' ||
            rose_close((int)original) != 0 ||
            rose_read((int)duplicate, &characters[4], 1U) != 1 ||
            characters[4] != 'o' || rose_close((int)duplicate) != 0 ||
            rose_close((int)replacement) != 0 || rose_close(7) != 0 ||
            rose_chdir("..") != 0 ||
            rose_getcwd(current_directory, sizeof(current_directory)) != 2 ||
            !strings_equal(current_directory, "/")) {
                return 40;
        }

        print("Working directory passed\n");
        print("Descriptor duplication passed\n");

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

        /* brk(0) exposes the page-aligned end of the loaded ELF. Growing by a
         * non-page multiple verifies both full and partial heap pages. */
        long heap_query = rose_brk(0U);
        if (heap_query <= 0 ||
            ((uintptr_t)heap_query & (UINT64_C(4096) - 1U)) != 0U) {
                return 33;
        }

        uintptr_t heap_start = (uintptr_t)heap_query;
        uintptr_t heap_end = heap_start + UINT64_C(8192) + 37U;

        if (rose_brk(heap_start - 1U) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_brk(UINTPTR_MAX) != -USER_ERROR_OUT_OF_MEMORY ||
            rose_brk(0U) != heap_query ||
            rose_brk(heap_end) != (long)heap_end) {
                return 34;
        }

        volatile uint8_t *heap = (volatile uint8_t *)heap_start;
        if (heap[0] != 0U || heap[4095] != 0U || heap[4096] != 0U ||
            heap[8192] != 0U || heap[8228] != 0U) {
                return 35;
        }

        heap[0] = UINT8_C(0x11);
        heap[4096] = UINT8_C(0x22);
        heap[8192] = UINT8_C(0x33);
        heap[8228] = UINT8_C(0x44);

        /* Dropping the third page and growing it again must provide a fresh,
         * zero-filled page rather than exposing its old contents. The test
         * deliberately exits with the heap still mapped so teardown is also
         * covered by the global leak check. */
        uintptr_t shrunken_end = heap_start + UINT64_C(4096) + 1U;
        if (rose_brk(shrunken_end) != (long)shrunken_end ||
            rose_brk(heap_end) != (long)heap_end || heap[0] != UINT8_C(0x11) ||
            heap[4096] != UINT8_C(0x22) || heap[8192] != 0U ||
            heap[8228] != 0U) {
                return 36;
        }

        char *child_arguments[] = {"/bin/hello", NULL};
        char *child_environment[] = {NULL};
        long parent_pid = rose_getpid();

        if (parent_pid <= 0 ||
            rose_waitpid(parent_pid, NULL, 0U) != -USER_ERROR_NO_CHILD ||
            rose_waitpid(-1, NULL, 2U) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_spawn("/missing", child_arguments, child_environment) !=
                -USER_ERROR_NO_ENTRY) {
                return 30;
        }

        long child_pid =
            rose_spawn("/bin/hello", child_arguments, child_environment);
        int wait_status = -1;

        if (child_pid <= 0 || child_pid == parent_pid ||
            rose_waitpid(child_pid, (int *)kernel_address,
                         USER_WAIT_NO_HANG) != -USER_ERROR_BAD_ADDRESS) {
                return 31;
        }

        long waited_pid =
            rose_waitpid(-1, &wait_status, USER_WAIT_NO_HANG);
        if (waited_pid == 0) {
                waited_pid = rose_waitpid(-1, &wait_status, 0U);
        }
        if (waited_pid != child_pid ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U ||
            rose_waitpid(child_pid, NULL, USER_WAIT_NO_HANG) !=
                -USER_ERROR_NO_CHILD) {
                return 32;
        }

        print("Process hierarchy passed\n");
        print("Userspace heap passed\n");
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

static int run_execve_test(void) {
        char *arguments[] = {"/bin/execve-target", "replacement", NULL};
        char *environment[] = {"EXECVE_TEST=passed", NULL};
        char *too_many_arguments[17];
        const void *kernel_address =
            (const void *)(uintptr_t)UINT64_C(0x80200000);
        long descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        char first_character;
        long heap_query = rose_brk(0U);

        if (descriptor != 3 ||
            rose_read((int)descriptor, &first_character, 1U) != 1 ||
            first_character != 'W' || heap_query <= 0 ||
            rose_brk((uintptr_t)heap_query + 1U) != heap_query + 1) {
                return 25;
        }

        volatile uint8_t *heap = (volatile uint8_t *)(uintptr_t)heap_query;
        heap[0] = UINT8_C(0x5a);

        for (size_t index = 0U; index < 17U; index++) {
                too_many_arguments[index] = "x";
        }

        /* Every failure must return to this unchanged image. */
        if (rose_execve((const char *)kernel_address, arguments,
                        environment) != -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/missing", arguments, environment) !=
                -USER_ERROR_NO_ENTRY ||
            rose_execve("/etc/motd", arguments, environment) !=
                -USER_ERROR_EXEC_FORMAT ||
            rose_execve("/bin/execve-target",
                        (char *const *)kernel_address,
                        environment) != -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/bin/execve-target", arguments,
                        (char *const *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/bin/execve-target", too_many_arguments,
                        environment) !=
                -USER_ERROR_ARGUMENT_LIST_TOO_LONG) {
                return 26;
        }
        if (rose_brk(0U) != heap_query + 1 || heap[0] != UINT8_C(0x5a)) {
                return 37;
        }

        /* Success cannot return; the target verifies the copied vectors,
         * descriptor offset, and working directory inherited from this image.
         */
        if (rose_chdir("/bin") != 0) {
                return 41;
        }
        long result =
            rose_execve("execve-target", arguments, environment);
        return result < 0 ? 27 : 28;
}

static int run_execve_target(int argc, char **argv, char **environment) {
        const char *test_value =
            find_environment_value(environment, "EXECVE_TEST");
        char second_character;
        char current_directory[64];
        long heap_query = rose_brk(0U);

        if (argc != 2 || argv == NULL ||
            !strings_equal(argv[0], "/bin/execve-target") ||
            !strings_equal(argv[1], "replacement") || argv[2] != NULL ||
            environment == NULL || environment[0] == NULL ||
            environment[1] != NULL || test_value == NULL ||
            !strings_equal(test_value, "passed") ||
            rose_getcwd(current_directory, sizeof(current_directory)) != 5 ||
            !strings_equal(current_directory, "/bin") ||
            rose_read(3, &second_character, 1U) != 1 ||
            second_character != 'e' || heap_query <= 0 ||
            rose_brk((uintptr_t)heap_query + 1U) != heap_query + 1) {
                return 29;
        }

        volatile uint8_t *heap = (volatile uint8_t *)(uintptr_t)heap_query;
        if (heap[0] != 0U) {
                return 38;
        }

        print("Execve replacement passed\n");
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

static int run_pipe_test(void) {
        const void *kernel_address =
            (const void *)(uintptr_t)UINT64_C(0x80200000);
        int descriptors[2] = {-1, -1};
        struct user_file_status status;

        if (rose_pipe((int *)kernel_address) != -USER_ERROR_BAD_ADDRESS ||
            rose_pipe(descriptors) != 0 || descriptors[0] != 3 ||
            descriptors[1] != 4 ||
            rose_fstat(descriptors[0], &status) != 0 ||
            status.type != USER_FILE_PIPE ||
            rose_lseek(descriptors[0], 0, USER_SEEK_SET) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_write(descriptors[0], "x", 1U) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_read(descriptors[1], &descriptors[0], 1U) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR) {
                return 42;
        }

        char *arguments[] = {"/bin/pipe-writer", NULL};
        char *environment[] = {NULL};
        long child = rose_spawn("/bin/pipe-writer", arguments, environment);

        if (child <= 0 || rose_close(descriptors[1]) != 0) {
                return 43;
        }

        char streaming_buffer[257];
        size_t received = 0U;
        while (received < 2048U) {
                long streaming_count = rose_read(
                    descriptors[0], streaming_buffer,
                    sizeof(streaming_buffer));
                if (streaming_count <= 0 ||
                    (size_t)streaming_count > 2048U - received) {
                        return 44;
                }
                for (long index = 0; index < streaming_count; index++) {
                        if (streaming_buffer[index] != 'P') {
                                return 44;
                        }
                }
                received += (size_t)streaming_count;
        }

        static const char expected[] = "Pipe communication passed";
        char buffer[sizeof(expected)];
        long count = rose_read(descriptors[0], buffer, sizeof(buffer) - 1U);
        if (count != (long)(sizeof(expected) - 1U)) {
                return 45;
        }
        buffer[count] = '\0';
        if (!strings_equal(buffer, expected) ||
            rose_read(descriptors[0], buffer, sizeof(buffer)) != 0 ||
            rose_close(descriptors[0]) != 0) {
                return 46;
        }

        int wait_status = -1;
        if (rose_waitpid(child, &wait_status, 0U) != child ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U) {
                return 47;
        }

        if (rose_pipe(descriptors) != 0 ||
            rose_close(descriptors[0]) != 0 ||
            rose_write(descriptors[1], "x", 1U) !=
                -USER_ERROR_BROKEN_PIPE ||
            rose_close(descriptors[1]) != 0) {
                return 48;
        }

        print("Pipe communication passed\n");
        return 0;
}

static int run_pipe_writer(void) {
        static char full_buffer[1024];
        static const char payload[] = "Pipe communication passed";

        for (size_t index = 0U; index < sizeof(full_buffer); index++) {
                full_buffer[index] = 'P';
        }

        if (rose_close(3) != 0 ||
            rose_write(4, full_buffer, sizeof(full_buffer)) !=
                (long)sizeof(full_buffer) ||
            rose_write(4, full_buffer, sizeof(full_buffer)) !=
                (long)sizeof(full_buffer) ||
            rose_write(4, payload, sizeof(payload) - 1U) !=
                (long)(sizeof(payload) - 1U) ||
            rose_close(4) != 0) {
                return 49;
        }

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
                return run_init();

        case USER_PROGRAM_FS_TEST:
                return run_filesystem_test();

        case USER_PROGRAM_ARGUMENTS_ENVIRONMENT:
                return run_arguments_environment_test(argc, argv, environment);

        case USER_PROGRAM_EXECVE:
                return run_execve_test();

        case USER_PROGRAM_EXECVE_TARGET:
                return run_execve_target(argc, argv, environment);

        case USER_PROGRAM_PIPE_TEST:
                return run_pipe_test();

        case USER_PROGRAM_PIPE_WRITER:
                return run_pipe_writer();

        case USER_PROGRAM_SH:
                return rose_shell_main(environment);

        default:
                return 2;
        }
}
