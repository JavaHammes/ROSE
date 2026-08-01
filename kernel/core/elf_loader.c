/*
 * Minimal in-memory ELF64 loader for user programs.
 *
 * ROSE resolves independently linked user ELFs through its VFS. Loading still
 * treats every file as untrusted: validate every offset before reading it,
 * accept only the expected architecture and executable type, enforce the
 * configured user address range, and never create a writable+executable page.
 *
 * The loader owns every physical page recorded in elf_loaded_image. Page-table
 * mappings themselves do not own physical memory, so elf_unload_image removes
 * each mapping and then returns its corresponding page to the allocator.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf_loader.h"
#include "page_allocator.h"
#include "panic.h"
#include "virtual_memory.h"

/* Byte offsets and values from the ELF64 file format. Structures are not cast
 * directly over the image because that would rely on alignment and host C
 * structure packing. */
enum {
        BITS_PER_BYTE = 8,
        ELF_HEADER_SIZE = 64,
        ELF_PROGRAM_HEADER_SIZE = 56,
        ELF_MAGIC_VALUE = 0x7f,
        ELF_IDENT_MAGIC_0 = 0,
        ELF_IDENT_MAGIC_1 = 1,
        ELF_IDENT_MAGIC_2 = 2,
        ELF_IDENT_MAGIC_3 = 3,
        ELF_IDENT_CLASS = 4,
        ELF_IDENT_DATA = 5,
        ELF_IDENT_VERSION = 6,
        ELF_HEADER_TYPE = 16,
        ELF_HEADER_MACHINE = 18,
        ELF_HEADER_VERSION = 20,
        ELF_HEADER_ENTRY = 24,
        ELF_HEADER_PROGRAM_OFFSET = 32,
        ELF_HEADER_SIZE_OFFSET = 52,
        ELF_HEADER_PROGRAM_ENTRY_SIZE = 54,
        ELF_HEADER_PROGRAM_COUNT = 56,
        ELF_PROGRAM_TYPE = 0,
        ELF_PROGRAM_FLAGS = 4,
        ELF_PROGRAM_OFFSET = 8,
        ELF_PROGRAM_VIRTUAL_ADDRESS = 16,
        ELF_PROGRAM_FILE_SIZE = 32,
        ELF_PROGRAM_MEMORY_SIZE = 40,
        ELF_PROGRAM_ALIGNMENT = 48,
        ELF_CLASS_64 = 2,
        ELF_DATA_LITTLE_ENDIAN = 1,
        ELF_IDENT_VERSION_CURRENT = 1,
        ELF_TYPE_EXECUTABLE = 2,
        ELF_MACHINE_RISCV = 243,
        ELF_VERSION_CURRENT = 1,
        ELF_PROGRAM_LOAD = 1,
};

enum elf_segment_flags {
        ELF_SEGMENT_EXECUTE = (1U << 0),
        ELF_SEGMENT_WRITE = (1U << 1),
        ELF_SEGMENT_READ = (1U << 2),
};

struct elf_header {
        /* Only the fields needed after initial validation are retained. */
        uint64_t entry;
        uint64_t program_header_offset;
        uint16_t program_header_count;
};

struct elf_program_header {
        /* Decoded representation of one on-disk Elf64_Phdr. */
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t virtual_address;
        uint64_t file_size;
        uint64_t memory_size;
        uint64_t alignment;
};

/* ELF64 files produced for RISC-V are little-endian. */
static uint16_t read_u16(const uint8_t *bytes) {
        return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << BITS_PER_BYTE);
}

static uint32_t read_u32(const uint8_t *bytes) {
        uint32_t value = 0U;

        for (size_t index = 0U; index < sizeof(value); index++) {
                value |= (uint32_t)bytes[index] << (index * BITS_PER_BYTE);
        }

        return value;
}

static uint64_t read_u64(const uint8_t *bytes) {
        uint64_t value = 0U;

        for (size_t index = 0U; index < sizeof(value); index++) {
                value |= (uint64_t)bytes[index] << (index * BITS_PER_BYTE);
        }

        return value;
}

static void bytes_zero(void *destination, size_t size) {
        volatile uint8_t *bytes = destination;

        for (size_t index = 0U; index < size; index++) {
                bytes[index] = 0U;
        }
}

/* Overflow-safe test for a file range [offset, offset + length). */
static bool range_is_inside(uint64_t offset, uint64_t length,
                            size_t container_size) {
        if (offset > container_size) {
                return false;
        }

        return length <= (uint64_t)container_size - offset;
}

/*
 * Validate the ELF identity and the program-header table before returning any
 * decoded offsets. Section headers are intentionally irrelevant at runtime.
 */
