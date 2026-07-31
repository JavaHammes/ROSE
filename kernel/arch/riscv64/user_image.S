/* Embed the independently linked ramfs executables as read-only kernel data. */
.section .user_image,"a",@progbits
.balign 16

.globl user_program_elf_start
user_program_elf_start:

.globl user_hello_elf_start
user_hello_elf_start:
    .incbin "kernel/build/user/hello/program.load.elf"
.globl user_hello_elf_end
user_hello_elf_end:

.balign 16
.globl user_fault_elf_start
user_fault_elf_start:
    .incbin "kernel/build/user/fault/program.load.elf"
.globl user_fault_elf_end
user_fault_elf_end:

.balign 16
.globl user_process_a_elf_start
user_process_a_elf_start:
    .incbin "kernel/build/user/process_a/program.load.elf"
.globl user_process_a_elf_end
user_process_a_elf_end:

.balign 16
.globl user_process_b_elf_start
user_process_b_elf_start:
    .incbin "kernel/build/user/process_b/program.load.elf"
.globl user_process_b_elf_end
user_process_b_elf_end:

.balign 16
.globl user_syscall_test_elf_start
user_syscall_test_elf_start:
    .incbin "kernel/build/user/syscall_test/program.load.elf"
.globl user_syscall_test_elf_end
user_syscall_test_elf_end:

.globl user_program_elf_end
user_program_elf_end:

.section .note.GNU-stack,"",@progbits
