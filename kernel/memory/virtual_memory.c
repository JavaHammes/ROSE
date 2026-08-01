/*
 * Sv39 virtual-memory management.
 *
 * The kernel currently uses an identity map: a mapped physical address has the
 * same virtual address. Each user process receives a private root page table
 * containing supervisor-only kernel/MMIO mappings plus its own user pages.
 *
 * Page-table pages are owned by the page-table hierarchy. Leaf mappings only
 * refer to physical pages; callers such as the ELF loader remain responsible
 * for freeing those pages. This ownership split is important during process
 * teardown and is enforced by page_table_destroy and page_table_unmap.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "page_allocator.h"
#include "panic.h"
#include "platform.h"
#include "virtual_memory.h"

/*
 * Sv39 uses three levels of 512-entry page tables. Each entry is eight bytes,
 * so one complete page table occupies exactly one 4 KiB physical page.
 */
enum {
        SV39_PAGE_TABLE_ENTRIES = 512,
        SV39_LEVELS = 3,
        SV39_VPN_BITS = 9,
        SV39_VPN_MASK = 0x1ff,
        SV39_PAGE_SHIFT = 12,
        SV39_LARGE_PAGE_SHIFT = 21,
        PTE_PPN_SHIFT = 10,
        PTE_PPN_BITS = 44,
        SATP_MODE_SHIFT = 60,
        SATP_MODE_SV39 = 8,
        SV39_ADDRESS_BITS = 39,
        SV39_SIGN_BIT = 38,
};

#define SV39_LARGE_PAGE_SIZE (UINT64_C(1) << SV39_LARGE_PAGE_SHIFT)

enum page_table_entry_bits {
        PTE_VALID = (UINT64_C(1) << 0),
        PTE_READ = (UINT64_C(1) << 1),
        PTE_WRITE = (UINT64_C(1) << 2),
        PTE_EXECUTE = (UINT64_C(1) << 3),
        PTE_USER = (UINT64_C(1) << 4),
        PTE_GLOBAL = (UINT64_C(1) << 5),
        PTE_ACCESSED = (UINT64_C(1) << 6),
        PTE_DIRTY = (UINT64_C(1) << 7),
        /* Reserved-for-software bit used to distinguish intentional COW write
         * faults from genuine protection violations. */
        PTE_COPY_ON_WRITE = (UINT64_C(1) << 8),
};

#define PTE_LEAF_BITS (PTE_READ | PTE_WRITE | PTE_EXECUTE)
#define PTE_PPN_MASK                                                           \
        (((UINT64_C(1) << PTE_PPN_BITS) - UINT64_C(1)) << PTE_PPN_SHIFT)

#define SATP_PPN_MASK ((UINT64_C(1) << PTE_PPN_BITS) - UINT64_C(1))

struct page_table {
        uint64_t entries[SV39_PAGE_TABLE_ENTRIES];
};

_Static_assert(sizeof(struct page_table) == PAGE_SIZE,
               "Sv39 page table must occupy exactly one page");

extern char text_start[];
extern char text_end[];
extern char rodata_start[];
extern char rodata_end[];
extern char user_program_elf_start[];
extern char user_program_elf_end[];
extern char data_start[];
extern char data_end[];
extern char bss_start[];
extern char bss_end[];
extern char stack_bottom[];
extern char stack_top[];
extern char kernel_start[];
extern char kernel_end[];

static struct page_table *active_page_table;
static struct page_table *kernel_page_table;

/*
 * Sv39 accepts only canonical virtual addresses: bits 63:39 must all equal bit
 * 38. Rejecting non-canonical addresses here avoids hardware-dependent faults
 * later during a page-table walk.
 */
