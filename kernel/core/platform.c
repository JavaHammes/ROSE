/*
 * QEMU passes a Flattened Device Tree (FDT, commonly called a DTB) to the
 * kernel in register a1. The blob is a compact, big-endian description of the
 * machine that firmware selected for this boot.
 *
 * This file implements the small FDT subset that ROSE currently needs. It
 * discovers RAM, firmware-reserved regions, the timer frequency, the NS16550A
 * UART, the PLIC, and VirtIO-MMIO transports. Keeping this parser in one place
 * means the allocator, virtual-memory manager, timer, and drivers do not each
 * carry their own QEMU address constants.
 *
 * No pointers obtained from the blob are trusted until their containing range
 * has been checked against the total DTB size. The parser also uses fixed-size
 * stacks and result arrays because dynamic allocation is not available until
 * after platform discovery has completed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panic.h"
#include "platform.h"

#define FDT_MAGIC UINT32_C(0xd00dfeed)
#define FDT_BEGIN_NODE UINT32_C(1)
#define FDT_END_NODE UINT32_C(2)
#define FDT_PROPERTY UINT32_C(3)
#define FDT_NOP UINT32_C(4)
#define FDT_END UINT32_C(9)

/* Hard limits keep malformed firmware data from consuming kernel memory. */
enum {
        FDT_HEADER_SIZE = 40,
        FDT_MAX_DEPTH = 16,
        PLATFORM_RESERVED_REGION_LIMIT = 32,
        PLATFORM_VIRTIO_LIMIT = 8,
};

/* These defaults are used only while reporting an early device-tree panic. */
#define QEMU_UART_FALLBACK UINT64_C(0x10000000)

struct fdt_view {
        /* Validated slices of the original blob. */
        const uint8_t *blob;
        size_t total_size;
        const uint8_t *structure;
        size_t structure_size;
        const char *strings;
        size_t strings_size;
        size_t reserve_map_offset;
};

/*
 * State accumulated while visiting one node in the structure block.
 *
 * A node's reg property uses the address/size cell counts declared by its
 * parent. The address_cells and size_cells fields, in contrast, describe how
 * that node's children encode their reg properties.
 */
struct fdt_node {
        uint32_t address_cells;
        uint32_t size_cells;
        uint32_t reg_address_cells;
        uint32_t reg_size_cells;
        uintptr_t reg_start;
        size_t reg_size;
        uint32_t interrupt;
        bool has_reg;
        bool has_interrupt;
        bool enabled;
        bool is_memory;
        bool is_uart;
        bool is_plic;
        bool is_virtio;
        bool is_cpus;
        bool is_reserved_container;
        bool parent_is_reserved_container;
};

/* Physical ranges which the page allocator must never hand out. */
struct reserved_region {
        uintptr_t start;
        size_t size;
};

struct virtio_region {
        uintptr_t base;
        size_t size;
        uint32_t interrupt;
};

/* Parsed platform values become immutable after platform_init returns. */
static uintptr_t ram_start;
static uintptr_t ram_end;
static uint64_t timebase_frequency;
static uintptr_t uart_base = QEMU_UART_FALLBACK;
static uint32_t uart_interrupt;
static uintptr_t plic_base;
static size_t plic_size;
static struct reserved_region
    reserved_regions[PLATFORM_RESERVED_REGION_LIMIT];
static size_t reserved_region_count;
static bool ram_contains_kernel;
static bool uart_found;
static bool plic_found;
static struct virtio_region virtio_regions[PLATFORM_VIRTIO_LIMIT];
static size_t virtio_region_count;
static bool platform_initialized;

extern char kernel_start[];
extern char kernel_end[];

/* FDT integers are always big-endian, independent of the CPU byte order. */
static uint32_t read_be32(const uint8_t *bytes) {
        return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
               ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes) {
        return ((uint64_t)read_be32(bytes) << 32U) |
               read_be32(bytes + sizeof(uint32_t));
}

