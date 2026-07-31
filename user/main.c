/*
 * Freestanding U-mode demonstration program.
 *
 * The build produces one small ELF for each /bin path in the initial ramfs.
 * ROSE_PROGRAM is constant for a given image, so the compiler discards the
 * other demonstrations while the sources continue sharing one runtime.
 */
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
        (void)rose_write(text, string_length(text));
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

        if (rose_write(kernel_address, 1U) != -USER_ERROR_BAD_ADDRESS) {
                return 4;
        }
        /* The raw wrapper lets this test issue a deliberately unknown number. */
        if (rose_syscall(UINT64_C(0xffff), 0U, 0U) !=
            -USER_ERROR_NOT_IMPLEMENTED) {
                return 5;
        }

        print("Syscall validation passed\n");
        return 0;
}

/* Referenced by the assembly entry point, so this must retain external linkage.
 */
int user_main(void) { // NOLINT(misc-use-internal-linkage)
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

        default:
                return 2;
        }
}