static bool sv39_virtual_address_is_valid(uintptr_t address) {
        uint64_t upper_bits = (uint64_t)address >> SV39_ADDRESS_BITS;
        bool sign_bit =
            ((uint64_t)address & (UINT64_C(1) << SV39_SIGN_BIT)) != 0U;

        if (!sign_bit) {
                return upper_bits == 0U;
        }

        return upper_bits == (UINT64_MAX >> SV39_ADDRESS_BITS);
}

static bool physical_address_is_valid(uintptr_t address) {
        /* A Sv39 PTE carries a 44-bit physical page number. */
        return ((uint64_t)address >> (PTE_PPN_BITS + SV39_PAGE_SHIFT)) == 0U;
}

static size_t virtual_page_number(uintptr_t virtual_address, size_t level) {
        /* level 0 selects bits 20:12, level 1 bits 29:21, and level 2 bits
         * 38:30. */
        size_t shift = SV39_PAGE_SHIFT + (level * SV39_VPN_BITS);

        return (size_t)((virtual_address >> shift) & SV39_VPN_MASK);
}

static bool page_table_entry_is_leaf(uint64_t entry) {
        /* A valid PTE with any R/W/X bit is a leaf; otherwise it points to the
         * next page-table level. */
        return (entry & PTE_LEAF_BITS) != 0U;
}

static uintptr_t page_table_entry_physical_address(uint64_t entry) {
        return (uintptr_t)(((entry & PTE_PPN_MASK) >> PTE_PPN_SHIFT)
                           << SV39_PAGE_SHIFT);
}

static uint64_t page_table_entry_from_physical_address(uintptr_t address) {
        return (((uint64_t)address >> SV39_PAGE_SHIFT) << PTE_PPN_SHIFT) &
               PTE_PPN_MASK;
}

static uint64_t page_table_entry_flags(uint64_t flags) {
        /* Set A/D eagerly. This avoids depending on whether the emulated CPU
         * updates them automatically or raises a page fault. */
        uint64_t entry_flags = PTE_ACCESSED;

        if ((flags & VM_PAGE_READ) != 0U) {
                entry_flags |= PTE_READ;
        }
        if ((flags & VM_PAGE_WRITE) != 0U) {
                entry_flags |= PTE_WRITE | PTE_DIRTY;
        }
        if ((flags & VM_PAGE_EXECUTE) != 0U) {
                entry_flags |= PTE_EXECUTE;
        }
        if ((flags & VM_PAGE_USER) != 0U) {
                entry_flags |= PTE_USER;
        }
        if ((flags & VM_PAGE_GLOBAL) != 0U) {
                entry_flags |= PTE_GLOBAL;
        }
        if ((flags & VM_PAGE_COPY_ON_WRITE) != 0U) {
                entry_flags |= PTE_COPY_ON_WRITE;
        }

        return entry_flags;
}

static uint64_t virtual_memory_flags(uint64_t entry) {
        /* Convert hardware bits back into the stable flags exposed by the
         * virtual-memory API. */
        uint64_t flags = 0U;

        if ((entry & PTE_READ) != 0U) {
                flags |= VM_PAGE_READ;
        }
        if ((entry & PTE_WRITE) != 0U) {
                flags |= VM_PAGE_WRITE;
        }
        if ((entry & PTE_EXECUTE) != 0U) {
                flags |= VM_PAGE_EXECUTE;
        }
        if ((entry & PTE_USER) != 0U) {
                flags |= VM_PAGE_USER;
        }
        if ((entry & PTE_GLOBAL) != 0U) {
                flags |= VM_PAGE_GLOBAL;
        }
        if ((entry & PTE_COPY_ON_WRITE) != 0U) {
                flags |= VM_PAGE_COPY_ON_WRITE;
        }

        return flags;
}

static void invalidate_virtual_address(uintptr_t virtual_address) {
        /* Page-table memory can change while this root is active. Flush any
         * cached translation for the affected virtual address. */
        __asm__ volatile("sfence.vma %[address], zero"
                         :
                         : [address] "r"(virtual_address)
                         : "memory");
}

