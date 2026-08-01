/*
 * Bitmap-backed physical page allocator.
 *
 * One bit describes each 4 KiB page in the RAM region selected by the platform
 * parser. Pages below kernel_end, firmware reservations, /reserved-memory
 * regions, and the boot DTB are permanently unavailable. Every successful
 * allocation returns a zero-filled page so page tables and user BSS never
 * expose data left by a previous owner.
 *
 * ROSE is currently single-hart, so allocator operations do not yet require a
 * spinlock. The bookkeeping is nevertheless centralized here so adding a lock
 * later will not change callers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "page_allocator.h"
#include "panic.h"
#include "platform.h"

#define MAX_PHYSICAL_MEMORY_SIZE (UINT64_C(1) << 30U)
#define MAX_PHYSICAL_PAGE_COUNT (MAX_PHYSICAL_MEMORY_SIZE / PAGE_SIZE)
#define RESERVED_PAGE_RANGE_LIMIT 32U

enum { BITMAP_BITS_PER_BYTE = 8 };

#define PAGE_BITMAP_SIZE                                                       \
        ((MAX_PHYSICAL_PAGE_COUNT + BITMAP_BITS_PER_BYTE - 1U) /               \
         BITMAP_BITS_PER_BYTE)

/*
 * This linker symbol points just past the kernel image and boot stack.
 */
extern char kernel_end[];

/*
 * One bit represents one physical page:
 *
 *     0 -> free
 *     1 -> allocated or reserved
 *
 * The bitmap occupies 4096 bytes for 128 MiB of RAM. Because it is in .bss,
 * it is itself located below kernel_end and is automatically reserved.
 */
/* The fixed maximum avoids bootstrapping an allocator for its own bitmap. */
static uint8_t page_bitmap[PAGE_BITMAP_SIZE];
/* Reference counts cover the maximum supported RAM layout. Reserved and free
 * pages retain a zero count; each ordinary allocation starts at one. */
static uint16_t page_reference_counts[MAX_PHYSICAL_PAGE_COUNT];

static size_t first_usable_page;
static size_t usable_page_count;
static size_t free_pages;
static size_t next_free_hint;
static uintptr_t ram_start;
static uintptr_t ram_end;
static size_t physical_page_count;
static bool allocator_initialized;

struct reserved_page_range {
        /* Half-open page-index interval [first_page, end_page). */
        size_t first_page;
        size_t end_page;
};

static struct reserved_page_range
    reserved_page_ranges[RESERVED_PAGE_RANGE_LIMIT];
static size_t reserved_page_range_count;

static uintptr_t align_up(uintptr_t value, uintptr_t alignment) {
        return (value + alignment - 1U) & ~(alignment - 1U);
}

/* Bitmap indices are relative to the discovered beginning of RAM. */
static bool bitmap_page_is_used(size_t page_index) {
        size_t byte_index = page_index / BITMAP_BITS_PER_BYTE;
        uint8_t bit = UINT8_C(1) << (page_index % BITMAP_BITS_PER_BYTE);

        return (page_bitmap[byte_index] & bit) != 0U;
}

static void bitmap_mark_page_used(size_t page_index) {
        size_t byte_index = page_index / BITMAP_BITS_PER_BYTE;
        uint8_t bit = UINT8_C(1) << (page_index % BITMAP_BITS_PER_BYTE);

        page_bitmap[byte_index] |= bit;
}

static void bitmap_mark_page_free(size_t page_index) {
        size_t byte_index = page_index / BITMAP_BITS_PER_BYTE;
        uint8_t bit = UINT8_C(1) << (page_index % BITMAP_BITS_PER_BYTE);

        page_bitmap[byte_index] &= (uint8_t)~bit;
}

static void zero_page(void *page) {
        uint8_t *bytes = page;

        for (size_t index = 0U; index < PAGE_SIZE; index++) {
                bytes[index] = 0U;
        }
}

/*
 * Permanently reserve every RAM page touched by [start, start + size).
 *
 * Device-tree reservations need not be page-aligned and may overlap one
 * another. Clipping keeps non-RAM reservations harmless, while checking the
 * allocation bitmap prevents overlapping reservations from reducing the free
 * count twice.
 */
