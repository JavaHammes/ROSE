#ifndef PAGE_ALLOCATOR_H
#define PAGE_ALLOCATOR_H

#include <stddef.h>

/* Base-page size used by the allocator and Sv39 level-zero mappings. */
#define PAGE_SIZE 4096UL

/* Initialize from the platform RAM map; the self-test is a separate boot step. */
void page_allocator_init(void);
void page_allocator_self_test(void);

/* Allocated pages begin with one reference. Retain/release permit user pages to
 * be shared by copy-on-write address spaces without changing page-table
 * ownership rules. */
void *page_alloc(void);
void page_retain(void *page);
void page_release(void *page);
size_t page_reference_count(const void *page);

/* Counts cover allocatable RAM after kernel and firmware reservations. */
size_t page_total_count(void);
size_t page_free_count(void);
size_t page_used_count(void);

#endif