struct page_table *page_table_create(void) {
        /* page_alloc returns a zero-filled page, which is also an empty table. */
        return (struct page_table *)page_alloc();
}

static bool page_table_is_empty(const struct page_table *table) {
        for (size_t index = 0U; index < SV39_PAGE_TABLE_ENTRIES; index++) {
                if ((table->entries[index] & PTE_VALID) != 0U) {
                        return false;
                }
        }

        return true;
}

static bool page_flags_are_valid(uint64_t flags) {
        const uint64_t valid_flags = VM_PAGE_READ | VM_PAGE_WRITE |
                                     VM_PAGE_EXECUTE | VM_PAGE_USER |
                                     VM_PAGE_GLOBAL | VM_PAGE_COPY_ON_WRITE;

        /* RISC-V reserves the W=1,R=0 PTE encoding. A leaf must also grant at
         * least read or execute permission. */
        bool copy_on_write = (flags & VM_PAGE_COPY_ON_WRITE) != 0U;
        bool copy_on_write_flags_are_valid =
            !copy_on_write ||
            ((flags & (VM_PAGE_USER | VM_PAGE_READ)) ==
                 (VM_PAGE_USER | VM_PAGE_READ) &&
             (flags &
              (VM_PAGE_WRITE | VM_PAGE_EXECUTE | VM_PAGE_GLOBAL)) == 0U);

        return (flags & ~valid_flags) == 0U &&
               (flags & (VM_PAGE_READ | VM_PAGE_EXECUTE)) != 0U &&
               ((flags & VM_PAGE_WRITE) == 0U ||
                (flags & VM_PAGE_READ) != 0U) &&
               copy_on_write_flags_are_valid;
}

static bool page_table_map_at_level(struct page_table *root,
                                    uintptr_t virtual_address,
                                    uintptr_t physical_address,
                                    uint64_t flags, size_t target_level) {
        uintptr_t mapping_size =
            (uintptr_t)1U
            << (SV39_PAGE_SHIFT + target_level * SV39_VPN_BITS);

        if (root == NULL || target_level >= SV39_LEVELS ||
            (virtual_address & (mapping_size - 1U)) != 0U ||
            (physical_address & (mapping_size - 1U)) != 0U ||
            !sv39_virtual_address_is_valid(virtual_address) ||
            !physical_address_is_valid(physical_address) ||
            !page_flags_are_valid(flags)) {
                return false;
        }

        /* Track tables created by this operation. If allocation or validation
         * fails later, only these new tables may be detached and freed. */
        struct page_table *table = root;
        uint64_t *created_parent_entries[SV39_LEVELS - 1U] = {0};
        struct page_table *created_tables[SV39_LEVELS - 1U] = {0};
        size_t created_count = 0U;

        /* Walk from the root toward the requested leaf level, allocating
         * missing intermediate tables. A leaf encountered too early covers a
         * larger range and cannot be descended through. */
        for (size_t level = SV39_LEVELS - 1U; level > target_level; level--) {
                uint64_t *entry =
                    &table->entries[virtual_page_number(virtual_address, level)];

                if ((*entry & PTE_VALID) != 0U) {
                        if (page_table_entry_is_leaf(*entry)) {
                                goto fail;
                        }

                        table = (struct page_table *)
                            page_table_entry_physical_address(*entry);
                        continue;
                }

                struct page_table *next_table = page_table_create();

                if (next_table == NULL) {
                        goto fail;
                }

                *entry = page_table_entry_from_physical_address(
                             (uintptr_t)next_table) |
                         PTE_VALID;
                created_parent_entries[created_count] = entry;
                created_tables[created_count] = next_table;
                created_count++;
                table = next_table;
        }

        uint64_t *leaf = &table->entries[virtual_page_number(virtual_address,
                                                             target_level)];

        /* Mapping an occupied address is a caller error, never an implicit
         * permission or physical-address replacement. */
        if ((*leaf & PTE_VALID) != 0U) {
                goto fail;
        }

        *leaf = page_table_entry_from_physical_address(physical_address) |
                page_table_entry_flags(flags) | PTE_VALID;

        if (root == active_page_table) {
                invalidate_virtual_address(virtual_address);
        }

        return true;

fail:
        /* Roll back in reverse creation order so no parent retains a pointer to
         * a page after that page has been freed. */
        while (created_count != 0U) {
                created_count--;
                *created_parent_entries[created_count] = 0U;
                page_release(created_tables[created_count]);
        }

        return false;
}

