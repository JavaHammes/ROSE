 /*
 * A trap is any event that causes the CPU to stop normal instruction flow and
 * jump to a trap handler.
 *
 * There are two types:
 *
 * 1. Exception (synchronous)
 *    Caused directly by the currently executing instruction.
 *    Examples: illegal instruction, misaligned access, page fault, ecall.
 *
 * 2. Interrupt (asynchronous)
 *    Caused by an external or independently running source.
 *    Examples: timer tick, UART receive, GPIO event.
 *
 * When a trap is taken into M-mode or S-mode, the CPU:
 *
 * 1. Saves the interrupted instruction address in mepc or sepc.
 * 2. Stores the trap cause in mcause or scause.
 * 3. Stores additional trap-specific information in mtval or stval.
 * 4. Updates privilege and interrupt-enable fields in mstatus or sstatus.
 * 5. Jumps to the trap handler address configured in mtvec or stvec.
 *
 * The trap handler normally:
 *
 * 1. Saves the remaining CPU registers into a trap frame.
 * 2. Handles the exception or interrupt.
 * 3. Restores the saved registers.
 * 4. Executes mret or sret to resume execution.
 *
 * mepc / sepc:
 *   Machine/Supervisor Exception Program Counter.
 *   Contains the address at which execution should resume after the trap.
 *
 *   For interrupts, this normally points to the next instruction that would
 *   have executed.
 *
 *   For exceptions, it normally points to the instruction that caused the
 *   exception. The handler may need to increment it before returning, for
 *   example after handling an ecall.
 *
 * mcause / scause:
 *   Machine/Supervisor Cause register.
 *   Describes why the trap occurred.
 *
 *   The most significant bit indicates whether the trap was an interrupt:
 *
 *     0 = exception
 *     1 = interrupt
 *
 *   The remaining bits contain the exception or interrupt cause code.
 *
 * mtval / stval:
 *   Machine/Supervisor Trap Value register.
 *   Contains additional information about the trap when applicable.
 *
 *   Examples:
 *   - Faulting virtual or physical address for an access fault or page fault
 *   - Faulting instruction bits for some illegal-instruction exceptions
 *   - Zero when no additional information is provided
 *
 * mstatus / sstatus:
 *   Machine/Supervisor Status register.
 *   Contains privilege-state and interrupt-control fields.
 *
 * mpp / spp:
 *   Machine Previous Privilege / Supervisor Previous Privilege.
 *   Records the privilege mode that was active before the trap.
 *
 *   mret or sret uses this field to determine the privilege mode to which
 *   execution returns.
 *
 *   MPP can represent M-mode, S-mode or U-mode.
 *   SPP represents either S-mode or U-mode.
 *
 * mpie / spie:
 *   Machine Previous Interrupt Enable / Supervisor Previous Interrupt Enable.
 *   Stores the previous value of MIE or SIE when the trap is taken.
 *
 *   This allows mret or sret to restore the previous interrupt-enable state.
 *
 * mie / sie:
 *   Machine Interrupt Enable / Supervisor Interrupt Enable.
 *   Globally enables interrupts at the corresponding privilege level.
 *
 *   When a trap is taken:
 *
 *     MPIE = MIE
 *     MIE  = 0
 *
 *   or:
 *
 *     SPIE = SIE
 *     SIE  = 0
 *
 * mtvec / stvec:
 *   Machine/Supervisor Trap-Vector Base-Address register.
 *   Specifies where the CPU jumps when a trap occurs.
 *
 *   It supports two common modes:
 *
 *   - Direct mode:
 *       All traps jump to the same handler address.
 *
 *   - Vectored mode:
 *       Exceptions jump to the base address, while interrupts jump to:
 *
 *         BASE + 4 * cause
 *
 * mret / sret:
 *   Machine/Supervisor Return-from-Trap instruction.
 *
 *   Restores the previous privilege and interrupt-enable state and sets:
 *
 *     pc = mepc
 *
 *   or:
 *
 *     pc = sepc
 *
 */


