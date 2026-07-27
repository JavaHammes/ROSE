#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

enum { TRAP_FRAME_SIZE = 288 };

struct trap_frame {
        uint64_t ra;
        uint64_t sp;
        uint64_t gp;
        uint64_t tp;

        uint64_t t0;
        uint64_t t1;
        uint64_t t2;

        uint64_t s0;
        uint64_t s1;

        uint64_t a0;
        uint64_t a1;
        uint64_t a2;
        uint64_t a3;
        uint64_t a4;
        uint64_t a5;
        uint64_t a6;
        uint64_t a7;

        uint64_t s2;
        uint64_t s3;
        uint64_t s4;
        uint64_t s5;
        uint64_t s6;
        uint64_t s7;
        uint64_t s8;
        uint64_t s9;
        uint64_t s10;
        uint64_t s11;

        uint64_t t3;
        uint64_t t4;
        uint64_t t5;
        uint64_t t6;

        uint64_t sepc;
        uint64_t sstatus;
        uint64_t scause;
        uint64_t stval;

        uint64_t padding;
};

_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
               "trap frame layout mismatch");

enum interrupt_masks {
        /*
         * Bit 63 of scause indicates whether the trap was caused by an
         * interrupt.
         */
        SCAUSE_INTERRUPT_BIT = (UINT64_C(1) << 63),

        /*
         * The remaining bits of scause contain the interrupt or exception code.
         */
        SCAUSE_CODE_MASK = (~SCAUSE_INTERRUPT_BIT),
};

/*
 * Supervisor interrupt cause codes.
 */
enum supervisor_interrupt_code {
        SCAUSE_SUPERVISOR_SOFTWARE = 1,
        SCAUSE_SUPERVISOR_TIMER = 5,
        SCAUSE_SUPERVISOR_EXTERNAL = 9,
};

/*
 * Synchronous exception cause codes.
 */
enum exception_code {
        SCAUSE_INSTRUCTION_ADDRESS_MISALIGNED = 0,
        SCAUSE_INSTRUCTION_ACCESS_FAULT = 1,
        SCAUSE_ILLEGAL_INSTRUCTION = 2,
        SCAUSE_BREAKPOINT = 3,
        SCAUSE_LOAD_ADDRESS_MISALIGNED = 4,
        SCAUSE_LOAD_ACCESS_FAULT = 5,
        SCAUSE_STORE_ADDRESS_MISALIGNED = 6,
        SCAUSE_STORE_ACCESS_FAULT = 7,
        SCAUSE_ECALL_FROM_USER = 8,
        SCAUSE_ECALL_FROM_SUPERVISOR = 9,
        SCAUSE_INSTRUCTION_PAGE_FAULT = 12,
        SCAUSE_LOAD_PAGE_FAULT = 13,
        SCAUSE_STORE_PAGE_FAULT = 15,
};

void trap_init(void);
void trap_handler(struct trap_frame *frame);

#endif