void page_table_destroy(struct page_table *root) {
        if (root == NULL) {
                return;
        }

        if (root == active_page_table || root == kernel_page_table) {
                panic("Attempted to destroy an active or kernel page table");
        }

        /*
         * Leaf mappings do not own their mapped physical pages. Free only the
         * three-level Sv39 hierarchy itself, from the lowest level upward.
         */
        for (size_t root_index = 0U; root_index < SV39_PAGE_TABLE_ENTRIES;
             root_index++) {
                uint64_t root_entry = root->entries[root_index];

                if ((root_entry & PTE_VALID) == 0U ||
                    page_table_entry_is_leaf(root_entry)) {
                        continue;
                }

                struct page_table *level_one =
                    (struct page_table *)page_table_entry_physical_address(
                        root_entry);

                for (size_t level_one_index = 0U;
                     level_one_index < SV39_PAGE_TABLE_ENTRIES;
                     level_one_index++) {
                        uint64_t level_one_entry =
                            level_one->entries[level_one_index];

                        if ((level_one_entry & PTE_VALID) == 0U ||
                            page_table_entry_is_leaf(level_one_entry)) {
                                continue;
                        }

                        void *level_zero =
                            (void *)page_table_entry_physical_address(
                                level_one_entry);
                        page_release(level_zero);
                }

                page_release(level_one);
        }

        page_release(root);
}

bool page_table_map(struct page_table *root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags) {
        return page_table_map_at_level(root, virtual_address, physical_address,
                                       flags, 0U);
}

/*
 * Map a contiguous sequence of 4 KiB pages atomically from the caller's point
 * of view. If any page collides or allocation fails, remove every earlier page
 * installed by this call. page_table_unmap also reclaims now-empty tables.
 */
bool page_table_map_range(struct page_table *root, uintptr_t virtual_address,
                          uintptr_t physical_address, size_t size,
                          uint64_t flags) {
        if ((size & (PAGE_SIZE - 1U)) != 0U ||
            size > UINTPTR_MAX - virtual_address ||
            size > UINTPTR_MAX - physical_address) {
                return false;
        }

        for (size_t offset = 0U; offset < size; offset += PAGE_SIZE) {
                if (!page_table_map(root, virtual_address + offset,
                                    physical_address + offset, flags)) {
                        while (offset != 0U) {
                                offset -= PAGE_SIZE;
                                if (!page_table_unmap(root,
                                                      virtual_address + offset)) {
                                        panic("Sv39 map rollback failed");
                                }
                        }
                        return false;
                }
        }

        return true;
}

/*
 * Change permissions on one existing 4 KiB leaf without changing its physical
 * page. A request inside a large leaf is rejected because altering the entire
 * 2 MiB mapping would violate the function's one-page contract.
 */