/*
 * Supervisor-mode trap entry for RV64.
 *
 * stvec should point to trap_entry, usually in direct mode:
 *      csrw stvec, trap_entry
 *
 * When a delegated exception or interrupt is taken into S-mode, the CPU
 * automatically updates the Supervisor trap CSRs and begins execution here.
 *
 * The CPU does not automatically save the integer registers. Therefore, this
 * function must preserve the complete interrupted register state before
 * calling any C code.
 */
.section .text
.align 2

.globl trap_entry
.type trap_entry, @function

/*
 * RV64 Supervisor-mode trap frame
 * --------------------------------
 *
 * This structure contains the complete integer state of the interrupted
 * execution context, together with the most important Supervisor trap CSRs.
 *
 * x0 is not stored because it is hard-wired to zero and cannot change.
 *
 * Stored values:
 *
 *   31 general-purpose registers:
 *     x1-x31, excluding x0
 *
 *   4 Supervisor trap CSRs:
 *     sepc
 *     sstatus
 *     scause
 *     stval
 *
 * Total stored values:
 *
 *     31 + 4 = 35 values
 *
 * Each RV64 register occupies 8 bytes:
 *
 *     35 * 8 = 280 bytes
 *
 * The allocated frame is rounded up to 288 bytes so that sp remains aligned
 * to a 16-byte boundary when trap_handler is called.
 *
 * The RISC-V ABI requires the stack pointer to be 16-byte aligned at function
 * call boundaries.
 *
 * Conceptual C representation:
 *
 * struct trap_frame {
 *     uint64_t ra;
 *     uint64_t sp;
 *     uint64_t gp;
 *     uint64_t tp;
 *
 *     uint64_t t0;
 *     uint64_t t1;
 *     uint64_t t2;
 *
 *     uint64_t s0;
 *     uint64_t s1;
 *
 *     uint64_t a0;
 *     uint64_t a1;
 *     uint64_t a2;
 *     uint64_t a3;
 *     uint64_t a4;
 *     uint64_t a5;
 *     uint64_t a6;
 *     uint64_t a7;
 *
 *     uint64_t s2;
 *     uint64_t s3;
 *     uint64_t s4;
 *     uint64_t s5;
 *     uint64_t s6;
 *     uint64_t s7;
 *     uint64_t s8;
 *     uint64_t s9;
 *     uint64_t s10;
 *     uint64_t s11;
 *
 *     uint64_t t3;
 *     uint64_t t4;
 *     uint64_t t5;
 *     uint64_t t6;
 *
 *     uint64_t sepc;
 *     uint64_t sstatus;
 *     uint64_t scause;
 *     uint64_t stval;
 *
 *     uint64_t padding;
 * };
 */

/*
 * Register offsets relative to the trap-frame base.
 *
 * After allocation, sp points to the first byte of this frame.
 */

/* Return address, stack pointer, global pointer and thread pointer. */
.equ TF_RA,        0
.equ TF_SP,        8
.equ TF_GP,       16
.equ TF_TP,       24

/* Temporary registers x5-x7. */
.equ TF_T0,       32
.equ TF_T1,       40
.equ TF_T2,       48

/* Saved registers x8-x9 */
.equ TF_S0,       56
.equ TF_S1,       64

/* Function argument and return-value registers x10-x17. */
.equ TF_A0,       72
.equ TF_A1,       80
.equ TF_A2,       88
.equ TF_A3,       96
.equ TF_A4,      104
.equ TF_A5,      112
.equ TF_A6,      120
.equ TF_A7,      128

/* Remaining callee-saved registers x18-x27. */
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

/* Remaining temporary registers x28-x31. */
.equ TF_T3,      216
.equ TF_T4,      224
.equ TF_T5,      232
.equ TF_T6,      240

