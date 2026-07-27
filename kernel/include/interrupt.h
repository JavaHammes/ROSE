#ifndef INTERRUPT_H
#define INTERRUPT_H

enum {
        /*
         * sstatus.SSIE - Supervisor Interrupt Enable
         *
         * This is the global interrupt-enable bit for supervisor mode.
         *
         * When SIE = 0: No s. interrupts while cpu in s mode.
         * WHen SIE = 1: S interrupts are taken when corresponding inttertups
         * source is also enabled in the sie CSR.
         */
        SSTATUS_SIE = (1UL << 1),

        /*
         * STIE - Supervisor Timer Interrupts Enable
         *
         * An interrupt is only enabled when:
         *  1. its corresponding bit in sie is enabled
         *  2. the global sstatus.SIE bit is enabled
         */
        SIE_STIE = (1UL << 5),

        /*
         * SEIE - Supervisor External Interrupts Enable
         */
        SIE_SEIE = (1UL << 9)
};

void global_interrupts_enable(void);
void global_interrupts_disable(void);

void timer_interrupts_enable(void);
void timer_interrupts_disable(void);

void external_interrupts_enable(void);
void external_interrupts_disable(void);

#endif