bool page_table_protect(struct page_table *root, uintptr_t virtual_address,
                        uint64_t flags) {
        if (root == NULL || (virtual_address & (PAGE_SIZE - 1U)) != 0U ||
            !sv39_virtual_address_is_valid(virtual_address) ||
            !page_flags_are_valid(flags)) {
                return false;
        }

        struct page_table *table = root;

        for (size_t level = SV39_LEVELS; level-- > 0U;) {
                uint64_t *entry =
                    &table->entries[virtual_page_number(virtual_address, level)];

                if ((*entry & PTE_VALID) == 0U) {
                        return false;
                }
                if (page_table_entry_is_leaf(*entry)) {
                        /* A 4 KiB protection change must not silently alter an
                         * entire 2 MiB or 1 GiB mapping. */
                        if (level != 0U) {
                                return false;
                        }

                        uintptr_t physical_address =
                            page_table_entry_physical_address(*entry);
                        *entry = page_table_entry_from_physical_address(
                                     physical_address) |
                                 page_table_entry_flags(flags) | PTE_VALID;

                        if (root == active_page_table) {
                                invalidate_virtual_address(virtual_address);
                        }
                        return true;
                }

                if (level == 0U) {
                        return false;
                }

                table = (struct page_table *)
                    page_table_entry_physical_address(*entry);
        }

        return false;
}

/* Replace one level-zero leaf without dropping its page-table hierarchy. This
 * is the atomic mapping operation needed by a copy-on-write fault after its
 * private physical page has already been allocated. */
bool page_table_replace(struct page_table *root, uintptr_t virtual_address,
                        uintptr_t physical_address, uint64_t flags) {
        if (root == NULL || (virtual_address & (PAGE_SIZE - 1U)) != 0U ||
            (physical_address & (PAGE_SIZE - 1U)) != 0U ||
            !sv39_virtual_address_is_valid(virtual_address) ||
            !physical_address_is_valid(physical_address) ||
            !page_flags_are_valid(flags)) {
                return false;
        }

        struct page_table *table = root;

        for (size_t level = SV39_LEVELS; level-- > 0U;) {
                size_t index = virtual_page_number(virtual_address, level);
                uint64_t *entry = &table->entries[index];

                if ((*entry & PTE_VALID) == 0U) {
                        return false;
                }
                if (page_table_entry_is_leaf(*entry)) {
                        if (level != 0U) {
                                return false;
                        }

                        *entry = page_table_entry_from_physical_address(
                                     physical_address) |
                                 page_table_entry_flags(flags) | PTE_VALID;
                        if (root == active_page_table) {
                                invalidate_virtual_address(virtual_address);
                        }
                        return true;
                }
                if (level == 0U) {
                        return false;
                }

                table = (struct page_table *)page_table_entry_physical_address(
                    *entry);
        }

        return false;
}

/*
 * Remove one 4 KiB leaf and reclaim empty lower-level tables on the way back to
 * the root. Large leaves are deliberately not split implicitly.
 */
bool page_table_unmap(struct page_table *root, uintptr_t virtual_address) {
        if (root == NULL || (virtual_address & (PAGE_SIZE - 1U)) != 0U ||
            !sv39_virtual_address_is_valid(virtual_address)) {
                return false;
        }

        struct page_table *tables[SV39_LEVELS] = {0};
        uint64_t *parent_entries[SV39_LEVELS - 1U] = {0};
        struct page_table *table = root;
        tables[SV39_LEVELS - 1U] = root;

        for (size_t level = SV39_LEVELS; level-- > 0U;) {
                uint64_t *entry =
                    &table->entries[virtual_page_number(virtual_address, level)];

                if ((*entry & PTE_VALID) == 0U) {
                        return false;
                }
                if (page_table_entry_is_leaf(*entry)) {
                        if (level != 0U) {
                                return false;
                        }
                        *entry = 0U;
                        break;
                }
                if (level == 0U) {
                        return false;
                }

                parent_entries[level - 1U] = entry;
                table = (struct page_table *)
                    page_table_entry_physical_address(*entry);
                tables[level - 1U] = table;
        }

        /* The arrays are indexed bottom-up here: level zero is the table which
         * held the removed leaf, followed by its parent. */
        for (size_t level = 0U; level < SV39_LEVELS - 1U; level++) {
                if (!page_table_is_empty(tables[level])) {
                        break;
                }

                *parent_entries[level] = 0U;
                page_release(tables[level]);
        }

        if (root == active_page_table) {
                invalidate_virtual_address(virtual_address);
        }

        return true;
}

