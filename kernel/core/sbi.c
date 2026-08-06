#include <stdint.h>

#include "sbi.h"

/*
 * SBI extension ID for the Timer extension.
 * SBI extensions are identified through values placed in register a7
 * before executing an ecall.
 *
 * 0x54494D45 is the ASCII encoding of "TIME"
 */
#define SBI_EXT_TIME 0x54494D45UL

/*
 * Function ID for set_timer() inside the SBI Timer extension.
 *
 * An SBI extension may expose multiple functions. Register a6
 * selects the function within the extension specified in a7.
 */
#define SBI_FID_SET_TIMER 0UL

/*
 * SBI System Reset extension ("SRST") and its system_reset function.
 */
#define SBI_EXT_SYSTEM_RESET 0x53525354UL
#define SBI_FID_SYSTEM_RESET 0UL

/*
 * Reset type zero requests shutdown and type one requests a cold reboot. A
 * reset reason of zero means that no specific failure caused the reset.
 */
#define SBI_RESET_TYPE_SHUTDOWN 0UL
#define SBI_RESET_TYPE_COLD_REBOOT 1UL
#define SBI_RESET_REASON_NONE 0UL

/*
 * Perform generic SBI call.
 *
 * An SBI call is made by placing arguments and identifiers in the arg
 * registers and then executing ecall:
 *
 * - a0-a5: function arguments
 * - a6: SBI function ID
 * - a7: SBI extension ID
 *
 * OpenSBI handles the requested operation and then returns to this function.
 */
struct sbi_ret sbi_call(unsigned long extension_id, unsigned long function_id,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5) {

        register unsigned long a0 __asm__("a0") = arg0;
        register unsigned long a1 __asm__("a1") = arg1;
        register unsigned long a2 __asm__("a2") = arg2;
        register unsigned long a3 __asm__("a3") = arg3;
        register unsigned long a4 __asm__("a4") = arg4;
        register unsigned long a5 __asm__("a5") = arg5;
        register unsigned long a6 __asm__("a6") = function_id;
        register unsigned long a7 __asm__("a7") = extension_id;

        /*
         * Execute the SBI call.
         *
         * Note: - "+r"(a0) the "+" means that the register is read before
         *  		ecall and written afterward.
         *  	 - "memory" prevents the compiler from moving memory access
         *  	 	across the SBI call.
         */
        __asm__ volatile("ecall"
                         : "+r"(a0), "+r"(a1)
                         : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                         : "memory");

        return (struct sbi_ret){
            .error = (long)a0,
            .value = (long)a1,
        };
}

/*
 * Schedule next timer interrupt.
 *
 * a0 = deadline
 * a1-a5 = 0
 * a6 = SBI_FID_SET_TIMER
 * a7 = SBI_EXT_TIME
 *
 * OpenSBI programs the machine-level timer on behalf of the S-mode kernel.
 * When the timer counter reaches deadline, a supervisor timer interrupt
 * becomes pending.
 */
long sbi_set_timer(uint64_t deadline) {
        struct sbi_ret result =
            sbi_call(SBI_EXT_TIME, SBI_FID_SET_TIMER, (unsigned long)deadline,
                     0, 0, 0, 0, 0);

        return result.error;
}

/*
 * Ask the SBI firmware to shut down the machine.
 *
 * A successful request never returns. If the extension is unavailable or the
 * request fails, return the SBI error code to the caller.
 */
long sbi_shutdown(void) {
        struct sbi_ret result = sbi_call(
            SBI_EXT_SYSTEM_RESET, SBI_FID_SYSTEM_RESET, SBI_RESET_TYPE_SHUTDOWN,
            SBI_RESET_REASON_NONE, 0, 0, 0, 0);

        return result.error;
}

/* Restart all harts and platform components through the same SRST extension. */
long sbi_reboot(void) {
        struct sbi_ret result = sbi_call(
            SBI_EXT_SYSTEM_RESET, SBI_FID_SYSTEM_RESET,
            SBI_RESET_TYPE_COLD_REBOOT, SBI_RESET_REASON_NONE, 0, 0, 0, 0);

        return result.error;
}