/* Every token and property payload in the structure block is 4-byte aligned. */
static size_t align4(size_t value) { return (value + 3U) & ~(size_t)3U; }

/* Overflow-safe containment check used before following any DTB offset. */
static bool range_is_inside(size_t offset, size_t length, size_t total) {
        return offset <= total && length <= total - offset;
}

/*
 * DTB strings are length-bounded data, not ordinary trusted C strings. These
 * helpers require a terminator within the validated region before succeeding.
 */
static bool bounded_string_equals(const char *text, size_t available,
                                  const char *expected) {
        size_t index = 0U;

        while (index < available && expected[index] != '\0' &&
               text[index] == expected[index]) {
                index++;
        }

        return index < available && text[index] == '\0' &&
               expected[index] == '\0';
}

static bool bounded_string_starts_with(const char *text, size_t available,
                                       const char *prefix) {
        size_t index = 0U;

        while (prefix[index] != '\0') {
                if (index >= available || text[index] != prefix[index]) {
                        return false;
                }
                index++;
        }

        return true;
}

static bool string_list_contains(const uint8_t *bytes, size_t length,
                                 const char *expected) {
        size_t offset = 0U;

        while (offset < length) {
                size_t remaining = length - offset;
                const char *item = (const char *)&bytes[offset];
                size_t item_length = 0U;

                while (item_length < remaining && item[item_length] != '\0') {
                        item_length++;
                }
                if (item_length == remaining) {
                        return false;
                }
                if (bounded_string_equals(item, item_length + 1U, expected)) {
                        return true;
                }

                offset += item_length + 1U;
        }

        return false;
}

/* Record one firmware or /reserved-memory range for the allocator. */
static void add_reserved_region(uint64_t start, uint64_t size) {
        if (size == 0U || start > UINTPTR_MAX || size > SIZE_MAX ||
            start > UINTPTR_MAX - size) {
                return;
        }
        if (reserved_region_count >= PLATFORM_RESERVED_REGION_LIMIT) {
                panic("Too many reserved device-tree memory regions");
        }

        reserved_regions[reserved_region_count].start = (uintptr_t)start;
        reserved_regions[reserved_region_count].size = (size_t)size;
        reserved_region_count++;
}

/*
 * Validate the fixed FDT header and turn its offsets into bounded views. The
 * parser requires version 17 because that format carries explicit structure
 * and string block sizes.
 */
static bool parse_header(const void *device_tree, struct fdt_view *view) {
        if (device_tree == NULL) {
                return false;
        }

        const uint8_t *header = device_tree;

        if (read_be32(header) != FDT_MAGIC) {
                return false;
        }

        size_t total_size = read_be32(header + 4U);
        size_t structure_offset = read_be32(header + 8U);
        size_t strings_offset = read_be32(header + 12U);
        size_t reserve_map_offset = read_be32(header + 16U);
        uint32_t version = read_be32(header + 20U);
        size_t strings_size = read_be32(header + 32U);
        size_t structure_size = read_be32(header + 36U);

        if (total_size < FDT_HEADER_SIZE || version < 17U ||
            !range_is_inside(structure_offset, structure_size, total_size) ||
            !range_is_inside(strings_offset, strings_size, total_size) ||
            !range_is_inside(reserve_map_offset, 16U, total_size)) {
                return false;
        }

        view->blob = header;
        view->total_size = total_size;
        view->structure = header + structure_offset;
        view->structure_size = structure_size;
        view->strings = (const char *)(header + strings_offset);
        view->strings_size = strings_size;
        view->reserve_map_offset = reserve_map_offset;
        return true;
}

/* Resolve a property-name offset into the validated string block. */
static const char *property_name(const struct fdt_view *view,
                                 uint32_t name_offset) {
        if (name_offset >= view->strings_size) {
                return NULL;
        }

        const char *name = view->strings + name_offset;

        for (size_t index = name_offset; index < view->strings_size; index++) {
                if (view->strings[index] == '\0') {
                        return name;
                }
        }

        return NULL;
}

