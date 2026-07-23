/*
 */


.section .text

.globl trap_entry
.type trap_entry, @function

/*
 * RV64 Supervisor-mode trap frame.
 *
 * 35 stored 64-bit values:
 *   31 general-purpose registers, excluding x0
 *   sepc
 *   sstatus
 *   scause
 *   stval
 *
 * Data size: 280 bytes
 * Allocated size: 288 bytes for 16-byte stack alignment (required by riscv)
 */
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
.equ TF_SCAUSE,  264
.equ TF_STVAL,   272

.equ TRAP_FRAME_SIZE, 288


trap_entry:
    /*
     * Allocate the trap frame on the current stack.
     */
    addi sp, sp, -TRAP_FRAME_SIZE

    /*
     * Save t0 before using as temp register.
     */
    sd t0, TF_T0(sp)

    /*
     * Reconstruct and save the interrupted sp.
     */
    addi t0, sp, TRAP_FRAME_SIZE
    sd t0, TF_SP(sp)

    /*
     * Save all remaining integer registers.
     */
    sd ra, TF_RA(sp)
    sd gp, TF_GP(sp)
    sd tp, TF_TP(sp)

    sd t1, TF_T1(sp)
    sd t2, TF_T2(sp)

    sd s0, TF_S0(sp)
    sd s1, TF_S1(sp)

    sd a0, TF_A0(sp)
    sd a1, TF_A1(sp)
    sd a2, TF_A2(sp)
    sd a3, TF_A3(sp)
    sd a4, TF_A4(sp)
    sd a5, TF_A5(sp)
    sd a6, TF_A6(sp)
    sd a7, TF_A7(sp)

    sd s2,  TF_S2(sp)
    sd s3,  TF_S3(sp)
    sd s4,  TF_S4(sp)
    sd s5,  TF_S5(sp)
    sd s6,  TF_S6(sp)
    sd s7,  TF_S7(sp)
    sd s8,  TF_S8(sp)
    sd s9,  TF_S9(sp)
    sd s10, TF_S10(sp)
    sd s11, TF_S11(sp)

    sd t3, TF_T3(sp)
    sd t4, TF_T4(sp)
    sd t5, TF_T5(sp)
    sd t6, TF_T6(sp)

    /*
     * Save Supervisor-mode trap CSRs.
     */
    csrr t0, sepc
    sd t0, TF_SEPC(sp)

    csrr t0, sstatus
    sd t0, TF_SSTATUS(sp)

    csrr t0, scause
    sd t0, TF_SCAUSE(sp)

    csrr t0, stval
    sd t0, TF_STVAL(sp)

    mv a0, sp

    /*
     * void trap_handler(struct trap_frame *frame);
     */
    call trap_handler

    /*
     * Restore the return PC and supervisor status.
     */
     ld t0, TF_SEPC(sp)
     csrw sepc, t0

     ld t0, TF_SSTATUS(sp)
     csrw sstatus, t0

     /*
      * Restore all registers except sp.
      */
     ld ra, TF_RA(sp)
     ld gp, TF_GP(sp)
     ld tp, TF_TP(sp)

     ld t0, TF_T1(sp)
     ld t1, TF_T1(sp)
     ld t2, TF_T2(sp)

     ld s0, TF_S0(sp)
     ld s1, TF_S1(sp)

     ld a0, TF_A0(sp)
     ld a1, TF_A1(sp)
     ld a2, TF_A2(sp)
     ld a3, TF_A3(sp)
     ld a4, TF_A4(sp)
     ld a5, TF_A5(sp)
     ld a6, TF_A6(sp)
     ld a7, TF_A7(sp)

     ld s2,  TF_S2(sp)
     ld s3,  TF_S3(sp)
     ld s4,  TF_S4(sp)
     ld s5,  TF_S5(sp)
     ld s6,  TF_S6(sp)
     ld s7,  TF_S7(sp)
     ld s8,  TF_S8(sp)
     ld s9,  TF_S9(sp)
     ld s10, TF_S10(sp)
     ld s11, TF_S11(sp)

     ld t3, TF_T3(sp)
     ld t4, TF_T4(sp)
     ld t5, TF_T5(sp)
     ld t6, TF_T6(sp)

     /*
      * Release the frame. This restores the original sp because the frame
      * was allocated directly on that stack.
      */
     addi sp, sp, TRAP_FRAME_SIZE

     sret

.size trap_entry, . - trap_entry
