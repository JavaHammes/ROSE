/* Embed the independently linked user ELF as read-only kernel data. */
.section .user_image,"a",@progbits
.balign 16

.globl user_program_elf_start
user_program_elf_start:
    .incbin "kernel/build/user/program.load.elf"

.globl user_program_elf_end
user_program_elf_end:

.section .note.GNU-stack,"",@progbits
