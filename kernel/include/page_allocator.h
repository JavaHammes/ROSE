#ifndef PAGE_ALLOCATOR_H
#define PAGE_ALLOCATOR_H

#include <stddef.h>

/* Base-page size used by the allocator and Sv39 level-zero mappings. */
#define PAGE_SIZE 4096UL

/* Initialize from the platform RAM map; the self-test is a separate boot step. */
void page_allocator_init(void);
void page_allocator_self_test(void);

/* Allocate a zero-filled page or return NULL. Freeing requires exact ownership. */
void *page_alloc(void);
void page_free(void *page);

/* Counts cover allocatable RAM after kernel and firmware reservations. */
size_t page_total_count(void);
size_t page_free_count(void);
size_t page_used_count(void);

#endif