static bool parse_elf_header(const uint8_t *image, size_t image_size,
                             struct elf_header *header) {
        if (image_size < ELF_HEADER_SIZE ||
            image[ELF_IDENT_MAGIC_0] != ELF_MAGIC_VALUE ||
            image[ELF_IDENT_MAGIC_1] != 'E' ||
            image[ELF_IDENT_MAGIC_2] != 'L' ||
            image[ELF_IDENT_MAGIC_3] != 'F' ||
            image[ELF_IDENT_CLASS] != ELF_CLASS_64 ||
            image[ELF_IDENT_DATA] != ELF_DATA_LITTLE_ENDIAN ||
            image[ELF_IDENT_VERSION] != ELF_IDENT_VERSION_CURRENT ||
            read_u16(&image[ELF_HEADER_TYPE]) != ELF_TYPE_EXECUTABLE ||
            read_u16(&image[ELF_HEADER_MACHINE]) != ELF_MACHINE_RISCV ||
            read_u32(&image[ELF_HEADER_VERSION]) != ELF_VERSION_CURRENT ||
            read_u16(&image[ELF_HEADER_SIZE_OFFSET]) != ELF_HEADER_SIZE ||
            read_u16(&image[ELF_HEADER_PROGRAM_ENTRY_SIZE]) !=
                ELF_PROGRAM_HEADER_SIZE) {
                return false;
        }

        header->entry = read_u64(&image[ELF_HEADER_ENTRY]);
        header->program_header_offset =
            read_u64(&image[ELF_HEADER_PROGRAM_OFFSET]);
        header->program_header_count =
            read_u16(&image[ELF_HEADER_PROGRAM_COUNT]);

        uint64_t table_size =
            (uint64_t)header->program_header_count * ELF_PROGRAM_HEADER_SIZE;

        if (header->program_header_count == 0U) {
                return false;
        }

        return range_is_inside(header->program_header_offset, table_size,
                               image_size);
}

/* Decode one already-bounds-checked program-header entry. */
static struct elf_program_header
parse_program_header(const uint8_t *program_header) {
        return (struct elf_program_header){
            .type = read_u32(&program_header[ELF_PROGRAM_TYPE]),
            .flags = read_u32(&program_header[ELF_PROGRAM_FLAGS]),
            .offset = read_u64(&program_header[ELF_PROGRAM_OFFSET]),
            .virtual_address =
                read_u64(&program_header[ELF_PROGRAM_VIRTUAL_ADDRESS]),
            .file_size = read_u64(&program_header[ELF_PROGRAM_FILE_SIZE]),
            .memory_size = read_u64(&program_header[ELF_PROGRAM_MEMORY_SIZE]),
            .alignment = read_u64(&program_header[ELF_PROGRAM_ALIGNMENT]),
        };
}

static uintptr_t align_down(uintptr_t address) {
        return address & ~(PAGE_SIZE - 1U);
}