/*
 * Perform a software page-table walk. A leaf may appear at any Sv39 level, so
 * the offset mask is derived from the level rather than fixed at 4 KiB.
 */
bool page_table_translate(const struct page_table *root,
                          uintptr_t virtual_address,
                          uintptr_t *physical_address, uint64_t *flags) {
        if (root == NULL || physical_address == NULL ||
            !sv39_virtual_address_is_valid(virtual_address)) {
                return false;
        }

        const struct page_table *table = root;

        for (size_t level = SV39_LEVELS; level-- > 0U;) {
                size_t index = virtual_page_number(virtual_address, level);
                uint64_t entry = table->entries[index];

                if ((entry & PTE_VALID) == 0U) {
                        return false;
                }

                if (page_table_entry_is_leaf(entry)) {
                        size_t page_shift =
                            SV39_PAGE_SHIFT + (level * SV39_VPN_BITS);
                        uintptr_t page_offset_mask =
                            ((uintptr_t)1U << page_shift) - 1U;
                        uintptr_t page_base =
                            page_table_entry_physical_address(entry) &
                            ~page_offset_mask;

                        *physical_address =
                            page_base | (virtual_address & page_offset_mask);

                        if (flags != NULL) {
                                *flags = virtual_memory_flags(entry);
                        }

                        return true;
                }

                if (level == 0U) {
                        return false;
                }

                table = (const struct page_table *)
                    page_table_entry_physical_address(entry);
        }

        return false;
}

/* Install a root table in satp and discard translations from the prior root. */
void page_table_activate(struct page_table *root) {
        if (root == NULL || ((uintptr_t)root & (PAGE_SIZE - 1U)) != 0U) {
                panic("Attempted to activate an invalid page table");
        }

        uint64_t root_page_number =
            (uint64_t)(uintptr_t)root >> SV39_PAGE_SHIFT;
        uint64_t satp = ((uint64_t)SATP_MODE_SV39 << SATP_MODE_SHIFT) |
                        (root_page_number & SATP_PPN_MASK);

        active_page_table = root;

        /* Flush before and after satp. The first fence orders earlier page-table
         * writes; the second prevents stale translations from the old root. */
        __asm__ volatile("sfence.vma zero, zero\n"
                         "csrw satp, %[satp]\n"
                         "sfence.vma zero, zero"
                         :
                         : [satp] "r"(satp)
                         : "memory");
}

struct page_table *page_table_current(void) { return active_page_table; }

struct page_table *virtual_memory_kernel_page_table(void) {
        return kernel_page_table;
}

static uintptr_t align_down(uintptr_t value) {
        return value & ~(PAGE_SIZE - 1U);
}