static void reserve_physical_region(uintptr_t start, size_t size) {
        if (size == 0U || start >= ram_end ||
            (start < ram_start && size <= ram_start - start)) {
                return;
        }

        uintptr_t clipped_start = start < ram_start ? ram_start : start;
        uintptr_t requested_end =
            size > UINTPTR_MAX - start ? UINTPTR_MAX : start + size;
        uintptr_t clipped_end = requested_end > ram_end ? ram_end : requested_end;
        size_t first_page = (size_t)((clipped_start - ram_start) / PAGE_SIZE);
        size_t end_page = (size_t)((align_up(clipped_end, PAGE_SIZE) -
                                    ram_start) /
                                   PAGE_SIZE);

        if (end_page <= first_page) {
                return;
        }
        if (reserved_page_range_count >= RESERVED_PAGE_RANGE_LIMIT) {
                panic("Too many permanent physical-memory reservations");
        }

        reserved_page_ranges[reserved_page_range_count].first_page = first_page;
        reserved_page_ranges[reserved_page_range_count].end_page = end_page;
        reserved_page_range_count++;

        for (size_t page_index = first_page; page_index < end_page;
             page_index++) {
                if (page_index < first_usable_page ||
                    bitmap_page_is_used(page_index)) {
                        continue;
                }

                bitmap_mark_page_used(page_index);
                usable_page_count--;
                free_pages--;
        }
}

/* Permanent ranges are checked separately because their bitmap bits otherwise
 * look exactly like ordinary allocated pages. */
static bool page_is_permanently_reserved(size_t page_index) {
        for (size_t index = 0U; index < reserved_page_range_count; index++) {
                if (page_index >= reserved_page_ranges[index].first_page &&
                    page_index < reserved_page_ranges[index].end_page) {
                        return true;
                }
        }

        return false;
}

/* Validate an exact allocated, non-reserved page and return its bitmap index. */
static size_t allocated_page_index(const void *page) {
        uintptr_t address = (uintptr_t)page;

        if (!allocator_initialized) {
                panic("Physical page allocator used before initialization");
        }
        if ((address & (PAGE_SIZE - 1U)) != 0U) {
                panic("Attempted to reference an unaligned physical page");
        }
        if (address < ram_start || address >= ram_end) {
                panic("Attempted to reference a page outside physical RAM");
        }

        size_t page_index = (size_t)((address - ram_start) / PAGE_SIZE);

        if (page_index < first_usable_page ||
            page_is_permanently_reserved(page_index)) {
                panic("Attempted to reference a reserved physical page");
        }
        if (!bitmap_page_is_used(page_index) ||
            page_reference_counts[page_index] == 0U) {
                panic("Attempted to reference a free physical page");
        }

        return page_index;
}

/*
 * Initialize the bitmap after platform discovery but before virtual memory.
 *
 * The bitmap begins completely reserved. Only pages at or above the aligned
 * kernel end are made free, after which platform reservations are applied.
 * This naturally protects OpenSBI and the gap between firmware and the kernel.
 */
void page_allocator_init(void) {
        if (allocator_initialized) {
                panic("Physical page allocator initialized twice");
        }

        ram_start = platform_ram_start();
        ram_end = platform_ram_end();

        if (ram_end <= ram_start ||
            ram_end - ram_start > MAX_PHYSICAL_MEMORY_SIZE ||
            ((ram_start | ram_end) & (PAGE_SIZE - 1U)) != 0U) {
                panic("Unsupported physical RAM layout");
        }

        physical_page_count = (size_t)((ram_end - ram_start) / PAGE_SIZE);

        /*
         * Begin with every page reserved. This includes OpenSBI and the gap
         * between the firmware and the kernel.
         */
        for (size_t index = 0U; index < PAGE_BITMAP_SIZE; index++) {
                page_bitmap[index] = UINT8_MAX;
        }
        for (size_t index = 0U; index < MAX_PHYSICAL_PAGE_COUNT; index++) {
                page_reference_counts[index] = 0U;
        }

        uintptr_t first_free_address =
            align_up((uintptr_t)kernel_end, PAGE_SIZE);

        if (first_free_address < ram_start || first_free_address > ram_end) {
                panic("Kernel image lies outside configured physical RAM");
        }

        first_usable_page =
            (size_t)((first_free_address - ram_start) / PAGE_SIZE);
        usable_page_count = physical_page_count - first_usable_page;
        free_pages = usable_page_count;
        next_free_hint = first_usable_page;

        for (size_t page_index = first_usable_page;
             page_index < physical_page_count; page_index++) {
                bitmap_mark_page_free(page_index);
        }

        for (size_t index = 0U; index < platform_reserved_region_count();
             index++) {
                uintptr_t start;
                size_t size;

                if (!platform_reserved_region(index, &start, &size)) {
                        panic("Invalid platform memory reservation");
                }
                reserve_physical_region(start, size);
        }

        allocator_initialized = true;
}

