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
static volatile uint64_t user_signal_count;

static void signal_test_handler(int signal) {
        if (signal == USER_SIGNAL_USER_1) {
                user_signal_count++;
        }
}

/* Linked only into /bin/sh; constant program selection removes the reference
 * from every other independently linked user image. */
int rose_shell_main(char **environment);
int rose_desktop_main(int argc, char **argv);
int rose_gui_terminal_main(int argc, char **argv);
int rose_gui_files_main(int argc, char **argv);
int rose_gui_monitor_main(int argc, char **argv);

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

static bool write_all(int descriptor, const char *buffer, size_t length) {
        size_t written = 0U;

        while (written < length) {
                long count =
                    rose_write(descriptor, &buffer[written], length - written);
                if (count <= 0) {
                        return false;
                }
                written += (size_t)count;
        }
        return true;
}

/* PID 1 remains resident while the interactive shell runs. Keeping init as
 * the shell's parent gives the process hierarchy a stable userspace root and
 * lets init reap the shell before returning control to the kernel. */
static int run_init(void) {
        print("ROSE init: writable disk root online\n");

        char *arguments[] = {"/bin/sh", NULL};
        char *environment[] = {"HOME=/", "PATH=/bin:/sbin", "TERM=rose", NULL};
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

static int run_anonymous_mapping_test(void) {
        const size_t page_size = 4096U;
        const size_t mapping_length = page_size * 2U + 37U;
        const uint32_t read_write =
            USER_MEMORY_PROTECTION_READ | USER_MEMORY_PROTECTION_WRITE;
        struct user_system_info baseline;

        if (rose_system_info(&baseline) != 0 ||
            rose_mmap(0U, read_write) != -USER_ERROR_INVALID_ARGUMENT ||
            rose_mmap(page_size, USER_MEMORY_PROTECTION_WRITE) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_mmap(page_size, USER_MEMORY_PROTECTION_READ |
                                     USER_MEMORY_PROTECTION_WRITE |
                                     USER_MEMORY_PROTECTION_EXECUTE) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_mmap(page_size, UINT32_C(0x80000000)) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_mmap(SIZE_MAX, USER_MEMORY_PROTECTION_READ) !=
                -USER_ERROR_INVALID_ARGUMENT) {
                return 82;
        }

        /* Fork must copy the reservation itself without forcing either process
         * to materialize it. A child-only first touch must disappear when the
         * child is reaped, leaving the parent's reservation untouched. */
        long untouched_result = rose_mmap(page_size, read_write);
        if (untouched_result <= 0) {
                return 101;
        }
        volatile uint8_t *untouched =
            (volatile uint8_t *)(uintptr_t)untouched_result;
        long untouched_child = rose_fork();
        if (untouched_child < 0) {
                return 102;
        }
        if (untouched_child == 0) {
                if (untouched[0] != 0U) {
                        rose_exit(103U);
                }
                untouched[0] = UINT8_C(0x5a);
                rose_exit(0U);
        }

        int untouched_status = -1;
        struct user_system_info after_untouched_child;
        if (rose_waitpid(untouched_child, &untouched_status, 0U) !=
                untouched_child ||
            !USER_WAIT_STATUS_EXITED(untouched_status) ||
            USER_WAIT_STATUS_EXIT_CODE(untouched_status) != 0U ||
            rose_system_info(&after_untouched_child) != 0 ||
            after_untouched_child.used_pages != baseline.used_pages ||
            rose_munmap((uintptr_t)untouched_result, page_size) != 0) {
                return 104;
        }

        long mapping_result = rose_mmap(mapping_length, read_write);
        if (mapping_result <= 0 ||
            ((uintptr_t)mapping_result & (page_size - 1U)) != 0U) {
                return 83;
        }
        uintptr_t mapping_address = (uintptr_t)mapping_result;
        volatile uint8_t *mapping = (volatile uint8_t *)mapping_address;

        struct user_system_info reserved;
        if (rose_system_info(&reserved) != 0 ||
            reserved.used_pages != baseline.used_pages) {
                return 84;
        }

        if (mapping[0] != 0U || mapping[page_size] != 0U ||
            mapping[2U * page_size] != 0U ||
            mapping[mapping_length - 1U] != 0U) {
                return 85;
        }
        mapping[0] = UINT8_C(0x11);
        mapping[page_size] = UINT8_C(0x22);
        mapping[2U * page_size] = UINT8_C(0x33);

        struct user_system_info resident;
        if (rose_system_info(&resident) != 0 ||
            resident.used_pages < baseline.used_pages + 3U) {
                return 86;
        }

        if (rose_munmap(mapping_address + 1U, page_size) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_munmap(mapping_address, 0U) != -USER_ERROR_INVALID_ARGUMENT ||
            rose_munmap(mapping_address + page_size, page_size) != 0) {
                return 87;
        }

        struct user_system_info middle_unmapped;
        if (rose_system_info(&middle_unmapped) != 0 ||
            middle_unmapped.used_pages + 1U != resident.used_pages) {
                return 88;
        }

        long gap_result = rose_mmap(page_size, read_write);
        if (gap_result != (long)(mapping_address + page_size)) {
                return 89;
        }

        long descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        if (descriptor != 3 ||
            rose_read((int)descriptor, (void *)(uintptr_t)gap_result, 1U) !=
                1 ||
            rose_close((int)descriptor) != 0 ||
            mapping[page_size] != (uint8_t)'W') {
                return 90;
        }

        long child_pid = rose_fork();
        if (child_pid < 0) {
                return 91;
        }
        if (child_pid == 0) {
                if (mapping[0] != UINT8_C(0x11) ||
                    mapping[page_size] != (uint8_t)'W' ||
                    mapping[2U * page_size] != UINT8_C(0x33)) {
                        rose_exit(92U);
                }
                mapping[0] = UINT8_C(0xa1);
                mapping[page_size] = UINT8_C(0xb2);
                mapping[2U * page_size] = UINT8_C(0xc3);
                rose_exit(0U);
        }

        int wait_status = -1;
        if (rose_waitpid(child_pid, &wait_status, 0U) != child_pid ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U ||
            mapping[0] != UINT8_C(0x11) || mapping[page_size] != (uint8_t)'W' ||
            mapping[2U * page_size] != UINT8_C(0x33)) {
                return 93;
        }

        if (rose_munmap(mapping_address, mapping_length) != 0) {
                return 94;
        }

        long read_only_result =
            rose_mmap(page_size, USER_MEMORY_PROTECTION_READ);
        if (read_only_result <= 0) {
                return 95;
        }
        volatile uint8_t *read_only =
            (volatile uint8_t *)(uintptr_t)read_only_result;
        if (read_only[0] != 0U) {
                return 96;
        }

        child_pid = rose_fork();
        if (child_pid < 0) {
                return 97;
        }
        if (child_pid == 0) {
                read_only[0] = UINT8_C(1);
                rose_exit(98U);
        }

        wait_status = -1;
        if (rose_waitpid(child_pid, &wait_status, 0U) != child_pid ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 1U ||
            rose_munmap((uintptr_t)read_only_result, page_size) != 0) {
                return 99;
        }

        struct user_system_info released;
        if (rose_system_info(&released) != 0 ||
            released.used_pages != baseline.used_pages) {
                return 100;
        }

        /* Leave one resident mapping behind intentionally. The shell's wait and
         * the kernel's final idle-page assertion cover process-exit teardown.
         */
        long teardown_result = rose_mmap(page_size, read_write);
        if (teardown_result <= 0) {
                return 105;
        }
        volatile uint8_t *teardown =
            (volatile uint8_t *)(uintptr_t)teardown_result;
        teardown[0] = UINT8_C(0xd4);
        return 0;
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

        char current_directory[64];
        if (rose_getcwd(current_directory, sizeof(current_directory)) != 2 ||
            !strings_equal(current_directory, "/") ||
            rose_getcwd(current_directory, 1U) != -USER_ERROR_RANGE ||
            rose_getcwd((char *)kernel_address, sizeof(current_directory)) !=
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
            rose_dup2((int)original, 8) != -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_read((int)original, &characters[0], 1U) != 1 ||
            rose_read((int)duplicate, &characters[1], 1U) != 1 ||
            rose_read((int)replacement, &characters[2], 1U) != 1 ||
            rose_read(7, &characters[3], 1U) != 1 || characters[0] != 'W' ||
            characters[1] != 'e' || characters[2] != 'l' ||
            characters[3] != 'c' || rose_close((int)original) != 0 ||
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
        long parent_pid = rose_getpid();
        char byte;

        if (descriptor != 3 || parent_pid <= 0 ||
            rose_read(USER_STDOUT_FILENO, &byte, 1U) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_read((int)descriptor, (void *)kernel_address, 1U) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_close((int)descriptor) != 0) {
                return 17;
        }
        /* The raw wrapper lets this test issue a deliberately unknown number.
         */
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

        if (rose_brk(heap_start - 1U) != -USER_ERROR_INVALID_ARGUMENT ||
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

        /* fork returns in both processes with isolated COW-backed ELF, heap,
         * and stack pages. Descriptor entries retain a shared open-file
         * description, so the child advances the parent's observed offset. */
        descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        char fork_character;
        volatile uint64_t stack_cookie = UINT64_C(0x1020304050607080);
        user_counter = UINT64_C(0x1122334455667788);
        heap[0] = UINT8_C(0xa1);

        if (descriptor != 3 ||
            rose_read((int)descriptor, &fork_character, 1U) != 1 ||
            fork_character != 'W') {
                return 52;
        }

        long fork_pid = rose_fork();
        if (fork_pid < 0) {
                return 53;
        }
        if (fork_pid == 0) {
                if (rose_getpid() == parent_pid ||
                    user_counter != UINT64_C(0x1122334455667788) ||
                    heap[0] != UINT8_C(0xa1) ||
                    stack_cookie != UINT64_C(0x1020304050607080)) {
                        rose_exit(54U);
                }

                user_counter = UINT64_C(0x8877665544332211);
                heap[0] = UINT8_C(0xb2);
                stack_cookie = UINT64_C(0x8070605040302010);
                if (rose_read((int)descriptor, &fork_character, 1U) != 1 ||
                    fork_character != 'e') {
                        rose_exit(55U);
                }
                rose_exit(23U);
        }

        int fork_status = -1;
        if (fork_pid == parent_pid ||
            rose_waitpid(fork_pid, &fork_status, 0U) != fork_pid ||
            !USER_WAIT_STATUS_EXITED(fork_status) ||
            USER_WAIT_STATUS_EXIT_CODE(fork_status) != 23U ||
            user_counter != UINT64_C(0x1122334455667788) ||
            heap[0] != UINT8_C(0xa1) ||
            stack_cookie != UINT64_C(0x1020304050607080) ||
            rose_read((int)descriptor, &fork_character, 1U) != 1 ||
            fork_character != 'l' || rose_close((int)descriptor) != 0) {
                return 56;
        }

        /* Keep more simultaneous children alive than the former eight-slot
         * process table allowed. Their inherited address spaces remain mostly
         * shared while each child blocks on the same pipe. */
        enum { FORK_STRESS_CHILDREN = 10 };
        int fork_stress_pipe[2];
        long fork_stress_children[FORK_STRESS_CHILDREN];
        size_t fork_stress_created = 0U;
        if (rose_pipe(fork_stress_pipe) != 0) {
                return 57;
        }
        for (size_t index = 0U; index < FORK_STRESS_CHILDREN; index++) {
                long child = rose_fork();
                if (child == 0) {
                        char byte;
                        bool passed =
                            rose_close(fork_stress_pipe[1]) == 0 &&
                            rose_read(fork_stress_pipe[0], &byte, 1U) == 0 &&
                            rose_close(fork_stress_pipe[0]) == 0;
                        rose_exit(passed ? 0U : 57U);
                }
                if (child < 0) {
                        (void)rose_close(fork_stress_pipe[0]);
                        (void)rose_close(fork_stress_pipe[1]);
                        for (size_t created = 0U; created < fork_stress_created;
                             created++) {
                                (void)rose_waitpid(
                                    fork_stress_children[created], NULL, 0U);
                        }
                        return 57;
                }
                fork_stress_children[fork_stress_created++] = child;
        }
        if (rose_close(fork_stress_pipe[0]) != 0 ||
            rose_close(fork_stress_pipe[1]) != 0) {
                return 58;
        }
        for (size_t index = 0U; index < fork_stress_created; index++) {
                fork_status = -1;
                if (rose_waitpid(fork_stress_children[index], &fork_status,
                                 0U) != fork_stress_children[index] ||
                    !USER_WAIT_STATUS_EXITED(fork_status) ||
                    USER_WAIT_STATUS_EXIT_CODE(fork_status) != 0U) {
                        return 58;
                }
        }

        struct user_signal_action signal_action = {
            .handler = (uintptr_t)signal_test_handler,
            .flags = 0U,
        };
        struct user_signal_action old_signal_action;
        struct user_signal_action ignored_signal_action = {
            .handler = USER_SIGNAL_IGNORE,
            .flags = 0U,
        };
        struct user_signal_action default_signal_action = {
            .handler = USER_SIGNAL_DEFAULT,
            .flags = 0U,
        };

        if (rose_sigaction(0, &signal_action, NULL) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_sigaction(USER_SIGNAL_KILL, &signal_action, NULL) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_sigaction(USER_SIGNAL_STOP, &signal_action, NULL) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_sigaction(USER_SIGNAL_USER_1,
                           (const struct user_signal_action *)kernel_address,
                           NULL) != -USER_ERROR_BAD_ADDRESS ||
            rose_sigaction(USER_SIGNAL_USER_1, &signal_action,
                           (struct user_signal_action *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_sigaction(USER_SIGNAL_USER_1, &signal_action,
                           &old_signal_action) != 0 ||
            old_signal_action.handler != USER_SIGNAL_DEFAULT ||
            old_signal_action.flags != 0U || rose_kill(-1, 0) != 0 ||
            rose_kill(INT64_MAX, 0) != -USER_ERROR_NO_PROCESS ||
            rose_kill(parent_pid, USER_SIGNAL_MAX + 1) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_kill(parent_pid, 0) != 0 ||
            rose_syscall(USER_SYSCALL_SIGNAL_RETURN, 0U, 0U, 0U) !=
                -USER_ERROR_INVALID_ARGUMENT) {
                return 59;
        }

        user_signal_count = 0U;
        if (rose_kill(parent_pid, USER_SIGNAL_USER_1) != 0 ||
            user_signal_count != 1U ||
            rose_sigaction(USER_SIGNAL_TERMINATE, &ignored_signal_action,
                           NULL) != 0 ||
            rose_kill(parent_pid, USER_SIGNAL_TERMINATE) != 0 ||
            user_signal_count != 1U ||
            rose_sigaction(USER_SIGNAL_TERMINATE, &default_signal_action,
                           NULL) != 0) {
                return 60;
        }

        /* Caught dispositions survive fork, while the handler's data and
         * interrupted frame remain private to each address space. */
        long signal_child = rose_fork();
        if (signal_child < 0) {
                return 61;
        }
        if (signal_child == 0) {
                long signal_child_pid = rose_getpid();
                if (user_signal_count != 1U || signal_child_pid <= 0 ||
                    rose_kill(signal_child_pid, USER_SIGNAL_USER_1) != 0 ||
                    user_signal_count != 2U) {
                        rose_exit(62U);
                }
                rose_exit(0U);
        }
        fork_status = -1;
        if (rose_waitpid(signal_child, &fork_status, 0U) != signal_child ||
            !USER_WAIT_STATUS_EXITED(fork_status) ||
            USER_WAIT_STATUS_EXIT_CODE(fork_status) != 0U ||
            user_signal_count != 1U) {
                return 63;
        }

        /* A default disposition terminates a target before it next enters
         * userspace and waitpid reports the signal rather than an exit code. */
        int signal_pipe[2] = {-1, -1};
        if (rose_pipe(signal_pipe) != 0) {
                return 64;
        }
        signal_child = rose_fork();
        if (signal_child < 0) {
                return 64;
        }
        if (signal_child == 0) {
                char blocked_byte;
                if (rose_close(signal_pipe[1]) != 0 ||
                    rose_read(signal_pipe[0], &blocked_byte, 1U) >= 0) {
                        rose_exit(64U);
                }
                rose_exit(64U);
        }
        if (rose_close(signal_pipe[0]) != 0) {
                return 65;
        }
        rose_yield();
        if (rose_kill(signal_child, USER_SIGNAL_TERMINATE) != 0 ||
            rose_close(signal_pipe[1]) != 0) {
                return 65;
        }
        fork_status = -1;
        if (rose_waitpid(signal_child, &fork_status, 0U) != signal_child ||
            !USER_WAIT_STATUS_SIGNALED(fork_status) ||
            USER_WAIT_STATUS_TERMINATION_SIGNAL(fork_status) !=
                USER_SIGNAL_TERMINATE) {
                return 66;
        }

        /* exec resets a caught disposition to default. */
        signal_child = rose_fork();
        if (signal_child < 0) {
                return 67;
        }
        if (signal_child == 0) {
                char *signal_arguments[] = {"/bin/signal-exec-test", NULL};
                char *signal_environment[] = {NULL};
                (void)rose_execve("/bin/signal-exec-test", signal_arguments,
                                  signal_environment);
                rose_exit(68U);
        }
        fork_status = -1;
        if (rose_waitpid(signal_child, &fork_status, 0U) != signal_child ||
            !USER_WAIT_STATUS_SIGNALED(fork_status) ||
            USER_WAIT_STATUS_TERMINATION_SIGNAL(fork_status) !=
                USER_SIGNAL_USER_1) {
                return 69;
        }

        char *child_arguments[] = {"/bin/hello", NULL};
        char *child_environment[] = {NULL};

        if (rose_waitpid(parent_pid, NULL, 0U) != -USER_ERROR_NO_CHILD ||
            rose_waitpid(-1, NULL, UINT32_C(0x80000000)) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_spawn("/missing", child_arguments, child_environment) !=
                -USER_ERROR_NO_ENTRY) {
                return 30;
        }

        long child_pid =
            rose_spawn("/bin/hello", child_arguments, child_environment);
        int wait_status = -1;

        if (child_pid <= 0 || child_pid == parent_pid ||
            rose_waitpid(child_pid, (int *)kernel_address, USER_WAIT_NO_HANG) !=
                -USER_ERROR_BAD_ADDRESS) {
                return 31;
        }

        long waited_pid = rose_waitpid(-1, &wait_status, USER_WAIT_NO_HANG);
        if (waited_pid == 0) {
                waited_pid = rose_waitpid(-1, &wait_status, 0U);
        }
        if (waited_pid != child_pid || !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U ||
            rose_waitpid(child_pid, NULL, USER_WAIT_NO_HANG) !=
                -USER_ERROR_NO_CHILD) {
                return 32;
        }

        /* Process groups provide group-directed signals and wait selectors.
         * Stop and continue retain distinct wait events without turning the
         * child into a zombie. */
        long process_group = rose_getpgrp();
        if (process_group <= 0 ||
            rose_tcgetpgrp(USER_STDIN_FILENO) != process_group ||
            rose_tcgetpgrp(99) != -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_tcsetpgrp(USER_STDIN_FILENO, process_group) != 0 ||
            rose_setpgid(0, 0) != 0 || rose_kill(0, 0) != 0 ||
            rose_setpgid(0, INT64_MAX) != -USER_ERROR_PERMISSION) {
                return 70;
        }

        long stopped_child = rose_fork();
        if (stopped_child < 0) {
                return 71;
        }
        if (stopped_child == 0) {
                while (true) {
                        rose_yield();
                }
        }
        if (rose_setpgid(stopped_child, stopped_child) != 0 ||
            rose_waitpid(0, NULL, USER_WAIT_NO_HANG) != -USER_ERROR_NO_CHILD ||
            rose_kill(-stopped_child, 0) != 0 ||
            rose_kill(-stopped_child, USER_SIGNAL_STOP) != 0) {
                (void)rose_kill(stopped_child, USER_SIGNAL_KILL);
                return 72;
        }

        int stopped_status = 0;
        if (rose_waitpid(stopped_child, &stopped_status, USER_WAIT_UNTRACED) !=
                stopped_child ||
            !USER_WAIT_STATUS_STOPPED(stopped_status) ||
            USER_WAIT_STATUS_STOP_SIGNAL(stopped_status) != USER_SIGNAL_STOP ||
            rose_waitpid(stopped_child, NULL,
                         USER_WAIT_NO_HANG | USER_WAIT_UNTRACED) != 0) {
                (void)rose_kill(stopped_child, USER_SIGNAL_KILL);
                return 73;
        }
        if (rose_kill(-stopped_child, USER_SIGNAL_CONTINUE) != 0 ||
            rose_waitpid(stopped_child, &stopped_status, USER_WAIT_CONTINUED) !=
                stopped_child ||
            !USER_WAIT_STATUS_CONTINUED(stopped_status) ||
            rose_kill(-stopped_child, USER_SIGNAL_TERMINATE) != 0) {
                (void)rose_kill(stopped_child, USER_SIGNAL_KILL);
                return 74;
        }
        if (rose_waitpid(stopped_child, &stopped_status, 0U) != stopped_child ||
            !USER_WAIT_STATUS_SIGNALED(stopped_status) ||
            USER_WAIT_STATUS_TERMINATION_SIGNAL(stopped_status) !=
                USER_SIGNAL_TERMINATE) {
                return 75;
        }

        descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        char *descriptor_arguments[] = {"/bin/descriptor-test", NULL};
        if (descriptor != 3 ||
            rose_set_descriptor_flags(99, 0U) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            rose_set_descriptor_flags((int)descriptor, UINT32_C(0x80000000)) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_set_descriptor_flags((int)descriptor,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0) {
                return 50;
        }

        child_pid = rose_spawn("/bin/descriptor-test", descriptor_arguments,
                               child_environment);
        wait_status = -1;
        if (child_pid <= 0 ||
            rose_waitpid(child_pid, &wait_status, 0U) != child_pid ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U ||
            rose_close((int)descriptor) != 0) {
                return 51;
        }

        struct user_shared_memory_info shared;
        if (rose_shared_memory_create(0U, &shared) !=
                -USER_ERROR_INVALID_ARGUMENT ||
            rose_shared_memory_create(
                4096U, (struct user_shared_memory_info *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_shared_memory_create(4096U, &shared) != 0 ||
            shared.identifier == 0U || shared.size != 4096U) {
                return 76;
        }
        volatile uint64_t *shared_value = (volatile uint64_t *)shared.address;
        *shared_value = UINT64_C(0x524f534553484d31);
        child_pid = rose_fork();
        if (child_pid == 0) {
                struct user_shared_memory_info child_mapping;
                if (rose_shared_memory_map(shared.identifier, &child_mapping) !=
                        0 ||
                    *(volatile uint64_t *)child_mapping.address !=
                        UINT64_C(0x524f534553484d31)) {
                        rose_exit(77U);
                }
                *(volatile uint64_t *)child_mapping.address =
                    UINT64_C(0x524f534553484d32);
                if (rose_shared_memory_unmap(shared.identifier) != 0) {
                        rose_exit(78U);
                }
                rose_exit(0U);
        }
        wait_status = -1;
        if (child_pid <= 0 ||
            rose_waitpid(child_pid, &wait_status, 0U) != child_pid ||
            !USER_WAIT_STATUS_EXITED(wait_status) ||
            USER_WAIT_STATUS_EXIT_CODE(wait_status) != 0U ||
            *shared_value != UINT64_C(0x524f534553484d32) ||
            rose_shared_memory_unmap(shared.identifier) != 0 ||
            rose_shared_memory_unmap(shared.identifier) !=
                -USER_ERROR_NO_ENTRY) {
                return 79;
        }
        print("Shared memory passed\n");

        int terminals[2];
        char terminal_byte = 0;
        if (rose_openpty((int *)kernel_address) != -USER_ERROR_BAD_ADDRESS ||
            rose_openpty(terminals) != 0 || terminals[0] != 3 ||
            terminals[1] != 4 ||
            rose_set_descriptor_flags(terminals[0], USER_DESCRIPTOR_NONBLOCK) !=
                0 ||
            rose_read(terminals[0], &terminal_byte, 1U) !=
                -USER_ERROR_TRY_AGAIN ||
            rose_write(terminals[0], "m", 1U) != 1 ||
            rose_read(terminals[1], &terminal_byte, 1U) != 1 ||
            terminal_byte != 'm' || rose_write(terminals[1], "s", 1U) != 1 ||
            rose_read(terminals[0], &terminal_byte, 1U) != 1 ||
            terminal_byte != 's' ||
            rose_fstat(terminals[0], &descriptor_status) != 0 ||
            descriptor_status.type != USER_FILE_PIPE ||
            rose_close(terminals[0]) != 0 || rose_close(terminals[1]) != 0) {
                return 80;
        }
        print("Pseudo-terminal passed\n");

        struct user_system_info system_information;
        if (rose_system_info((struct user_system_info *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_system_info(&system_information) != 0 ||
            system_information.total_pages == 0U ||
            system_information.used_pages == 0U ||
            system_information.copy_on_write_faults == 0U ||
            system_information.copy_on_write_copies == 0U) {
                return 81;
        }
        print("System telemetry passed\n");
        print("Copy-on-write passed\n");

        int mapping_test = run_anonymous_mapping_test();
        if (mapping_test != 0) {
                return mapping_test;
        }
        print("Anonymous mmap passed\n");

        print("Process hierarchy passed\n");
        print("Fork semantics passed\n");
        print("Signal delivery passed\n");
        print("Job control passed\n");
        print("Descriptor inheritance passed\n");
        print("Userspace heap passed\n");
        print("Syscall validation passed\n");
        return 0;
}

static int copy_descriptor_to_stdout(int descriptor) {
        char buffer[128];
        while (true) {
                long count = rose_read(descriptor, buffer, sizeof(buffer));

                if (count < 0) {
                        return 1;
                }
                if (count == 0) {
                        return 0;
                }
                if (!write_all(USER_STDOUT_FILENO, buffer, (size_t)count)) {
                        return 1;
                }
        }
}

static int run_cat(int argc, char **argv) {
        if (argc == 1) {
                return copy_descriptor_to_stdout(USER_STDIN_FILENO);
        }

        int status = 0;
        for (int index = 1; index < argc; index++) {
                long descriptor = rose_open(argv[index], USER_OPEN_READ);

                if (descriptor < 0) {
                        print("cat: unable to open: ");
                        print(argv[index]);
                        print("\n");
                        status = 1;
                        continue;
                }
                if (copy_descriptor_to_stdout((int)descriptor) != 0) {
                        print("cat: read failed: ");
                        print(argv[index]);
                        print("\n");
                        status = 1;
                }
                if (rose_close((int)descriptor) != 0) {
                        status = 1;
                }
        }

        return status;
}

static int run_ls(int argc, char **argv) {
        if (argc > 2) {
                print("Usage: ls [DIR]\n");
                return 1;
        }

        const char *path = argc == 2 ? argv[1] : ".";
        long descriptor = rose_open(path, USER_OPEN_READ | USER_OPEN_DIRECTORY);
        if (descriptor < 0) {
                print("ls: unable to open directory: ");
                print(path);
                print("\n");
                return 1;
        }

        struct user_directory_entry entry;
        long result;
        while ((result = rose_read_directory((int)descriptor, &entry)) > 0) {
                if (strings_equal(entry.name, ".") ||
                    strings_equal(entry.name, "..")) {
                        continue;
                }
                print(entry.name);
                if (entry.type == USER_FILE_DIRECTORY) {
                        print("/");
                }
                print("\n");
        }

        int status = result == 0 ? 0 : 1;
        if (result < 0) {
                print("ls: unable to read directory: ");
                print(path);
                print("\n");
        }
        return rose_close((int)descriptor) == 0 ? status : 1;
}

static int run_echo(int argc, char **argv) {
        for (int index = 1; index < argc; index++) {
                print(argv[index]);
                if (index + 1 < argc) {
                        print(" ");
                }
        }
        print("\n");
        return 0;
}

static int run_pwd(int argc) {
        if (argc != 1) {
                print("Usage: pwd\n");
                return 1;
        }
        char directory[64];
        if (rose_getcwd(directory, sizeof(directory)) < 0) {
                print("pwd: unable to read the working directory\n");
                return 1;
        }
        print(directory);
        print("\n");
        return 0;
}

static int run_env(int argc, char **environment) {
        if (argc != 1) {
                print("Usage: env\n");
                return 1;
        }
        for (size_t index = 0U; environment[index] != NULL; index++) {
                print(environment[index]);
                print("\n");
        }
        return 0;
}

static int run_mkdir(int argc, char **argv) {
        if (argc < 2) {
                print("Usage: mkdir DIR...\n");
                return 1;
        }

        int status = 0;
        for (int index = 1; index < argc; index++) {
                if (rose_mkdir(argv[index]) < 0) {
                        print("mkdir: unable to create: ");
                        print(argv[index]);
                        print("\n");
                        status = 1;
                }
        }
        return status;
}

static int run_rm(int argc, char **argv) {
        if (argc < 2) {
                print("Usage: rm PATH...\n");
                return 1;
        }

        int status = 0;
        for (int index = 1; index < argc; index++) {
                if (rose_unlink(argv[index]) < 0) {
                        print("rm: unable to remove: ");
                        print(argv[index]);
                        print("\n");
                        status = 1;
                }
        }
        return status;
}

static int run_descriptor_test(void) {
        struct user_file_status status;

        if (rose_fstat(3, &status) != -USER_ERROR_BAD_FILE_DESCRIPTOR) {
                return 1;
        }
        return 0;
}

/* A caught disposition must reset to default across exec. The calling test
 * expects this image to be terminated before rose_kill can return. */
static int run_signal_exec_test(void) {
        long pid = rose_getpid();

        if (pid <= 0 || rose_kill(pid, USER_SIGNAL_USER_1) != 0) {
                return 57;
        }
        return 58;
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
        long close_on_exec_descriptor = rose_open("/etc/motd", USER_OPEN_READ);
        char first_character;
        long heap_query = rose_brk(0U);

        if (descriptor != 3 || close_on_exec_descriptor != 4 ||
            rose_set_descriptor_flags((int)close_on_exec_descriptor,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0 ||
            rose_read((int)descriptor, &first_character, 1U) != 1 ||
            first_character != 'W' || heap_query <= 0 ||
            rose_brk((uintptr_t)heap_query + 1U) != heap_query + 1) {
                return 25;
        }

        volatile uint8_t *heap = (volatile uint8_t *)(uintptr_t)heap_query;
        heap[0] = UINT8_C(0x5a);

        long mapping_result = rose_mmap(
            4096U, USER_MEMORY_PROTECTION_READ | USER_MEMORY_PROTECTION_WRITE);
        if (mapping_result <= 0) {
                return 106;
        }
        volatile uint8_t *mapping =
            (volatile uint8_t *)(uintptr_t)mapping_result;
        mapping[0] = UINT8_C(0x6b);

        for (size_t index = 0U; index < 17U; index++) {
                too_many_arguments[index] = "x";
        }

        /* Every failure must return to this unchanged image. */
        if (rose_execve((const char *)kernel_address, arguments, environment) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/missing", arguments, environment) !=
                -USER_ERROR_NO_ENTRY ||
            rose_execve("/etc/motd", arguments, environment) !=
                -USER_ERROR_EXEC_FORMAT ||
            rose_execve("/bin/execve-target", (char *const *)kernel_address,
                        environment) != -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/bin/execve-target", arguments,
                        (char *const *)kernel_address) !=
                -USER_ERROR_BAD_ADDRESS ||
            rose_execve("/bin/execve-target", too_many_arguments,
                        environment) != -USER_ERROR_ARGUMENT_LIST_TOO_LONG) {
                return 26;
        }
        if (rose_brk(0U) != heap_query + 1 || heap[0] != UINT8_C(0x5a) ||
            mapping[0] != UINT8_C(0x6b)) {
                return 37;
        }

        /* Success cannot return. In addition to replacing the heap and ELF, it
         * releases the resident anonymous mapping above. The target verifies
         * copied vectors, descriptor state, and the inherited directory. */
        if (rose_chdir("/bin") != 0) {
                return 41;
        }
        long result = rose_execve("execve-target", arguments, environment);
        return result < 0 ? 27 : 28;
}

static int run_execve_target(int argc, char **argv, char **environment) {
        const char *test_value =
            find_environment_value(environment, "EXECVE_TEST");
        char second_character;
        char current_directory[64];
        struct user_file_status descriptor_status;
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
            second_character != 'e' ||
            rose_fstat(4, &descriptor_status) !=
                -USER_ERROR_BAD_FILE_DESCRIPTOR ||
            heap_query <= 0 ||
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
        enum {
                LARGE_FILE_SIZE = 13 * 1024 + 37,
                FILESYSTEM_TRANSFER_SIZE = 257,
        };
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

        long descriptor =
            rose_open("/tmp/state", USER_OPEN_READ | USER_OPEN_WRITE |
                                        USER_OPEN_CREATE | USER_OPEN_TRUNCATE);
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
            !strings_equal(buffer, payload) ||
            rose_close((int)descriptor) != 0) {
                return 21;
        }

        /* Cross both the twelfth direct-block boundary and unaligned block
         * offsets, then reread the complete singly indirect file. */
        descriptor = rose_open("/tmp/state", USER_OPEN_READ | USER_OPEN_WRITE |
                                                 USER_OPEN_TRUNCATE);
        static uint8_t large_buffer[FILESYSTEM_TRANSFER_SIZE];
        size_t position = 0U;
        while (position < LARGE_FILE_SIZE) {
                size_t transfer = LARGE_FILE_SIZE - position;
                if (transfer > sizeof(large_buffer)) {
                        transfer = sizeof(large_buffer);
                }
                for (size_t index = 0U; index < transfer; index++) {
                        large_buffer[index] =
                            (uint8_t)((position + index) * 37U + 11U);
                }
                if (descriptor < 0 || rose_write((int)descriptor, large_buffer,
                                                 transfer) != (long)transfer) {
                        return 42;
                }
                position += transfer;
        }
        if (rose_stat("/tmp/state", &status) != 0 ||
            status.size != LARGE_FILE_SIZE ||
            rose_lseek((int)descriptor, 0, USER_SEEK_SET) != 0) {
                return 43;
        }
        position = 0U;
        while (position < LARGE_FILE_SIZE) {
                size_t transfer = LARGE_FILE_SIZE - position;
                if (transfer > sizeof(large_buffer)) {
                        transfer = sizeof(large_buffer);
                }
                if (rose_read((int)descriptor, large_buffer, transfer) !=
                    (long)transfer) {
                        return 44;
                }
                for (size_t index = 0U; index < transfer; index++) {
                        uint8_t expected =
                            (uint8_t)((position + index) * 37U + 11U);
                        if (large_buffer[index] != expected) {
                                return 44;
                        }
                }
                position += transfer;
        }
        if (rose_close((int)descriptor) != 0) {
                return 44;
        }

        descriptor =
            rose_open("/tmp/state", USER_OPEN_WRITE | USER_OPEN_TRUNCATE);
        if (descriptor < 0 || rose_stat("/tmp/state", &status) != 0 ||
            status.size != 0U || rose_close((int)descriptor) != 0) {
                return 45;
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
            descriptors[1] != 4 || rose_fstat(descriptors[0], &status) != 0 ||
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
                    descriptors[0], streaming_buffer, sizeof(streaming_buffer));
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

        if (rose_pipe(descriptors) != 0 || rose_close(descriptors[0]) != 0 ||
            rose_write(descriptors[1], "x", 1U) != -USER_ERROR_BROKEN_PIPE ||
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
        if (user_cookie != UINT64_C(0x524f5345) || user_counter != 0U ||
            user_signal_count != 0U) {
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
                return run_cat(argc, argv);

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

        case USER_PROGRAM_LS:
                return run_ls(argc, argv);

        case USER_PROGRAM_ECHO:
                return run_echo(argc, argv);

        case USER_PROGRAM_PWD:
                return run_pwd(argc);

        case USER_PROGRAM_ENV:
                return run_env(argc, environment);

        case USER_PROGRAM_MKDIR:
                return run_mkdir(argc, argv);

        case USER_PROGRAM_RM:
                return run_rm(argc, argv);

        case USER_PROGRAM_DESCRIPTOR_TEST:
                return run_descriptor_test();

        case USER_PROGRAM_SIGNAL_EXEC_TEST:
                return run_signal_exec_test();

        case USER_PROGRAM_DESKTOP:
                return rose_desktop_main(argc, argv);

        case USER_PROGRAM_GUI_TERMINAL:
                return rose_gui_terminal_main(argc, argv);

        case USER_PROGRAM_GUI_FILES:
                return rose_gui_files_main(argc, argv);

        case USER_PROGRAM_GUI_SYSTEM_MONITOR:
                return rose_gui_monitor_main(argc, argv);

        default:
                return 2;
        }
}