static bool align_up(uintptr_t address, uintptr_t *aligned_address) {
        /* The explicit overflow check is required for hostile ELF addresses. */
        if (address > UINTPTR_MAX - (PAGE_SIZE - 1U)) {
                return false;
        }

        *aligned_address = (address + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
        return true;
}

/* Translate ELF segment flags into the public virtual-memory API. */
static uint64_t segment_page_flags(uint32_t elf_flags) {
        uint64_t flags = VM_PAGE_USER;

        if ((elf_flags & ELF_SEGMENT_READ) != 0U) {
                flags |= VM_PAGE_READ;
        }
        if ((elf_flags & ELF_SEGMENT_WRITE) != 0U) {
                /* Sv39 does not permit a writable page without read access. */
                flags |= VM_PAGE_READ | VM_PAGE_WRITE;
        }
        if ((elf_flags & ELF_SEGMENT_EXECUTE) != 0U) {
                flags |= VM_PAGE_EXECUTE;
        }

        return flags;
}

/*
 * Validate one PT_LOAD segment without mutating an address space.
 *
 * In addition to ordinary file and address bounds, this rejects unknown
 * permission bits, W+X segments, invalid alignment relationships, and images
 * exceeding the loader's fixed ownership table. The page budget is
 * deliberately conservative when two segments overlap; rejecting an unusually
 * fragmented image is preferable to overflowing ownership metadata.
 */
static bool validate_load_segment(const struct elf_program_header *segment,
                                  size_t image_size, uintptr_t user_address_min,
                                  uintptr_t user_address_max,
                                  size_t *page_budget) {
        const uint32_t known_flags =
            ELF_SEGMENT_READ | ELF_SEGMENT_WRITE | ELF_SEGMENT_EXECUTE;

        if (segment->file_size > segment->memory_size ||
            !range_is_inside(segment->offset, segment->file_size, image_size) ||
            (segment->flags & ~known_flags) != 0U ||
            (segment->flags & (ELF_SEGMENT_READ | ELF_SEGMENT_WRITE |
                               ELF_SEGMENT_EXECUTE)) == 0U ||
            (segment->flags & (ELF_SEGMENT_WRITE | ELF_SEGMENT_EXECUTE)) ==
                (ELF_SEGMENT_WRITE | ELF_SEGMENT_EXECUTE)) {
                return false;
        }

        if (segment->alignment > 1U &&
            ((segment->alignment & (segment->alignment - 1U)) != 0U ||
             segment->virtual_address % segment->alignment !=
                 segment->offset % segment->alignment)) {
                return false;
        }

        if (segment->memory_size == 0U) {
                return true;
        }

        if (segment->virtual_address > UINTPTR_MAX ||
            segment->memory_size >
                UINTPTR_MAX - (uintptr_t)segment->virtual_address) {
                return false;
        }

        uintptr_t segment_start = (uintptr_t)segment->virtual_address;
        uintptr_t segment_end = segment_start + (uintptr_t)segment->memory_size;

        if (segment_start < user_address_min ||
            segment_end > user_address_max) {
                return false;
        }

        uintptr_t page_end;

        if (!align_up(segment_end, &page_end)) {
                return false;
        }

        size_t pages = (page_end - align_down(segment_start)) / PAGE_SIZE;

        if (pages > ELF_LOADER_MAX_PAGES - *page_budget) {
                return false;
        }

        *page_budget += pages;
        return true;
}

/* Find ownership metadata for the page containing virtual_address. */
static struct elf_loaded_page *
find_loaded_page(struct elf_loaded_image *loaded_image,
                 uintptr_t virtual_address) {
        uintptr_t page_address = align_down(virtual_address);

        for (size_t index = 0U; index < loaded_image->page_count; index++) {
                if (loaded_image->pages[index].virtual_address ==
                    page_address) {
                        return &loaded_image->pages[index];
                }
        }

        return NULL;
}

/*
 * Undo a successful or partially successful load.
 *
 * A translated page must still point at the physical page recorded by the
 * loader. A mismatch means ownership metadata or a page table was corrupted,
 * and freeing that page would risk returning live memory to the allocator.
 */
void elf_unload_image(struct page_table *root,
                      struct elf_loaded_image *loaded_image) {
        if (loaded_image == NULL) {
                return;
        }

        for (size_t index = 0U; index < loaded_image->page_count; index++) {
                struct elf_loaded_page *page = &loaded_image->pages[index];

                if (root != NULL) {
                        uintptr_t mapped_physical_address;

                        if (page_table_translate(root, page->virtual_address,
                                                 &mapped_physical_address,
                                                 NULL)) {
                                if (align_down(mapped_physical_address) !=
                                        (uintptr_t)page->physical_page ||
                                    !page_table_unmap(root,
                                                      page->virtual_address)) {
                                        panic("ELF page ownership mismatch");
                                }
                        }
                }

                page_release(page->physical_page);
        }

        bytes_zero(loaded_image, sizeof(*loaded_image));
}

/*
 * Allocate and map every page covered by a segment. ELF permits adjacent
 * segments to share a final page, so existing ownership records have their
 * permissions combined explicitly. The combined result is checked for W^X
 * before page_table_protect changes the mapping.
 */
static bool load_segment_pages(struct page_table *root,
                               const struct elf_program_header *segment,
                               struct elf_loaded_image *loaded_image) {
        if (segment->memory_size == 0U) {
                return true;
        }

        uintptr_t segment_start = (uintptr_t)segment->virtual_address;
        uintptr_t segment_end = segment_start + (uintptr_t)segment->memory_size;
        uintptr_t page_end;

        if (!align_up(segment_end, &page_end)) {
                return false;
        }

        uint64_t flags = segment_page_flags(segment->flags);

        for (uintptr_t virtual_address = align_down(segment_start);
             virtual_address < page_end; virtual_address += PAGE_SIZE) {
                struct elf_loaded_page *page =
                    find_loaded_page(loaded_image, virtual_address);

                if (page != NULL) {
                        uint64_t combined_flags = page->flags | flags;

                        if ((combined_flags &
                             (VM_PAGE_WRITE | VM_PAGE_EXECUTE)) ==
                            (VM_PAGE_WRITE | VM_PAGE_EXECUTE)) {
                                return false;
                        }

                        page->flags = combined_flags;
                        if (!page_table_protect(root, virtual_address,
                                               combined_flags)) {
                                return false;
                        }
                        continue;
                }

                if (loaded_image->page_count >= ELF_LOADER_MAX_PAGES) {
                        return false;
                }

                void *physical_page = page_alloc();

                if (physical_page == NULL) {
                        return false;
                }

                page = &loaded_image->pages[loaded_image->page_count];
                page->virtual_address = virtual_address;
                page->physical_page = physical_page;
                page->flags = flags;
                loaded_image->page_count++;

                if (!page_table_map(root, virtual_address,
                                    (uintptr_t)physical_page, flags)) {
                        return false;
                }
        }

        return true;
}

/*
 * Copy only file-backed bytes. page_alloc returns zeroed memory, so the
 * remaining memory_size - file_size bytes automatically implement ELF BSS.
 */
static bool copy_segment_bytes(const uint8_t *image,
                               const struct elf_program_header *segment,
                               struct elf_loaded_image *loaded_image) {
        for (uint64_t offset = 0U; offset < segment->file_size; offset++) {
                uintptr_t virtual_address =
                    (uintptr_t)(segment->virtual_address + offset);
                struct elf_loaded_page *page =
                    find_loaded_page(loaded_image, virtual_address);

                if (page == NULL) {
                        return false;
                }

                uint8_t *destination = page->physical_page;
                destination[virtual_address & (PAGE_SIZE - 1U)] =
                    image[segment->offset + offset];
        }

        return true;
}

/*
 * Load an ELF image in two passes.
 *
 * Pass one validates every PT_LOAD entry and verifies that the entry point lies
 * inside executable memory. Pass two allocates pages and copies bytes. This
 * ordering prevents a malformed later header from causing observable partial
 * mappings. Allocation failures during pass two are rolled back by
 * elf_unload_image.
 */
bool elf_load_image(struct page_table *root, const void *image,
                    size_t image_size, uintptr_t user_address_min,
                    uintptr_t user_address_max,
                    struct elf_loaded_image *loaded_image) {
        if (root == NULL || image == NULL || loaded_image == NULL ||
            user_address_min >= user_address_max ||
            (user_address_min & (PAGE_SIZE - 1U)) != 0U ||
            (user_address_max & (PAGE_SIZE - 1U)) != 0U) {
                return false;
        }

        bytes_zero(loaded_image, sizeof(*loaded_image));

        const uint8_t *image_bytes = image;
        struct elf_header header;

        if (!parse_elf_header(image_bytes, image_size, &header) ||
            header.entry > UINTPTR_MAX) {
                return false;
        }

        size_t page_budget = 0U;
        bool has_load_segment = false;
        bool entry_is_executable = false;

        /* Validation pass: do not allocate or modify the address space. */
        for (size_t index = 0U; index < header.program_header_count; index++) {
                const uint8_t *raw_program_header =
                    &image_bytes[header.program_header_offset +
                                 (index * ELF_PROGRAM_HEADER_SIZE)];
                struct elf_program_header segment =
                    parse_program_header(raw_program_header);

                if (segment.type != ELF_PROGRAM_LOAD) {
                        continue;
                }

                has_load_segment = true;
                if (!validate_load_segment(&segment, image_size,
                                           user_address_min, user_address_max,
                                           &page_budget)) {
                        return false;
                }

                if ((segment.flags & ELF_SEGMENT_EXECUTE) != 0U &&
                    header.entry >= segment.virtual_address &&
                    header.entry - segment.virtual_address <
                        segment.memory_size) {
                        entry_is_executable = true;
                }
        }

        if (!has_load_segment || !entry_is_executable) {
                return false;
        }

        loaded_image->entry = (uintptr_t)header.entry;

        /* Materialization pass: every failure below performs full teardown. */
        for (size_t index = 0U; index < header.program_header_count; index++) {
                const uint8_t *raw_program_header =
                    &image_bytes[header.program_header_offset +
                                 (index * ELF_PROGRAM_HEADER_SIZE)];
                struct elf_program_header segment =
                    parse_program_header(raw_program_header);

                if (segment.type != ELF_PROGRAM_LOAD) {
                        continue;
                }

                if (!load_segment_pages(root, &segment, loaded_image) ||
                    !copy_segment_bytes(image_bytes, &segment, loaded_image)) {
                        elf_unload_image(root, loaded_image);
                        return false;
                }
        }

        /* Recheck the installed entry mapping rather than trusting segment
         * metadata alone. It must be user-accessible, executable, and not
         * writable. */
        uintptr_t entry_physical_address;
        uint64_t entry_flags;

        if (!page_table_translate(root, loaded_image->entry,
                                  &entry_physical_address, &entry_flags) ||
            (entry_flags & (VM_PAGE_USER | VM_PAGE_EXECUTE)) !=
                (VM_PAGE_USER | VM_PAGE_EXECUTE) ||
            (entry_flags & VM_PAGE_WRITE) != 0U) {
                elf_unload_image(root, loaded_image);
                return false;
        }

        return true;
}