static uintptr_t align_up(uintptr_t value) {
        return (value + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
}

static bool map_identity_range_mixed(struct page_table *root, uintptr_t start,
                                     uintptr_t end, uint64_t flags) {
        if (end <= start) {
                return true;
        }

        uintptr_t address = align_down(start);
        uintptr_t aligned_end = align_up(end);

        /* Prefer one level-1 leaf for each aligned 2 MiB span. Unaligned edges
         * fall back to 4 KiB leaves. */
        while (address < aligned_end) {
                size_t level = 0U;
                uintptr_t mapping_size = PAGE_SIZE;

                if ((address & (SV39_LARGE_PAGE_SIZE - 1U)) == 0U &&
                    aligned_end - address >= SV39_LARGE_PAGE_SIZE) {
                        level = 1U;
                        mapping_size = SV39_LARGE_PAGE_SIZE;
                }

                if (!page_table_map_at_level(root, address, address, flags,
                                             level)) {
                        return false;
                }

                address += mapping_size;
        }

        return true;
}

static bool map_identity_range(struct page_table *root, uintptr_t start,
                               uintptr_t end, uint64_t flags) {
        if (end <= start) {
                return true;
        }

        uintptr_t page_start = align_down(start);
        uintptr_t page_end = align_up(end);

        return page_table_map_range(root, page_start, page_start,
                                    page_end - page_start, flags);
}

static bool protect_identity_range(struct page_table *root, uintptr_t start,
                                   uintptr_t end, uint64_t flags) {
        if (end <= start) {
                return true;
        }

        uintptr_t page_start = align_down(start);
        uintptr_t page_end = align_up(end);

        for (uintptr_t address = page_start; address < page_end;
             address += PAGE_SIZE) {
                if (!page_table_protect(root, address, flags)) {
                        return false;
                }
        }

        return true;
}

/*
 * Build the supervisor half of an address space.
 *
 * RAM outside the kernel image uses large mappings to keep each address space
 * small. The image's containing pages use 4 KiB leaves so individual linker
 * sections can receive RX, R, or RW permissions. Device registers are mapped
 * RW and never executable. None of these mappings has VM_PAGE_USER set.
 */
static bool map_kernel_space(struct page_table *root) {
        uintptr_t image_start = align_down((uintptr_t)kernel_start);
        uintptr_t image_end = align_up((uintptr_t)kernel_end);
        uintptr_t ram_start = platform_ram_start();
        uintptr_t ram_end = platform_ram_end();
        uintptr_t uart_start = platform_uart_base();
        uintptr_t plic_start = platform_plic_base();
        uintptr_t plic_end = plic_start + platform_plic_size();

        if (!map_identity_range_mixed(root, ram_start, image_start,
                                      VM_PAGE_READ | VM_PAGE_WRITE) ||
            !map_identity_range(root, image_start, image_end,
                                VM_PAGE_READ | VM_PAGE_WRITE) ||
            !map_identity_range_mixed(root, image_end, ram_end,
                                      VM_PAGE_READ | VM_PAGE_WRITE) ||
            !map_identity_range_mixed(root, plic_start, plic_end,
                                      VM_PAGE_READ | VM_PAGE_WRITE) ||
            !map_identity_range(root, uart_start, uart_start + PAGE_SIZE,
                                VM_PAGE_READ | VM_PAGE_WRITE) ||
            !protect_identity_range(root, (uintptr_t)text_start,
                                    (uintptr_t)text_end,
                                    VM_PAGE_READ | VM_PAGE_EXECUTE) ||
            !protect_identity_range(root, (uintptr_t)rodata_start,
                                    (uintptr_t)rodata_end, VM_PAGE_READ) ||
            !protect_identity_range(root, (uintptr_t)user_program_elf_start,
                                    (uintptr_t)user_program_elf_end,
                                    VM_PAGE_READ)) {
                return false;
        }

        for (size_t index = 0U; index < platform_virtio_count(); index++) {
                uintptr_t base;
                size_t size;
                uint32_t interrupt;

                if (!platform_virtio_device(index, &base, &size, &interrupt) ||
                    !map_identity_range(root, base, base + size,
                                        VM_PAGE_READ | VM_PAGE_WRITE)) {
                        return false;
                }
                (void)interrupt;
        }

        return true;
}

/*
 * Create a new root with kernel mappings. User pages are added later by the
 * process and ELF loaders. Rebuilding the compact hierarchy keeps ownership
 * simple: every address space can destroy all of its own table pages.
 */
struct page_table *virtual_memory_create_address_space(void) {
        struct page_table *root = page_table_create();

        if (root == NULL) {
                return NULL;
        }

        if (!map_kernel_space(root)) {
                page_table_destroy(root);
                return NULL;
        }

        return root;
}

static void verify_mapping(const struct page_table *root,
                           uintptr_t virtual_address, uint64_t required_flags,
                           uint64_t forbidden_flags) {
        uintptr_t physical_address;
        uint64_t actual_flags;

        if (!page_table_translate(root, virtual_address, &physical_address,
                                  &actual_flags) ||
            physical_address != virtual_address ||
            (actual_flags & required_flags) != required_flags ||
            (actual_flags & forbidden_flags) != 0U) {
                panic("Sv39 mapping verification failed");
        }
}

void virtual_memory_init(void) {
        /* Translation remains disabled while this table is built, so physical
         * page-table pointers are directly usable by the setup code. */
        struct page_table *root = virtual_memory_create_address_space();

        if (root == NULL) {
                panic("Failed to create the kernel Sv39 address space");
        }

        kernel_page_table = root;

        /* Exercise duplicate detection, transactional range rollback, removal,
         * protection changes, and page-table page reclamation before enabling
         * translation. */
        size_t pages_before_self_test = page_free_count();
        struct page_table *test_root = page_table_create();

        if (test_root == NULL ||
            !page_table_map(test_root, PAGE_SIZE, (uintptr_t)text_start,
                            VM_PAGE_READ) ||
            page_table_map(test_root, PAGE_SIZE, (uintptr_t)text_start,
                           VM_PAGE_READ) ||
            page_table_map_range(test_root, 0U, (uintptr_t)text_start,
                                 2U * PAGE_SIZE, VM_PAGE_READ)) {
                panic("Sv39 map invariant self-test failed");
        }

        uintptr_t unexpected_address;

        if (page_table_translate(test_root, 0U, &unexpected_address, NULL) ||
            !page_table_protect(test_root, PAGE_SIZE,
                                VM_PAGE_READ | VM_PAGE_EXECUTE) ||
            !page_table_unmap(test_root, PAGE_SIZE)) {
                panic("Sv39 rollback or protection self-test failed");
        }

        page_table_destroy(test_root);
        if (page_free_count() != pages_before_self_test) {
                panic("Sv39 self-test leaked page-table pages");
        }

        uintptr_t uart_address = platform_uart_base();
        uintptr_t plic_address = platform_plic_base();

        if (!page_table_unmap(root, uart_address)) {
                panic("Sv39 unmap self-test failed");
        }

        uintptr_t unmapped_address;

        if (page_table_translate(root, uart_address, &unmapped_address,
                                 NULL) ||
            !page_table_map(root, uart_address, uart_address,
                            VM_PAGE_READ | VM_PAGE_WRITE)) {
                panic("Sv39 remap self-test failed");
        }

        verify_mapping(root, (uintptr_t)text_start,
                       VM_PAGE_READ | VM_PAGE_EXECUTE, VM_PAGE_WRITE);
        verify_mapping(root, (uintptr_t)rodata_start, VM_PAGE_READ,
                       VM_PAGE_WRITE | VM_PAGE_EXECUTE);
        verify_mapping(root, (uintptr_t)user_program_elf_start, VM_PAGE_READ,
                       VM_PAGE_WRITE | VM_PAGE_EXECUTE);
        verify_mapping(root, (uintptr_t)bss_start, VM_PAGE_READ | VM_PAGE_WRITE,
                       VM_PAGE_EXECUTE);
        verify_mapping(root, uart_address, VM_PAGE_READ | VM_PAGE_WRITE,
                       VM_PAGE_EXECUTE);
        verify_mapping(root, plic_address, VM_PAGE_READ | VM_PAGE_WRITE,
                       VM_PAGE_EXECUTE);

        page_table_activate(root);

        if (!virtual_memory_is_enabled()) {
                panic("Failed to enable Sv39 virtual memory");
        }
}

bool virtual_memory_is_enabled(void) {
        /* satp.MODE occupies the high four bits on RV64. */
        uint64_t satp;

        __asm__ volatile("csrr %[satp], satp" : [satp] "=r"(satp));

        return (satp >> SATP_MODE_SHIFT) == SATP_MODE_SV39;
}
