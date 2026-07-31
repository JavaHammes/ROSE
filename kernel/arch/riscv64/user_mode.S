/*
 * Enter one U-mode program while preserving the supervisor call context.
 *
 * void user_mode_enter(uintptr_t entry, uintptr_t user_stack_top,
 *                      uintptr_t kernel_trap_stack_top,
 *                      uintptr_t initial_argument);
 */

.section .text,"ax",@progbits
.align 2

.equ KCTX_RA,    0
.equ KCTX_GP,    8
.equ KCTX_TP,   16
.equ KCTX_S0,   24
.equ KCTX_S1,   32
.equ KCTX_S2,   40
.equ KCTX_S3,   48
.equ KCTX_S4,   56
.equ KCTX_S5,   64
.equ KCTX_S6,   72
.equ KCTX_S7,   80
.equ KCTX_S8,   88
.equ KCTX_S9,   96
.equ KCTX_S10, 104
.equ KCTX_S11, 112
.equ KCTX_SIZE, 128

.equ SSTATUS_SIE,  (1 << 1)
.equ SSTATUS_SPIE, (1 << 5)
.equ SSTATUS_SPP,  (1 << 8)

.globl user_mode_enter
.type user_mode_enter, @function
user_mode_enter:
    /* Preserve every supervisor register that the C ABI requires us to keep. */
    addi sp, sp, -KCTX_SIZE
    sd ra,  KCTX_RA(sp)
    sd gp,  KCTX_GP(sp)
    sd tp,  KCTX_TP(sp)
    sd s0,  KCTX_S0(sp)
    sd s1,  KCTX_S1(sp)
    sd s2,  KCTX_S2(sp)
    sd s3,  KCTX_S3(sp)
    sd s4,  KCTX_S4(sp)
    sd s5,  KCTX_S5(sp)
    sd s6,  KCTX_S6(sp)
    sd s7,  KCTX_S7(sp)
    sd s8,  KCTX_S8(sp)
    sd s9,  KCTX_S9(sp)
    sd s10, KCTX_S10(sp)
    sd s11, KCTX_S11(sp)

    la t0, user_saved_kernel_context_sp
    sd sp, 0(t0)

    /* Prevent an S-mode interrupt during the sscratch transition window. */
    csrci sstatus, SSTATUS_SIE

    /* Configure the next trap to switch to the trusted kernel stack. */
    csrw sscratch, a2

    /* sret should enter U-mode with supervisor interrupts enabled. */
    csrr t0, sstatus
    li   t1, SSTATUS_SPP
    not  t1, t1
    and  t0, t0, t1
    ori  t0, t0, SSTATUS_SPIE
    csrw sstatus, t0
    csrw sepc, a0

    mv sp, a1

    /* Do not leak the supervisor register state into the user program. */
    mv ra, zero
    mv gp, zero
    mv tp, zero
    mv t0, zero
    mv t1, zero
    mv t2, zero
    mv s0, zero
    mv s1, zero
    /* Pass the kernel-selected program value to the C entry point in a0. */
    mv a0, a3
    mv a1, zero
    mv a2, zero
    mv a3, zero
    mv a4, zero
    mv a5, zero
    mv a6, zero
    mv a7, zero
    mv s2, zero
    mv s3, zero
    mv s4, zero
    mv s5, zero
    mv s6, zero
    mv s7, zero
    mv s8, zero
    mv s9, zero
    mv s10, zero
    mv s11, zero
    mv t3, zero
    mv t4, zero
    mv t5, zero
    mv t6, zero

    sret

.size user_mode_enter, . - user_mode_enter

/*
 * trap_handler redirects a process exit here in S-mode with sp pointing at
 * the preserved kernel context above.
 */
.globl user_mode_resume
.type user_mode_resume, @function
user_mode_resume:
    ld ra,  KCTX_RA(sp)
    ld gp,  KCTX_GP(sp)
    ld tp,  KCTX_TP(sp)
    ld s0,  KCTX_S0(sp)
    ld s1,  KCTX_S1(sp)
    ld s2,  KCTX_S2(sp)
    ld s3,  KCTX_S3(sp)
    ld s4,  KCTX_S4(sp)
    ld s5,  KCTX_S5(sp)
    ld s6,  KCTX_S6(sp)
    ld s7,  KCTX_S7(sp)
    ld s8,  KCTX_S8(sp)
    ld s9,  KCTX_S9(sp)
    ld s10, KCTX_S10(sp)
    ld s11, KCTX_S11(sp)
    addi sp, sp, KCTX_SIZE
    ret

.size user_mode_resume, . - user_mode_resume

.section .bss,"aw",@nobits
.align 3
.globl user_saved_kernel_context_sp
.type user_saved_kernel_context_sp, @object
user_saved_kernel_context_sp:
    .zero 8
.size user_saved_kernel_context_sp, 8
