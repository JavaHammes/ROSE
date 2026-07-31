/*
 * Enter one U-mode program while preserving the supervisor call context.
 *
 * void user_mode_enter(const struct trap_frame *context,
 *                      uintptr_t kernel_trap_stack_top);
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

/* Offsets shared with struct trap_frame in trap.h. */
.equ TF_RA,        0
.equ TF_SP,        8
.equ TF_GP,       16
.equ TF_TP,       24
.equ TF_T0,       32
.equ TF_T1,       40
.equ TF_T2,       48
.equ TF_S0,       56
.equ TF_S1,       64
.equ TF_A0,       72
.equ TF_A1,       80
.equ TF_A2,       88
.equ TF_A3,       96
.equ TF_A4,      104
.equ TF_A5,      112
.equ TF_A6,      120
.equ TF_A7,      128
.equ TF_S2,      136
.equ TF_S3,      144
.equ TF_S4,      152
.equ TF_S5,      160
.equ TF_S6,      168
.equ TF_S7,      176
.equ TF_S8,      184
.equ TF_S9,      192
.equ TF_S10,     200
.equ TF_S11,     208
.equ TF_T3,      216
.equ TF_T4,      224
.equ TF_T5,      232
.equ TF_T6,      240
.equ TF_SEPC,    248
.equ TF_SSTATUS, 256

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

    /* Keep the context pointer and trusted stack in temporaries until their
     * saved user values are restored at the very end. */
    mv t6, a0
    mv t5, a1

    /* Prevent an S-mode interrupt during the sscratch transition window. */
    csrci sstatus, SSTATUS_SIE

    /* Configure the next trap to switch to the trusted kernel stack. */
    csrw sscratch, t5

    /* Restore the process trap state, forcing a U-mode return with interrupts
     * enabled there. */
    ld   t0, TF_SSTATUS(t6)
    li   t1, (SSTATUS_SIE | SSTATUS_SPP)
    not  t1, t1
    and  t0, t0, t1
    ori  t0, t0, SSTATUS_SPIE
    csrw sstatus, t0
    ld   t0, TF_SEPC(t6)
    csrw sepc, t0

    /* Restore the complete saved integer context. A blocked syscall relies on
     * ra, a7, and sepc surviving a trip through the supervisor idle loop. */
    ld ra, TF_RA(t6)
    ld sp, TF_SP(t6)
    ld gp, TF_GP(t6)
    ld tp, TF_TP(t6)
    ld t0, TF_T0(t6)
    ld t1, TF_T1(t6)
    ld t2, TF_T2(t6)
    ld s0, TF_S0(t6)
    ld s1, TF_S1(t6)
    ld a0, TF_A0(t6)
    ld a1, TF_A1(t6)
    ld a2, TF_A2(t6)
    ld a3, TF_A3(t6)
    ld a4, TF_A4(t6)
    ld a5, TF_A5(t6)
    ld a6, TF_A6(t6)
    ld a7, TF_A7(t6)
    ld s2, TF_S2(t6)
    ld s3, TF_S3(t6)
    ld s4, TF_S4(t6)
    ld s5, TF_S5(t6)
    ld s6, TF_S6(t6)
    ld s7, TF_S7(t6)
    ld s8, TF_S8(t6)
    ld s9, TF_S9(t6)
    ld s10, TF_S10(t6)
    ld s11, TF_S11(t6)
    ld t3, TF_T3(t6)
    ld t4, TF_T4(t6)
    ld t5, TF_T5(t6)
    ld t6, TF_T6(t6)

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