/* Supervisor trap state. */
.equ TF_SEPC,    248
.equ TF_SSTATUS, 256
.equ TF_SCAUSE,  264
.equ TF_STVAL,   272

/*
 * Bytes 280-287 are unused padding.
 *
 * They keep the total allocation divisible by 16.
 */
.equ TRAP_FRAME_SIZE, 288


/*
 * void trap_entry(void);
 *
 * This is not a normal C-callable function. The CPU enters it directly through
 * stvec when a Supervisor-mode trap occurs.
 *
 * On entry:
 *
 *   - sepc contains the interrupted or faulting PC.
 *   - scause identifies the exception or interrupt.
 *   - stval may contain additional fault information.
 *   - sstatus contains the previous privilege and interrupt state.
 *   - SIE has been cleared by the CPU.
 *   - General-purpose registers still contain the interrupted context.
 *
 * On exit:
 *
 *   - All integer registers are restored.
 *   - sepc and sstatus are restored from the trap frame.
 *   - sret resumes the interrupted context.
 */
trap_entry:

    /*
     * Allocate space for the trap frame.
     *
     * Before this instruction, sp is the stack pointer of the interrupted
     * context.
     *
     * After this instruction, sp points to the newly allocated trap frame:
     *
     *     old sp
     *       |
     *       v
     *     +-------------------+
     *     | previous stack    |
     *     +-------------------+  <- old sp
     *     | trap frame        |
     *     | 288 bytes         |
     *     +-------------------+  <- new sp
     */
    addi sp, sp, -TRAP_FRAME_SIZE

    /*
     * Save t0 immediately.
     *
     * We need a temporary register to reconstruct the interrupted value of sp.
     * Saving t0 first allows us to use t0 without destroying the interrupted
     * context.
     */
    sd t0, TF_T0(sp)

    /*
     * Reconstruct the interrupted stack pointer.
     *
     * The current sp points to the bottom of the trap frame. Adding the frame
     * size gives us the value that sp had immediately before trap_entry
     * allocated the frame.
     *
     * We store that original value in TF_SP.
     *
     * We cannot simply execute:
     *
     *     sd sp, TF_SP(sp)
     *
     * because that would save the current trap-frame pointer rather than the
     * interrupted stack pointer.
     */
    addi t0, sp, TRAP_FRAME_SIZE
    sd t0, TF_SP(sp)

    /*
     * Save all remaining general-purpose registers.
     *
     * x0 is not saved because reading x0 always produces zero.
     *
     * Although the RISC-V calling convention allows normal C functions to
     * overwrite caller-saved registers, a trap can happen between any two
     * instructions. Therefore, the trap entry must preserve both caller-saved
     * and callee-saved registers.
     */
    sd ra, TF_RA(sp)
    sd gp, TF_GP(sp)
    sd tp, TF_TP(sp)

    sd t1, TF_T1(sp)
    sd t2, TF_T2(sp)

    sd s0, TF_S0(sp)
    sd s1, TF_S1(sp)

    /*
     * Argument registers.
     *
     * These may contain:
     *
     *   - ordinary function arguments,
     *   - temporary values from interrupted code,
     *   - syscall arguments if the exception was caused by ecall.
     *
     * Under the conventional RISC-V syscall ABI:
     *
     *   a0-a5 = syscall arguments
     *   a7    = syscall number
     *   a0    = return value
     */
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
     * Save the Supervisor trap CSRs.
     *
     * These registers were populated or updated by the CPU when it took the
     * trap.
     *
     * They are copied into the trap frame so that the C handler can inspect
     * and, when necessary, modify the return state.
     */

    /*
     * sepc: Supervisor Exception Program Counter.
     *
     * This is the PC that sret will load when returning.
     *
     * For an ecall exception, sepc points to the ecall instruction itself.
     * The handler normally advances sepc by 4 so that execution resumes at the
     * instruction following ecall.
     */
    csrr t0, sepc
    sd t0, TF_SEPC(sp)

    /*
     * sstatus: Supervisor status.
     *
     * Important fields include:
     *
     *   SPP  - privilege mode before the trap
     *   SPIE - previous value of SIE
     *   SIE  - Supervisor interrupt-enable state
     *
     * sret uses these fields to restore the previous execution state.
     */
    csrr t0, sstatus
    sd t0, TF_SSTATUS(sp)

    /*
     * scause: Supervisor trap cause.
     *
     * The most significant bit indicates the trap type:
     *
     *   0 = exception
     *   1 = interrupt
     *
     * The remaining bits contain the cause number.
     *
     * Examples:
     *
     *   8 = environment call from U-mode
     *   9 = environment call from S-mode
     *   5 = Supervisor timer interrupt, when the interrupt bit is set
     */
    csrr t0, scause
    sd t0, TF_SCAUSE(sp)

     /*
     * stval: Supervisor trap value.
     *
     * Its meaning depends on scause. It may contain:
     *
     *   - a faulting virtual address,
     *   - information about an illegal instruction,
     *   - or zero when no additional information is available.
     */
    csrr t0, stval
    sd t0, TF_STVAL(sp)

    /*
     * Pass the trap-frame address as the first C argument.
     *
     * According to the RISC-V ABI, a0 holds the first function argument.
     *
     * At this point the interrupted value of a0 has already been saved in the
     * frame, so a0 may safely be overwritten.
     */
    mv a0, sp

    /*
     * Call the high-level trap dispatcher:
     *
     *     void trap_handler(struct trap_frame *frame);
     *
     * The C handler can inspect:
     *
     *     frame->scause
     *     frame->sepc
     *     frame->stval
     *     frame->sstatus
     *
     * It may also modify saved registers. For example:
     *
     *   - place a syscall return value in frame->a0,
     *   - advance frame->sepc after handling ecall,
     *   - alter a task's saved context during a context switch.
     *
     * The C function may overwrite all caller-saved registers. That is safe
     * because the interrupted values are stored in the trap frame.
     */
    call trap_handler

    /*
     * Restore trap-return CSRs.
     *
     * These values are loaded from the frame rather than assumed unchanged,
     * because trap_handler may deliberately modify them.
     */

    /*
     * Restore the PC used by sret.
     *
     * For example, an ecall handler may have changed frame->sepc from the
     * address of ecall to the following instruction.
     */
     ld t0, TF_SEPC(sp)
     csrw sepc, t0

    /*
     * Restore the privilege and interrupt state used by sret.
     */
     ld t0, TF_SSTATUS(sp)
     csrw sstatus, t0

    /*
     * Restore all general-purpose registers except sp.
     *
     * sp must remain the trap-frame base until all other registers have been
     * loaded. Restoring sp too early would make the remaining frame offsets
     * refer to the wrong memory.
     */
     ld ra, TF_RA(sp)
     ld gp, TF_GP(sp)
     ld tp, TF_TP(sp)

     ld t0, TF_T0(sp)
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
     * Release the trap frame.
     *
     * Because the frame was allocated directly below the interrupted stack
     * pointer, adding TRAP_FRAME_SIZE restores sp to exactly the value it had
     * when the trap occurred.
     *
     * This also avoids needing to load TF_SP explicitly.
     *
     * Note that this is correct only if trap_handler does not replace the frame
     * with a different task's frame. A future context-switch implementation may
     * instead set sp to the selected task's trap frame before restoration.
     */
     addi sp, sp, TRAP_FRAME_SIZE

    /*
     * Return from the Supervisor trap.
     *
     * sret performs, conceptually:
     *
     *   privilege <- sstatus.SPP
     *   SIE       <- sstatus.SPIE
     *   SPIE      <- 1
     *   SPP       <- U-mode
     *   pc        <- sepc
     *
     * Execution then continues in the interrupted context.
     */
     sret

.size trap_entry, . - trap_entry