static bool property_name_is(const struct fdt_view *view, const char *name,
                             const char *expected) {
        size_t available =
            view->strings_size - (size_t)(name - view->strings);
        return bounded_string_equals(name, available, expected);
}

static bool read_cells(const uint8_t *value, size_t length, uint32_t cell_count,
                       uint64_t *result) {
        /* ROSE supports the 32-bit and 64-bit encodings used by QEMU virt. */
        if (cell_count > 2U || length < (size_t)cell_count * sizeof(uint32_t)) {
                return false;
        }

        uint64_t parsed = 0U;

        for (uint32_t index = 0U; index < cell_count; index++) {
                parsed = (parsed << 32U) |
                         read_be32(value + index * sizeof(uint32_t));
        }

        *result = parsed;
        return true;
}

/* Decode the first address/size tuple from a node's reg property. */
static void parse_reg_property(struct fdt_node *node, const uint8_t *value,
                               size_t length) {
        size_t address_bytes =
            (size_t)node->reg_address_cells * sizeof(uint32_t);
        size_t size_bytes = (size_t)node->reg_size_cells * sizeof(uint32_t);
        uint64_t address;
        uint64_t size;

        if (length < address_bytes + size_bytes ||
            !read_cells(value, length, node->reg_address_cells, &address) ||
            !read_cells(value + address_bytes, length - address_bytes,
                        node->reg_size_cells, &size) ||
            address > UINTPTR_MAX || size > SIZE_MAX ||
            address > UINTPTR_MAX - size) {
                return;
        }

        node->reg_start = (uintptr_t)address;
        node->reg_size = (size_t)size;
        node->has_reg = size != 0U;
}

/* Apply the properties which affect the platform information ROSE consumes. */
static void parse_node_property(const struct fdt_view *view,
                                struct fdt_node *node, const char *name,
                                const uint8_t *value, size_t length) {
        if (property_name_is(view, name, "#address-cells") && length >= 4U) {
                node->address_cells = read_be32(value);
        } else if (property_name_is(view, name, "#size-cells") &&
                   length >= 4U) {
                node->size_cells = read_be32(value);
        } else if (property_name_is(view, name, "device_type") &&
                   bounded_string_equals((const char *)value, length,
                                         "memory")) {
                node->is_memory = true;
        } else if (property_name_is(view, name, "compatible")) {
                node->is_uart =
                    node->is_uart ||
                    string_list_contains(value, length, "ns16550a");
                node->is_plic =
                    node->is_plic ||
                    string_list_contains(value, length, "riscv,plic0") ||
                    string_list_contains(value, length,
                                         "sifive,plic-1.0.0");
                node->is_virtio =
                    node->is_virtio ||
                    string_list_contains(value, length, "virtio,mmio");
        } else if (property_name_is(view, name, "reg")) {
                parse_reg_property(node, value, length);
        } else if (property_name_is(view, name, "interrupts") &&
                   length >= sizeof(uint32_t)) {
                node->interrupt = read_be32(value);
                node->has_interrupt = true;
        } else if (property_name_is(view, name, "timebase-frequency") &&
                   node->is_cpus && length >= sizeof(uint32_t)) {
                timebase_frequency = read_be32(value);
        } else if (property_name_is(view, name, "status") &&
                   bounded_string_equals((const char *)value, length,
                                         "disabled")) {
                node->enabled = false;
        }
}

/*
 * Commit a completed node to the global platform description. If several RAM
 * nodes exist, prefer the one containing the linked kernel image; pages from a
 * disjoint RAM bank cannot serve the current identity-mapped allocator.
 */
