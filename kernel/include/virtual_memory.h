#ifndef VIRTUAL_MEMORY_H
#define VIRTUAL_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct page_table;

/* Architecture-independent permissions accepted by the Sv39 implementation. */
enum virtual_memory_page_flags {
        VM_PAGE_READ = (1U << 0),
        VM_PAGE_WRITE = (1U << 1),
        VM_PAGE_EXECUTE = (1U << 2),
        VM_PAGE_USER = (1U << 3),
        VM_PAGE_GLOBAL = (1U << 4),
};

/* Page tables own hierarchy pages, not physical pages referenced by leaves. */
struct page_table *page_table_create(void);
void page_table_destroy(struct page_table *root);

/* Mapping never replaces an existing leaf. Use protect to change permissions. */
bool page_table_map(struct page_table *root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags);
bool page_table_map_range(struct page_table *root, uintptr_t virtual_address,
                          uintptr_t physical_address, size_t size,
                          uint64_t flags);
bool page_table_protect(struct page_table *root, uintptr_t virtual_address,
                        uint64_t flags);
bool page_table_unmap(struct page_table *root, uintptr_t virtual_address);

/* Software translation can also report the encountered leaf's public flags. */
bool page_table_translate(const struct page_table *root,
                          uintptr_t virtual_address,
                          uintptr_t *physical_address, uint64_t *flags);

void page_table_activate(struct page_table *root);
struct page_table *page_table_current(void);

/* New process roots contain compact supervisor-only kernel and MMIO mappings. */
struct page_table *virtual_memory_create_address_space(void);
struct page_table *virtual_memory_kernel_page_table(void);

void virtual_memory_init(void);
bool virtual_memory_is_enabled(void);

#endif
