#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct page_table;

/* Fixed ownership capacity avoids requiring a kernel heap during early work. */
enum { ELF_LOADER_MAX_PAGES = 64 };

/* One record owns one allocated physical page and its user mapping. */
struct elf_loaded_page {
        uintptr_t virtual_address;
        void *physical_page;
        uint64_t flags;
};

struct elf_loaded_image {
        /* entry is the validated U-mode start address. */
        uintptr_t entry;
        size_t page_count;
        struct elf_loaded_page pages[ELF_LOADER_MAX_PAGES];
};

/* Validate and materialize an ELF entirely inside the supplied user range. */
bool elf_load_image(struct page_table *root, const void *image,
                    size_t image_size, uintptr_t user_address_min,
                    uintptr_t user_address_max,
                    struct elf_loaded_image *loaded_image);

/* Remove mappings and free all pages, including those from a partial load. */
void elf_unload_image(struct page_table *root,
                      struct elf_loaded_image *loaded_image);

#endif