static void finish_node(const struct fdt_node *node) {
        if (!node->enabled || !node->has_reg) {
                return;
        }

        uintptr_t region_end = node->reg_start + node->reg_size;

        if (node->is_memory) {
                bool contains_kernel =
                    node->reg_start <= (uintptr_t)kernel_start &&
                    region_end >= (uintptr_t)kernel_end;

                if (ram_end == 0U || (contains_kernel && !ram_contains_kernel)) {
                        ram_start = node->reg_start;
                        ram_end = region_end;
                        ram_contains_kernel = contains_kernel;
                }
        }
        if (node->is_uart && !uart_found) {
                uart_base = node->reg_start;
                if (node->has_interrupt) {
                        uart_interrupt = node->interrupt;
                }
                uart_found = node->has_interrupt;
        }
        if (node->is_plic && !plic_found) {
                plic_base = node->reg_start;
                plic_size = node->reg_size;
                plic_found = true;
        }
        if (node->is_virtio && node->has_interrupt) {
                if (virtio_region_count >= PLATFORM_VIRTIO_LIMIT) {
                        panic("Too many VirtIO device-tree regions");
                }

                virtio_regions[virtio_region_count++] =
                    (struct virtio_region){.base = node->reg_start,
                                           .size = node->reg_size,
                                           .interrupt = node->interrupt};
        }
        if (node->parent_is_reserved_container) {
                add_reserved_region(node->reg_start, node->reg_size);
        }
}

/*
 * The reservation map precedes the tree structure and consists of 64-bit
 * address/size pairs terminated by a zero pair. Firmware may use it for data
 * that is not represented by a /reserved-memory child.
 */
static void parse_reserve_map(const struct fdt_view *view) {
        size_t offset = view->reserve_map_offset;

        while (range_is_inside(offset, 16U, view->total_size)) {
                uint64_t address = read_be64(view->blob + offset);
                uint64_t size = read_be64(view->blob + offset + 8U);
                offset += 16U;

                if (address == 0U && size == 0U) {
                        return;
                }

                add_reserved_region(address, size);
        }

        panic("Unterminated device-tree memory reservation map");
}

/*
 * Walk the token stream without recursion. BEGIN_NODE pushes inherited cell
 * information, properties update the top entry, and END_NODE commits it.
 */
static bool parse_structure(const struct fdt_view *view) {
        struct fdt_node stack[FDT_MAX_DEPTH];
        size_t depth = 0U;
        size_t offset = 0U;

        while (range_is_inside(offset, sizeof(uint32_t),
                               view->structure_size)) {
                uint32_t token = read_be32(view->structure + offset);
                offset += sizeof(uint32_t);

                if (token == FDT_BEGIN_NODE) {
                        if (depth >= FDT_MAX_DEPTH) {
                                return false;
                        }

                        size_t name_start = offset;

                        while (offset < view->structure_size &&
                               view->structure[offset] != '\0') {
                                offset++;
                        }
                        if (offset == view->structure_size) {
                                return false;
                        }

                        size_t name_length = offset - name_start;
                        const char *node_name =
                            (const char *)(view->structure + name_start);
                        struct fdt_node node = {0};

                        /* Root defaults come from the FDT specification. QEMU
                         * buses normally state their values explicitly, but
                         * inheriting the parent values is a safe fallback for
                         * the simple identity-mapped buses ROSE supports. */
                        node.enabled = true;
                        node.address_cells = depth == 0U
                                                 ? 2U
                                                 : stack[depth - 1U]
                                                       .address_cells;
                        node.size_cells = depth == 0U
                                              ? 1U
                                              : stack[depth - 1U].size_cells;
                        node.reg_address_cells =
                            depth == 0U ? 2U
                                        : stack[depth - 1U].address_cells;
                        node.reg_size_cells =
                            depth == 0U ? 1U : stack[depth - 1U].size_cells;
                        node.parent_is_reserved_container =
                            depth != 0U &&
                            stack[depth - 1U].is_reserved_container;
                        node.is_memory = bounded_string_starts_with(
                            node_name, name_length, "memory@");
                        node.is_uart = bounded_string_starts_with(
                            node_name, name_length, "serial@");
                        node.is_plic = bounded_string_starts_with(
                            node_name, name_length, "plic@");
                        node.is_virtio = bounded_string_starts_with(
                            node_name, name_length, "virtio_mmio@");
                        node.is_cpus = bounded_string_equals(
                            node_name, name_length + 1U, "cpus");
                        node.is_reserved_container = bounded_string_equals(
                            node_name, name_length + 1U, "reserved-memory");
                        stack[depth] = node;
                        depth++;
                        offset = align4(offset + 1U);
                } else if (token == FDT_END_NODE) {
                        if (depth == 0U) {
                                return false;
                        }
                        depth--;
                        finish_node(&stack[depth]);
                } else if (token == FDT_PROPERTY) {
                        if (depth == 0U ||
                            !range_is_inside(offset, 8U,
                                             view->structure_size)) {
                                return false;
                        }

                        size_t length = read_be32(view->structure + offset);
                        uint32_t name_offset =
                            read_be32(view->structure + offset + 4U);
                        offset += 8U;

                        if (!range_is_inside(offset, length,
                                             view->structure_size)) {
                                return false;
                        }

                        const char *name = property_name(view, name_offset);

                        if (name == NULL) {
                                return false;
                        }

                        parse_node_property(view, &stack[depth - 1U], name,
                                            view->structure + offset, length);
                        offset = align4(offset + length);
                } else if (token == FDT_NOP) {
                        continue;
                } else if (token == FDT_END) {
                        return depth == 0U;
                } else {
                        return false;
                }
        }

        return false;
}

