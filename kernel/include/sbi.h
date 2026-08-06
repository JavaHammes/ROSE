#ifndef SBI_H
#define SBI_H

#include <stdint.h>

struct sbi_ret {
        long error;
        long value;
};

struct sbi_ret sbi_call(unsigned long extension_id, unsigned long function_id,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5);

long sbi_set_timer(uint64_t deadline);
long sbi_shutdown(void);
long sbi_reboot(void);

#endif