/*
 * Return the next free page at or after next_free_hint. A freed lower page moves
 * the hint backward, so holes are reused without scanning permanently reserved
 * low memory on every allocation.
 */
void *page_alloc(void) {
        if (!allocator_initialized) {
                panic("Physical page allocator used before initialization");
        }

        if (free_pages == 0U) {
                return NULL;
        }

        for (size_t page_index = next_free_hint;
             page_index < physical_page_count; page_index++) {
                if (bitmap_page_is_used(page_index)) {
                        continue;
                }

                bitmap_mark_page_used(page_index);
                page_reference_counts[page_index] = 1U;
                free_pages--;
                next_free_hint = page_index + 1U;

                void *page =
                    (void *)(uintptr_t)(ram_start + (page_index * PAGE_SIZE));
                zero_page(page);
                return page;
        }

        /*
         * A free-page count greater than zero must always correspond to a
         * clear bit at or after next_free_hint.
         */
        panic("Physical page allocator bitmap is inconsistent");
}

/* Retain or release an owned page. Invalid ownership and count misuse are fatal
 * because accepting either would corrupt allocator bookkeeping. */
void page_retain(void *page) {
        size_t page_index = allocated_page_index(page);

        if (page_reference_counts[page_index] == UINT16_MAX) {
                panic("Physical page reference count overflow");
        }
        page_reference_counts[page_index]++;
}

void page_release(void *page) {
        size_t page_index = allocated_page_index(page);

        page_reference_counts[page_index]--;
        if (page_reference_counts[page_index] != 0U) {
                return;
        }

        bitmap_mark_page_free(page_index);
        free_pages++;

        if (page_index < next_free_hint) {
                next_free_hint = page_index;
        }
}

size_t page_reference_count(const void *page) {
        size_t page_index = allocated_page_index(page);

        return page_reference_counts[page_index];
}

size_t page_total_count(void) {
        if (!allocator_initialized) {
                return 0U;
        }

        return usable_page_count;
}

size_t page_free_count(void) {
        if (!allocator_initialized) {
                return 0U;
        }

        return free_pages;
}

size_t page_used_count(void) {
        if (!allocator_initialized) {
                return 0U;
        }

        return usable_page_count - free_pages;
}

void page_allocator_self_test(void) {
        /* Verify uniqueness, alignment, zero-on-allocation, deterministic reuse,
         * and exact free-page accounting without retaining any test pages. */
        size_t initial_free_pages = page_free_count();
        void *first = page_alloc();
        void *second = page_alloc();

        if (first == NULL || second == NULL || first == second) {
                panic("Physical page allocator self-test allocation failed");
        }

        if (((uintptr_t)first & (PAGE_SIZE - 1U)) != 0U ||
            ((uintptr_t)second & (PAGE_SIZE - 1U)) != 0U) {
                panic("Physical page allocator returned an unaligned page");
        }

        uint8_t *first_bytes = first;

        for (size_t index = 0U; index < PAGE_SIZE; index++) {
                if (first_bytes[index] != 0U) {
                        panic("Physical page allocator returned a dirty page");
                }
        }

        first_bytes[0] = UINT8_C(0xa5);
        page_retain(first);
        page_release(first);
        if (page_reference_count(first) != 1U) {
                panic("Physical page allocator retain self-test failed");
        }
        page_release(first);

        void *reused = page_alloc();

        if (reused != first || ((uint8_t *)reused)[0] != 0U) {
                panic("Physical page allocator reuse self-test failed");
        }

        page_release(reused);
        page_release(second);

        if (page_free_count() != initial_free_pages) {
                panic("Physical page allocator leaked pages during self-test");
        }
}