/*
 * Parse and validate all platform data before any memory allocator or MMIO
 * driver is initialized. The DTB itself is reserved so it remains readable
 * after the allocator starts serving pages.
 */
void platform_init(const void *device_tree) {
        if (platform_initialized) {
                panic("Platform initialized twice");
        }

        struct fdt_view view;

        if (!parse_header(device_tree, &view)) {
                panic("Invalid flattened device tree header");
        }

        add_reserved_region((uintptr_t)device_tree, view.total_size);
        parse_reserve_map(&view);

        if (!parse_structure(&view)) {
                panic("Invalid flattened device tree structure");
        }
        if (!ram_contains_kernel || ram_end <= ram_start) {
                panic("Device tree has no usable kernel RAM region");
        }
        if (timebase_frequency == 0U) {
                panic("Device tree has no timer frequency");
        }
        if (!uart_found) {
                panic("Device tree has no supported UART");
        }
        if (!plic_found || plic_size == 0U) {
                panic("Device tree has no supported PLIC");
        }

        platform_initialized = true;
}

/* Read-only accessors used by the rest of the kernel after initialization. */
uintptr_t platform_ram_start(void) { return ram_start; }

uintptr_t platform_ram_end(void) { return ram_end; }

uint64_t platform_timebase_frequency(void) { return timebase_frequency; }

uintptr_t platform_uart_base(void) { return uart_base; }

uint32_t platform_uart_interrupt(void) { return uart_interrupt; }

uintptr_t platform_plic_base(void) { return plic_base; }

size_t platform_plic_size(void) { return plic_size; }

size_t platform_virtio_count(void) { return virtio_region_count; }

bool platform_virtio_device(size_t index, uintptr_t *base, size_t *size,
                            uint32_t *interrupt) {
        if (index >= virtio_region_count || base == NULL || size == NULL ||
            interrupt == NULL) {
                return false;
        }

        *base = virtio_regions[index].base;
        *size = virtio_regions[index].size;
        *interrupt = virtio_regions[index].interrupt;
        return true;
}

size_t platform_reserved_region_count(void) { return reserved_region_count; }

bool platform_reserved_region(size_t index, uintptr_t *start, size_t *size) {
        /* Both output pointers are mandatory, keeping partial results out of
         * callers when an invalid index is supplied. */
        if (index >= reserved_region_count || start == NULL || size == NULL) {
                return false;
        }

        *start = reserved_regions[index].start;
        *size = reserved_regions[index].size;
        return true;
}
