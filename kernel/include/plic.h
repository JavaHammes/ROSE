#ifndef PLIC_H
#define PLIC_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*plic_interrupt_handler)(void);

void plic_init(void);
bool plic_register_handler(uint32_t interrupt_id,
                           plic_interrupt_handler handler);
void plic_dispatch(void);
void plic_enable(uint32_t interrupt_id);
void plic_disable(uint32_t interrupt_id);
void plic_set_priority(uint32_t interrupt_id, uint32_t priority);
uint32_t plic_claim(void);
void plic_complete(uint32_t interrupt_id);

#endif
