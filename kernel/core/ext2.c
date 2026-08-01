/*
 * Compact writable ext2 implementation for the first ROSE disk format.
 *
 * The supported profile is deliberately narrow: one block group, 1 KiB
 * blocks, 128-byte inodes, file-type directory entries, and direct plus singly
 * indirect block addressing. Those constraints are validated at mount time.
 * The image generator emits exactly this profile, while retaining standard
 * ext2 on-disk structures so ordinary ext2 tools can inspect the disk.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_cache.h"
#include "block_device.h"
#include "ext2.h"
#include "vfs.h"

enum {
        EXT2_MAGIC = 0xef53,
        EXT2_ROOT_INODE = 2,
        EXT2_DIRECT_BLOCKS = 12,
        EXT2_INDIRECT_BLOCK_ENTRIES =
            BLOCK_CACHE_BLOCK_SIZE / sizeof(uint32_t),
        EXT2_MAX_DATA_BLOCKS =
            EXT2_DIRECT_BLOCKS + EXT2_INDIRECT_BLOCK_ENTRIES,
        EXT2_SECTORS_PER_BLOCK =
            BLOCK_CACHE_BLOCK_SIZE / BLOCK_DEVICE_SECTOR_SIZE,
        EXT2_INODE_SIZE = 128,
        EXT2_PATH_MAX = 64,
        EXT2_DIRECTORY_HEADER_SIZE = 8,
        EXT2_FEATURE_INCOMPAT_FILETYPE = 0x2,
        EXT2_MODE_TYPE_MASK = 0xf000,
        EXT2_MODE_DIRECTORY = 0x4000,
        EXT2_MODE_REGULAR = 0x8000,
        EXT2_DIRECTORY_TYPE_REGULAR = 1,
        EXT2_DIRECTORY_TYPE_DIRECTORY = 2,
};

enum superblock_offset {
        SUPER_INODES_COUNT = 0,
        SUPER_BLOCKS_COUNT = 4,
        SUPER_FREE_BLOCKS_COUNT = 12,
        SUPER_FREE_INODES_COUNT = 16,
        SUPER_FIRST_DATA_BLOCK = 20,
        SUPER_LOG_BLOCK_SIZE = 24,
        SUPER_BLOCKS_PER_GROUP = 32,
        SUPER_INODES_PER_GROUP = 40,
        SUPER_MAGIC = 56,
        SUPER_REVISION = 76,
        SUPER_FIRST_INODE = 84,
        SUPER_INODE_SIZE = 88,
        SUPER_FEATURE_INCOMPAT = 96,
};

enum group_offset {
        GROUP_BLOCK_BITMAP = 0,
        GROUP_INODE_BITMAP = 4,
        GROUP_INODE_TABLE = 8,
        GROUP_FREE_BLOCKS_COUNT = 12,
        GROUP_FREE_INODES_COUNT = 14,
        GROUP_USED_DIRECTORIES_COUNT = 16,
};

enum inode_offset {
        INODE_MODE = 0,
        INODE_SIZE = 4,
        INODE_LINK_COUNT = 26,
        INODE_BLOCK_COUNT = 28,
        INODE_BLOCK_POINTERS = 40,
};

struct ext2_inode {
        uint16_t mode;
        uint16_t link_count;
        uint32_t size;
        uint32_t block_count;
        uint32_t direct_blocks[EXT2_DIRECT_BLOCKS];
        uint32_t indirect_block;
};

struct ext2_filesystem {
        uint32_t blocks_count;
        uint32_t inodes_count;
        uint32_t first_data_block;
        uint32_t blocks_per_group;
        uint32_t inodes_per_group;
        uint32_t first_inode;
        uint32_t block_bitmap;
        uint32_t inode_bitmap;
        uint32_t inode_table;
        uint16_t free_blocks;
        uint16_t free_inodes;
        uint16_t used_directories;
};

static struct ext2_filesystem filesystem;

static uint16_t read_u16(const uint8_t *bytes) {
        return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static uint32_t read_u32(const uint8_t *bytes) {
        return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
               ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void write_u16(uint8_t *bytes, uint16_t value) {
        bytes[0] = (uint8_t)value;
        bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
        for (size_t index = 0U; index < sizeof(value); index++) {
                bytes[index] = (uint8_t)(value >> (index * 8U));
        }
}

static void copy_bytes(void *destination, const void *source, size_t length) {
        uint8_t *output = destination;
        const uint8_t *input = source;
        for (size_t index = 0U; index < length; index++) {
                output[index] = input[index];
        }
}

static void zero_bytes(void *destination, size_t length) {
        uint8_t *output = destination;
        for (size_t index = 0U; index < length; index++) {
                output[index] = 0U;
        }
}

static size_t string_length_bounded(const char *text, size_t limit) {
        size_t length = 0U;
        while (length < limit && text[length] != '\0') {
                length++;
        }
        return length;
}

static bool names_equal(const uint8_t *disk_name, size_t disk_length,
                        const char *name, size_t name_length) {
        if (disk_length != name_length) {
                return false;
        }
        for (size_t index = 0U; index < name_length; index++) {
                if (disk_name[index] != (uint8_t)name[index]) {
                        return false;
                }
        }
        return true;
}

static size_t directory_record_size(size_t name_length) {
        return (EXT2_DIRECTORY_HEADER_SIZE + name_length + 3U) & ~(size_t)3U;
}

static enum vfs_node_type mode_type(uint16_t mode) {
        return (mode & EXT2_MODE_TYPE_MASK) == EXT2_MODE_DIRECTORY
                   ? VFS_NODE_DIRECTORY
                   : VFS_NODE_REGULAR;
}

static uint8_t directory_type(enum vfs_node_type type) {
        return type == VFS_NODE_DIRECTORY ? EXT2_DIRECTORY_TYPE_DIRECTORY
                                          : EXT2_DIRECTORY_TYPE_REGULAR;
}

static bool read_inode(struct ext2_filesystem *fs, uint32_t number,
                       struct ext2_inode *inode) {
        if (number == 0U || number > fs->inodes_count || inode == NULL) {
                return false;
        }

        uint32_t index = number - 1U;
        uint32_t block = fs->inode_table +
                         index / (BLOCK_CACHE_BLOCK_SIZE / EXT2_INODE_SIZE);
        size_t offset =
            (size_t)(index %
                     (BLOCK_CACHE_BLOCK_SIZE / EXT2_INODE_SIZE)) *
            EXT2_INODE_SIZE;
        static uint8_t bytes[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(block, bytes)) {
                return false;
        }

        inode->mode = read_u16(&bytes[offset + INODE_MODE]);
        inode->size = read_u32(&bytes[offset + INODE_SIZE]);
        inode->link_count = read_u16(&bytes[offset + INODE_LINK_COUNT]);
        inode->block_count = read_u32(&bytes[offset + INODE_BLOCK_COUNT]);
        for (size_t pointer = 0U; pointer < EXT2_DIRECT_BLOCKS; pointer++) {
                inode->direct_blocks[pointer] = read_u32(
                    &bytes[offset + INODE_BLOCK_POINTERS +
                           pointer * sizeof(uint32_t)]);
        }
        inode->indirect_block = read_u32(
            &bytes[offset + INODE_BLOCK_POINTERS +
                   EXT2_DIRECT_BLOCKS * sizeof(uint32_t)]);
        return inode->mode != 0U;
}

static bool write_inode(struct ext2_filesystem *fs, uint32_t number,
                        const struct ext2_inode *inode) {
        if (number == 0U || number > fs->inodes_count || inode == NULL) {
                return false;
        }

        uint32_t index = number - 1U;
        uint32_t block = fs->inode_table +
                         index / (BLOCK_CACHE_BLOCK_SIZE / EXT2_INODE_SIZE);
        size_t offset =
            (size_t)(index %
                     (BLOCK_CACHE_BLOCK_SIZE / EXT2_INODE_SIZE)) *
            EXT2_INODE_SIZE;
        static uint8_t bytes[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(block, bytes)) {
                return false;
        }

        /* Preserve ownership and timestamp fields emitted by the image. */
        write_u16(&bytes[offset + INODE_MODE], inode->mode);
        write_u32(&bytes[offset + INODE_SIZE], inode->size);
        write_u16(&bytes[offset + INODE_LINK_COUNT], inode->link_count);
        write_u32(&bytes[offset + INODE_BLOCK_COUNT], inode->block_count);
        for (size_t pointer = 0U; pointer < EXT2_DIRECT_BLOCKS; pointer++) {
                write_u32(&bytes[offset + INODE_BLOCK_POINTERS +
                                 pointer * sizeof(uint32_t)],
                          inode->direct_blocks[pointer]);
        }
        write_u32(&bytes[offset + INODE_BLOCK_POINTERS +
                         EXT2_DIRECT_BLOCKS * sizeof(uint32_t)],
                  inode->indirect_block);
        return block_cache_write(block, bytes);
}

static bool write_free_counts(struct ext2_filesystem *fs) {
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(1U, block)) {
                return false;
        }
        write_u32(&block[SUPER_FREE_BLOCKS_COUNT], fs->free_blocks);
        write_u32(&block[SUPER_FREE_INODES_COUNT], fs->free_inodes);
        if (!block_cache_write(1U, block) || !block_cache_read(2U, block)) {
                return false;
        }
        write_u16(&block[GROUP_FREE_BLOCKS_COUNT], fs->free_blocks);
        write_u16(&block[GROUP_FREE_INODES_COUNT], fs->free_inodes);
        write_u16(&block[GROUP_USED_DIRECTORIES_COUNT], fs->used_directories);
        return block_cache_write(2U, block);
}

static bool bitmap_test(const uint8_t *bitmap, uint32_t index) {
        return (bitmap[index / 8U] & (UINT8_C(1) << (index % 8U))) != 0U;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
        bitmap[index / 8U] |= UINT8_C(1) << (index % 8U);
}

static void bitmap_clear(uint8_t *bitmap, uint32_t index) {
        bitmap[index / 8U] &= (uint8_t)~(UINT8_C(1) << (index % 8U));
}

static uint32_t allocate_block(struct ext2_filesystem *fs) {
        static uint8_t bitmap[BLOCK_CACHE_BLOCK_SIZE];
        if (fs->free_blocks == 0U ||
            !block_cache_read(fs->block_bitmap, bitmap)) {
                return 0U;
        }

        for (uint32_t block = fs->first_data_block;
             block < fs->blocks_count; block++) {
                uint32_t bit = block - fs->first_data_block;
                if (bitmap_test(bitmap, bit)) {
                        continue;
                }
                bitmap_set(bitmap, bit);
                static uint8_t empty[BLOCK_CACHE_BLOCK_SIZE];
                zero_bytes(empty, sizeof(empty));
                if (!block_cache_write(fs->block_bitmap, bitmap) ||
                    !block_cache_write(block, empty)) {
                        return 0U;
                }
                fs->free_blocks--;
                if (!write_free_counts(fs)) {
                        return 0U;
                }
                return block;
        }
        return 0U;
}

static bool release_block(struct ext2_filesystem *fs, uint32_t block) {
        if (block < fs->first_data_block || block >= fs->blocks_count) {
                return false;
        }
        static uint8_t bitmap[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(fs->block_bitmap, bitmap)) {
                return false;
        }
        uint32_t bit = block - fs->first_data_block;
        if (!bitmap_test(bitmap, bit)) {
                return false;
        }
        bitmap_clear(bitmap, bit);
        if (!block_cache_write(fs->block_bitmap, bitmap)) {
                return false;
        }
        fs->free_blocks++;
        return write_free_counts(fs);
}

/* Resolve one file-relative data block without treating an unallocated block
 * as an error. The singly indirect table contains 256 little-endian block
 * numbers in the supported 1 KiB ext2 profile. */
static bool inode_data_block(const struct ext2_inode *inode, size_t index,
                             uint32_t *block_number) {
        if (inode == NULL || block_number == NULL ||
            index >= EXT2_MAX_DATA_BLOCKS) {
                return false;
        }
        if (index < EXT2_DIRECT_BLOCKS) {
                *block_number = inode->direct_blocks[index];
                return true;
        }

        *block_number = 0U;
        if (inode->indirect_block == 0U) {
                return true;
        }

        static uint8_t pointers[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(inode->indirect_block, pointers)) {
                return false;
        }
        size_t indirect_index = index - EXT2_DIRECT_BLOCKS;
        *block_number =
            read_u32(&pointers[indirect_index * sizeof(uint32_t)]);
        return true;
}

/* Allocate a missing data block and, when necessary, its singly indirect
 * pointer block. allocate_block zeroes both kinds of block before exposure. */
static bool inode_ensure_data_block(struct ext2_filesystem *fs,
                                    struct ext2_inode *inode, size_t index,
                                    uint32_t *block_number) {
        if (!inode_data_block(inode, index, block_number)) {
                return false;
        }
        if (*block_number != 0U) {
                return true;
        }

        if (index < EXT2_DIRECT_BLOCKS) {
                uint32_t allocated = allocate_block(fs);
                if (allocated == 0U) {
                        return false;
                }
                inode->direct_blocks[index] = allocated;
                inode->block_count += EXT2_SECTORS_PER_BLOCK;
                *block_number = allocated;
                return true;
        }

        bool created_indirect = false;
        if (inode->indirect_block == 0U) {
                inode->indirect_block = allocate_block(fs);
                if (inode->indirect_block == 0U) {
                        return false;
                }
                inode->block_count += EXT2_SECTORS_PER_BLOCK;
                created_indirect = true;
        }

        static uint8_t pointers[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(inode->indirect_block, pointers)) {
                goto fail;
        }
        size_t indirect_index = index - EXT2_DIRECT_BLOCKS;
        uint32_t allocated = allocate_block(fs);
        if (allocated == 0U) {
                goto fail;
        }
        write_u32(&pointers[indirect_index * sizeof(uint32_t)], allocated);
        if (!block_cache_write(inode->indirect_block, pointers)) {
                (void)release_block(fs, allocated);
                goto fail;
        }

        inode->block_count += EXT2_SECTORS_PER_BLOCK;
        *block_number = allocated;
        return true;

fail:
        if (created_indirect && release_block(fs, inode->indirect_block)) {
                inode->indirect_block = 0U;
                inode->block_count -= EXT2_SECTORS_PER_BLOCK;
        }
        return false;
}

static uint32_t allocate_inode(struct ext2_filesystem *fs) {
        static uint8_t bitmap[BLOCK_CACHE_BLOCK_SIZE];
        if (fs->free_inodes == 0U ||
            !block_cache_read(fs->inode_bitmap, bitmap)) {
                return 0U;
        }

        for (uint32_t number = fs->first_inode; number <= fs->inodes_count;
             number++) {
                uint32_t bit = number - 1U;
                if (bitmap_test(bitmap, bit)) {
                        continue;
                }
                bitmap_set(bitmap, bit);
                if (!block_cache_write(fs->inode_bitmap, bitmap)) {
                        return 0U;
                }
                fs->free_inodes--;
                if (!write_free_counts(fs)) {
                        return 0U;
                }
                return number;
        }
        return 0U;
}

static bool release_inode(struct ext2_filesystem *fs, uint32_t number) {
        if (number < fs->first_inode || number > fs->inodes_count) {
                return false;
        }
        static uint8_t bitmap[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_read(fs->inode_bitmap, bitmap)) {
                return false;
        }
        uint32_t bit = number - 1U;
        if (!bitmap_test(bitmap, bit)) {
                return false;
        }
        bitmap_clear(bitmap, bit);
        if (!block_cache_write(fs->inode_bitmap, bitmap)) {
                return false;
        }
        fs->free_inodes++;
        return write_free_counts(fs);
}

static int lookup_child(struct ext2_filesystem *fs, uint32_t directory_number,
                        const char *name, size_t name_length,
                        uint32_t *child_number) {
        struct ext2_inode directory;
        if (!read_inode(fs, directory_number, &directory)) {
                return -VFS_ERROR_IO;
        }
        if (mode_type(directory.mode) != VFS_NODE_DIRECTORY) {
                return -VFS_ERROR_NOT_DIRECTORY;
        }

        uint64_t position = 0U;
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        while (position < directory.size) {
                size_t block_index = (size_t)(position / BLOCK_CACHE_BLOCK_SIZE);
                size_t within = (size_t)(position % BLOCK_CACHE_BLOCK_SIZE);
                uint32_t block_number;
                if (!inode_data_block(&directory, block_index, &block_number) ||
                    block_number == 0U ||
                    !block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }

                uint32_t inode = read_u32(&block[within]);
                uint16_t record_length = read_u16(&block[within + 4U]);
                uint8_t disk_name_length = block[within + 6U];
                if (record_length < EXT2_DIRECTORY_HEADER_SIZE ||
                    (record_length & 3U) != 0U ||
                    within + record_length > BLOCK_CACHE_BLOCK_SIZE ||
                    disk_name_length > record_length -
                                           EXT2_DIRECTORY_HEADER_SIZE) {
                        return -VFS_ERROR_IO;
                }
                if (inode != 0U && names_equal(
                                      &block[within +
                                             EXT2_DIRECTORY_HEADER_SIZE],
                                      disk_name_length, name, name_length)) {
                        *child_number = inode;
                        return 0;
                }
                position += record_length;
        }
        return -VFS_ERROR_NO_ENTRY;
}

static int resolve_path(struct ext2_filesystem *fs, const char *path,
                        uint32_t *inode_number) {
        if (path == NULL || path[0] != '/' || inode_number == NULL) {
                return -VFS_ERROR_INVALID;
        }
        if (path[1] == '\0') {
                *inode_number = EXT2_ROOT_INODE;
                return 0;
        }

        uint32_t current = EXT2_ROOT_INODE;
        const char *cursor = &path[1];
        while (*cursor != '\0') {
                size_t length = 0U;
                while (cursor[length] != '\0' && cursor[length] != '/') {
                        length++;
                }
                if (length == 0U || length >= VFS_DIRECTORY_NAME_MAX ||
                    (length == 1U && cursor[0] == '.') ||
                    (length == 2U && cursor[0] == '.' && cursor[1] == '.')) {
                        return -VFS_ERROR_INVALID;
                }
                int result = lookup_child(fs, current, cursor, length, &current);
                if (result != 0) {
                        return result;
                }
                cursor += length;
                if (*cursor == '/') {
                        cursor++;
                        if (*cursor == '\0') {
                                return -VFS_ERROR_INVALID;
                        }
                }
        }
        *inode_number = current;
        return 0;
}

static int resolve_parent(struct ext2_filesystem *fs, const char *path,
                          uint32_t *parent, const char **name,
                          size_t *name_length) {
        size_t length = string_length_bounded(path, EXT2_PATH_MAX);
        if (length < 2U || length == EXT2_PATH_MAX || path[0] != '/' ||
            path[length - 1U] == '/') {
                return -VFS_ERROR_INVALID;
        }

        size_t slash = length;
        while (slash != 0U && path[slash - 1U] != '/') {
                slash--;
        }
        *name = &path[slash];
        *name_length = length - slash;
        if (*name_length == 0U || *name_length >= VFS_DIRECTORY_NAME_MAX ||
            (*name_length == 1U && (*name)[0] == '.') ||
            (*name_length == 2U && (*name)[0] == '.' && (*name)[1] == '.')) {
                return -VFS_ERROR_INVALID;
        }
        if (slash == 1U) {
                *parent = EXT2_ROOT_INODE;
                return 0;
        }

        char parent_path[EXT2_PATH_MAX];
        if (slash > sizeof(parent_path)) {
                return -VFS_ERROR_INVALID;
        }
        copy_bytes(parent_path, path, slash - 1U);
        parent_path[slash - 1U] = '\0';
        return resolve_path(fs, parent_path, parent);
}

static int add_directory_entry(struct ext2_filesystem *fs,
                               uint32_t directory_number, const char *name,
                               size_t name_length, uint32_t child_number,
                               enum vfs_node_type child_type) {
        struct ext2_inode directory;
        if (!read_inode(fs, directory_number, &directory) ||
            mode_type(directory.mode) != VFS_NODE_DIRECTORY) {
                return -VFS_ERROR_NOT_DIRECTORY;
        }
        size_t needed = directory_record_size(name_length);
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];

        for (size_t block_index = 0U; block_index < EXT2_MAX_DATA_BLOCKS;
             block_index++) {
                uint32_t block_number;
                if (!inode_data_block(&directory, block_index,
                                      &block_number)) {
                        return -VFS_ERROR_IO;
                }
                if (block_number == 0U) {
                        if (!inode_ensure_data_block(
                                fs, &directory, block_index, &block_number)) {
                                return -VFS_ERROR_NO_SPACE;
                        }
                        directory.size += BLOCK_CACHE_BLOCK_SIZE;
                        zero_bytes(block, sizeof(block));
                        write_u32(&block[0], child_number);
                        write_u16(&block[4], BLOCK_CACHE_BLOCK_SIZE);
                        block[6] = (uint8_t)name_length;
                        block[7] = directory_type(child_type);
                        copy_bytes(&block[8], name, name_length);
                        if (!block_cache_write(block_number, block) ||
                            !write_inode(fs, directory_number, &directory)) {
                                return -VFS_ERROR_IO;
                        }
                        return 0;
                }

                if (!block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                size_t within = 0U;
                while (within < BLOCK_CACHE_BLOCK_SIZE) {
                        uint16_t record_length = read_u16(&block[within + 4U]);
                        uint8_t existing_name_length = block[within + 6U];
                        if (record_length < EXT2_DIRECTORY_HEADER_SIZE ||
                            within + record_length > BLOCK_CACHE_BLOCK_SIZE) {
                                return -VFS_ERROR_IO;
                        }
                        if (read_u32(&block[within]) == 0U &&
                            record_length >= needed) {
                                write_u32(&block[within], child_number);
                                block[within + 6U] = (uint8_t)name_length;
                                block[within + 7U] = directory_type(child_type);
                                copy_bytes(&block[within + 8U], name,
                                           name_length);
                                return block_cache_write(block_number, block)
                                           ? 0
                                           : -VFS_ERROR_IO;
                        }
                        size_t actual =
                            directory_record_size(existing_name_length);
                        if (read_u32(&block[within]) != 0U &&
                            record_length >= actual + needed) {
                                write_u16(&block[within + 4U], (uint16_t)actual);
                                size_t inserted = within + actual;
                                write_u32(&block[inserted], child_number);
                                write_u16(&block[inserted + 4U],
                                          (uint16_t)(record_length - actual));
                                block[inserted + 6U] = (uint8_t)name_length;
                                block[inserted + 7U] = directory_type(child_type);
                                copy_bytes(&block[inserted + 8U], name,
                                           name_length);
                                return block_cache_write(block_number, block)
                                           ? 0
                                           : -VFS_ERROR_IO;
                        }
                        within += record_length;
                }
        }
        return -VFS_ERROR_NO_SPACE;
}

static int remove_directory_entry(struct ext2_filesystem *fs,
                                  uint32_t directory_number, const char *name,
                                  size_t name_length) {
        struct ext2_inode directory;
        if (!read_inode(fs, directory_number, &directory)) {
                return -VFS_ERROR_IO;
        }
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        for (size_t block_index = 0U;
             block_index < EXT2_MAX_DATA_BLOCKS &&
             block_index * BLOCK_CACHE_BLOCK_SIZE < directory.size;
             block_index++) {
                uint32_t block_number;
                if (!inode_data_block(&directory, block_index, &block_number) ||
                    block_number == 0U ||
                    !block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                size_t within = 0U;
                size_t previous = SIZE_MAX;
                while (within < BLOCK_CACHE_BLOCK_SIZE) {
                        uint32_t inode = read_u32(&block[within]);
                        uint16_t record_length = read_u16(&block[within + 4U]);
                        uint8_t disk_name_length = block[within + 6U];
                        if (record_length < EXT2_DIRECTORY_HEADER_SIZE ||
                            within + record_length > BLOCK_CACHE_BLOCK_SIZE) {
                                return -VFS_ERROR_IO;
                        }
                        if (inode != 0U && names_equal(
                                              &block[within + 8U],
                                              disk_name_length, name,
                                              name_length)) {
                                if (previous != SIZE_MAX) {
                                        uint16_t previous_length =
                                            read_u16(&block[previous + 4U]);
                                        write_u16(&block[previous + 4U],
                                                  (uint16_t)(previous_length +
                                                             record_length));
                                } else {
                                        write_u32(&block[within], 0U);
                                }
                                return block_cache_write(block_number, block)
                                           ? 0
                                           : -VFS_ERROR_IO;
                        }
                        if (inode != 0U) {
                                previous = within;
                        }
                        within += record_length;
                }
        }
        return -VFS_ERROR_NO_ENTRY;
}

static int truncate_inode(struct ext2_filesystem *fs, uint32_t number,
                          struct ext2_inode *inode) {
        for (size_t index = 0U; index < EXT2_DIRECT_BLOCKS; index++) {
                if (inode->direct_blocks[index] != 0U) {
                        if (!release_block(fs, inode->direct_blocks[index])) {
                                return -VFS_ERROR_IO;
                        }
                        inode->direct_blocks[index] = 0U;
                }
        }
        if (inode->indirect_block != 0U) {
                static uint8_t pointers[BLOCK_CACHE_BLOCK_SIZE];
                if (!block_cache_read(inode->indirect_block, pointers)) {
                        return -VFS_ERROR_IO;
                }
                for (size_t index = 0U; index < EXT2_INDIRECT_BLOCK_ENTRIES;
                     index++) {
                        uint32_t block_number =
                            read_u32(&pointers[index * sizeof(uint32_t)]);
                        if (block_number != 0U &&
                            !release_block(fs, block_number)) {
                                return -VFS_ERROR_IO;
                        }
                }
                if (!release_block(fs, inode->indirect_block)) {
                        return -VFS_ERROR_IO;
                }
                inode->indirect_block = 0U;
        }
        inode->size = 0U;
        inode->block_count = 0U;
        return write_inode(fs, number, inode) ? 0 : -VFS_ERROR_IO;
}

static int create_regular(struct ext2_filesystem *fs, const char *path,
                          uint32_t *created_number) {
        uint32_t parent;
        const char *name;
        size_t name_length;
        int result = resolve_parent(fs, path, &parent, &name, &name_length);
        if (result != 0) {
                return result;
        }
        uint32_t existing;
        result = lookup_child(fs, parent, name, name_length, &existing);
        if (result == 0) {
                return -VFS_ERROR_EXISTS;
        }
        if (result != -VFS_ERROR_NO_ENTRY) {
                return result;
        }

        uint32_t number = allocate_inode(fs);
        if (number == 0U) {
                return -VFS_ERROR_NO_SPACE;
        }
        struct ext2_inode inode = {.mode = EXT2_MODE_REGULAR | 0644U,
                                   .link_count = 1U};
        if (!write_inode(fs, number, &inode)) {
                (void)release_inode(fs, number);
                return -VFS_ERROR_IO;
        }
        result = add_directory_entry(fs, parent, name, name_length, number,
                                     VFS_NODE_REGULAR);
        if (result != 0) {
                (void)release_inode(fs, number);
                return result;
        }
        *created_number = number;
        return 0;
}

static int ext2_open_file(void *context, const char *path, uint32_t flags,
                          struct vfs_file *file) {
        struct ext2_filesystem *fs = context;
        const uint32_t valid_flags = VFS_OPEN_READ | VFS_OPEN_WRITE |
                                     VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE |
                                     VFS_OPEN_DIRECTORY;
        if ((flags & ~valid_flags) != 0U ||
            (flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U ||
            ((flags & VFS_OPEN_TRUNCATE) != 0U &&
             (flags & VFS_OPEN_WRITE) == 0U)) {
                return -VFS_ERROR_INVALID;
        }

        uint32_t number;
        int result = resolve_path(fs, path, &number);
        if (result == -VFS_ERROR_NO_ENTRY &&
            (flags & VFS_OPEN_CREATE) != 0U) {
                result = create_regular(fs, path, &number);
        }
        if (result != 0) {
                return result;
        }

        struct ext2_inode inode;
        if (!read_inode(fs, number, &inode)) {
                return -VFS_ERROR_IO;
        }
        enum vfs_node_type type = mode_type(inode.mode);
        if (((flags & VFS_OPEN_DIRECTORY) != 0U) !=
            (type == VFS_NODE_DIRECTORY)) {
                return type == VFS_NODE_DIRECTORY ? -VFS_ERROR_IS_DIRECTORY
                                                  : -VFS_ERROR_NOT_DIRECTORY;
        }
        if ((flags & VFS_OPEN_TRUNCATE) != 0U) {
                result = truncate_inode(fs, number, &inode);
                if (result != 0) {
                        return result;
                }
        }
        *file = (struct vfs_file){.type = type,
                                  .size = inode.size,
                                  .inode = number};
        return 0;
}

static long ext2_read_file(void *context, struct vfs_file *file,
                           uint64_t offset, void *buffer, size_t length) {
        struct ext2_filesystem *fs = context;
        struct ext2_inode inode;
        if (!read_inode(fs, file->inode, &inode)) {
                return -VFS_ERROR_IO;
        }
        if (offset >= inode.size) {
                return 0;
        }
        if (length > inode.size - offset) {
                length = (size_t)(inode.size - offset);
        }

        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        uint8_t *output = buffer;
        size_t remaining = length;
        while (remaining != 0U) {
                size_t block_index = (size_t)(offset / BLOCK_CACHE_BLOCK_SIZE);
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                size_t count = BLOCK_CACHE_BLOCK_SIZE - within;
                if (count > remaining) {
                        count = remaining;
                }
                uint32_t block_number;
                if (!inode_data_block(&inode, block_index, &block_number) ||
                    block_number == 0U ||
                    !block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                copy_bytes(output, &block[within], count);
                output += count;
                offset += count;
                remaining -= count;
        }
        return (long)length;
}

static long ext2_write_file(void *context, struct vfs_file *file,
                            uint64_t offset, const void *buffer,
                            size_t length) {
        struct ext2_filesystem *fs = context;
        const uint64_t maximum_size =
            (uint64_t)EXT2_MAX_DATA_BLOCKS * BLOCK_CACHE_BLOCK_SIZE;
        if (offset > maximum_size || length > maximum_size - offset) {
                return -VFS_ERROR_NO_SPACE;
        }
        struct ext2_inode inode;
        if (!read_inode(fs, file->inode, &inode) ||
            mode_type(inode.mode) != VFS_NODE_REGULAR) {
                return -VFS_ERROR_IO;
        }

        const uint8_t *input = buffer;
        size_t remaining = length;
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        while (remaining != 0U) {
                size_t block_index = (size_t)(offset / BLOCK_CACHE_BLOCK_SIZE);
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                size_t count = BLOCK_CACHE_BLOCK_SIZE - within;
                if (count > remaining) {
                        count = remaining;
                }
                uint32_t block_number;
                if (!inode_data_block(&inode, block_index, &block_number)) {
                        return -VFS_ERROR_IO;
                }
                if (block_number == 0U) {
                        if (!inode_ensure_data_block(fs, &inode, block_index,
                                                     &block_number)) {
                                return -VFS_ERROR_NO_SPACE;
                        }
                        zero_bytes(block, sizeof(block));
                } else if (!block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                copy_bytes(&block[within], input, count);
                if (!block_cache_write(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                input += count;
                offset += count;
                remaining -= count;
        }
        if (offset > inode.size) {
                inode.size = (uint32_t)offset;
        }
        if (!write_inode(fs, file->inode, &inode)) {
                return -VFS_ERROR_IO;
        }
        file->size = inode.size;
        return (long)length;
}

static int ext2_stat_file(void *context, struct vfs_file *file,
                          struct vfs_stat *status) {
        struct ext2_inode inode;
        if (!read_inode(context, file->inode, &inode)) {
                return -VFS_ERROR_IO;
        }
        *status = (struct vfs_stat){.inode = file->inode,
                                    .mode = inode.mode,
                                    .size = inode.size,
                                    .type = mode_type(inode.mode)};
        file->size = inode.size;
        return 0;
}

static long ext2_read_directory(void *context, struct vfs_file *file,
                                uint64_t offset,
                                struct vfs_directory_entry *entry) {
        struct ext2_filesystem *fs = context;
        struct ext2_inode directory;
        if (!read_inode(fs, file->inode, &directory)) {
                return -VFS_ERROR_IO;
        }
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        while (offset < directory.size) {
                size_t block_index = (size_t)(offset / BLOCK_CACHE_BLOCK_SIZE);
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                uint32_t block_number;
                if (!inode_data_block(&directory, block_index, &block_number) ||
                    block_number == 0U ||
                    !block_cache_read(block_number, block)) {
                        return -VFS_ERROR_IO;
                }
                uint32_t inode = read_u32(&block[within]);
                uint16_t record_length = read_u16(&block[within + 4U]);
                uint8_t name_length = block[within + 6U];
                uint8_t type = block[within + 7U];
                if (record_length < EXT2_DIRECTORY_HEADER_SIZE ||
                    within + record_length > BLOCK_CACHE_BLOCK_SIZE ||
                    name_length >= VFS_DIRECTORY_NAME_MAX ||
                    name_length > record_length - EXT2_DIRECTORY_HEADER_SIZE) {
                        return -VFS_ERROR_IO;
                }
                offset += record_length;
                if (inode == 0U) {
                        continue;
                }
                entry->inode = inode;
                entry->type = type == EXT2_DIRECTORY_TYPE_DIRECTORY
                                  ? VFS_NODE_DIRECTORY
                                  : VFS_NODE_REGULAR;
                copy_bytes(entry->name, &block[within + 8U], name_length);
                entry->name[name_length] = '\0';
                return (long)offset;
        }
        return 0;
}

static int ext2_make_directory(void *context, const char *path) {
        struct ext2_filesystem *fs = context;
        uint32_t parent;
        const char *name;
        size_t name_length;
        int result = resolve_parent(fs, path, &parent, &name, &name_length);
        if (result != 0) {
                return result;
        }
        uint32_t existing;
        result = lookup_child(fs, parent, name, name_length, &existing);
        if (result == 0) {
                return -VFS_ERROR_EXISTS;
        }
        if (result != -VFS_ERROR_NO_ENTRY) {
                return result;
        }

        uint32_t number = allocate_inode(fs);
        uint32_t block_number = allocate_block(fs);
        if (number == 0U || block_number == 0U) {
                if (number != 0U) {
                        (void)release_inode(fs, number);
                }
                return -VFS_ERROR_NO_SPACE;
        }
        struct ext2_inode inode = {
            .mode = EXT2_MODE_DIRECTORY | 0755U,
            .link_count = 2U,
            .size = BLOCK_CACHE_BLOCK_SIZE,
            .block_count = EXT2_SECTORS_PER_BLOCK,
            .direct_blocks = {block_number},
        };
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        zero_bytes(block, sizeof(block));
        write_u32(&block[0], number);
        write_u16(&block[4], 12U);
        block[6] = 1U;
        block[7] = EXT2_DIRECTORY_TYPE_DIRECTORY;
        block[8] = '.';
        write_u32(&block[12], parent);
        write_u16(&block[16], BLOCK_CACHE_BLOCK_SIZE - 12U);
        block[18] = 2U;
        block[19] = EXT2_DIRECTORY_TYPE_DIRECTORY;
        block[20] = '.';
        block[21] = '.';
        if (!block_cache_write(block_number, block) ||
            !write_inode(fs, number, &inode)) {
                return -VFS_ERROR_IO;
        }
        result = add_directory_entry(fs, parent, name, name_length, number,
                                     VFS_NODE_DIRECTORY);
        if (result != 0) {
                return result;
        }
        struct ext2_inode parent_inode;
        if (!read_inode(fs, parent, &parent_inode)) {
                return -VFS_ERROR_IO;
        }
        parent_inode.link_count++;
        fs->used_directories++;
        return write_inode(fs, parent, &parent_inode) && write_free_counts(fs)
                   ? 0
                   : -VFS_ERROR_IO;
}

static bool directory_is_empty(const struct ext2_inode *directory) {
        static uint8_t block[BLOCK_CACHE_BLOCK_SIZE];
        uint64_t offset = 0U;
        while (offset < directory->size) {
                size_t block_index = (size_t)(offset / BLOCK_CACHE_BLOCK_SIZE);
                size_t within = (size_t)(offset % BLOCK_CACHE_BLOCK_SIZE);
                uint32_t block_number;
                if (!inode_data_block(directory, block_index, &block_number) ||
                    block_number == 0U ||
                    !block_cache_read(block_number, block)) {
                        return false;
                }
                uint32_t inode = read_u32(&block[within]);
                uint16_t record_length = read_u16(&block[within + 4U]);
                uint8_t name_length = block[within + 6U];
                if (record_length < EXT2_DIRECTORY_HEADER_SIZE ||
                    within + record_length > BLOCK_CACHE_BLOCK_SIZE) {
                        return false;
                }
                if (inode != 0U &&
                    !((name_length == 1U && block[within + 8U] == '.') ||
                      (name_length == 2U && block[within + 8U] == '.' &&
                       block[within + 9U] == '.'))) {
                        return false;
                }
                offset += record_length;
        }
        return true;
}

static int ext2_unlink_path(void *context, const char *path) {
        struct ext2_filesystem *fs = context;
        uint32_t parent;
        const char *name;
        size_t name_length;
        int result = resolve_parent(fs, path, &parent, &name, &name_length);
        if (result != 0) {
                return result;
        }
        uint32_t number;
        result = lookup_child(fs, parent, name, name_length, &number);
        if (result != 0) {
                return result;
        }
        struct ext2_inode inode;
        if (!read_inode(fs, number, &inode)) {
                return -VFS_ERROR_IO;
        }
        bool is_directory = mode_type(inode.mode) == VFS_NODE_DIRECTORY;
        if (is_directory && !directory_is_empty(&inode)) {
                return -VFS_ERROR_NOT_EMPTY;
        }
        result = remove_directory_entry(fs, parent, name, name_length);
        if (result != 0) {
                return result;
        }
        result = truncate_inode(fs, number, &inode);
        if (result != 0) {
                return result;
        }
        inode.mode = 0U;
        inode.link_count = 0U;
        if (!write_inode(fs, number, &inode) || !release_inode(fs, number)) {
                return -VFS_ERROR_IO;
        }
        if (is_directory) {
                struct ext2_inode parent_inode;
                if (!read_inode(fs, parent, &parent_inode)) {
                        return -VFS_ERROR_IO;
                }
                parent_inode.link_count--;
                fs->used_directories--;
                if (!write_inode(fs, parent, &parent_inode) ||
                    !write_free_counts(fs)) {
                        return -VFS_ERROR_IO;
                }
        }
        return 0;
}

static const struct vfs_filesystem_operations ext2_operations = {
    .open = ext2_open_file,
    .read = ext2_read_file,
    .write = ext2_write_file,
    .stat = ext2_stat_file,
    .read_directory = ext2_read_directory,
    .make_directory = ext2_make_directory,
    .unlink = ext2_unlink_path,
};

bool ext2_mount(struct block_device *device) {
        static uint8_t superblock[BLOCK_CACHE_BLOCK_SIZE];
        static uint8_t group[BLOCK_CACHE_BLOCK_SIZE];
        if (!block_cache_init(device) || !block_cache_read(1U, superblock) ||
            read_u16(&superblock[SUPER_MAGIC]) != EXT2_MAGIC ||
            read_u32(&superblock[SUPER_LOG_BLOCK_SIZE]) != 0U ||
            read_u32(&superblock[SUPER_REVISION]) == 0U ||
            read_u16(&superblock[SUPER_INODE_SIZE]) != EXT2_INODE_SIZE ||
            (read_u32(&superblock[SUPER_FEATURE_INCOMPAT]) &
             ~EXT2_FEATURE_INCOMPAT_FILETYPE) != 0U ||
            !block_cache_read(2U, group)) {
                return false;
        }

        filesystem = (struct ext2_filesystem){
            .blocks_count = read_u32(&superblock[SUPER_BLOCKS_COUNT]),
            .inodes_count = read_u32(&superblock[SUPER_INODES_COUNT]),
            .first_data_block =
                read_u32(&superblock[SUPER_FIRST_DATA_BLOCK]),
            .blocks_per_group =
                read_u32(&superblock[SUPER_BLOCKS_PER_GROUP]),
            .inodes_per_group =
                read_u32(&superblock[SUPER_INODES_PER_GROUP]),
            .first_inode = read_u32(&superblock[SUPER_FIRST_INODE]),
            .block_bitmap = read_u32(&group[GROUP_BLOCK_BITMAP]),
            .inode_bitmap = read_u32(&group[GROUP_INODE_BITMAP]),
            .inode_table = read_u32(&group[GROUP_INODE_TABLE]),
            .free_blocks = read_u16(&group[GROUP_FREE_BLOCKS_COUNT]),
            .free_inodes = read_u16(&group[GROUP_FREE_INODES_COUNT]),
            .used_directories =
                read_u16(&group[GROUP_USED_DIRECTORIES_COUNT]),
        };

        if (filesystem.blocks_count > block_cache_block_count() ||
            filesystem.blocks_count > filesystem.blocks_per_group ||
            filesystem.inodes_count == 0U ||
            filesystem.inodes_count > filesystem.inodes_per_group ||
            filesystem.first_data_block != 1U ||
            filesystem.first_inode < 11U ||
            filesystem.block_bitmap >= filesystem.blocks_count ||
            filesystem.inode_bitmap >= filesystem.blocks_count ||
            filesystem.inode_table >= filesystem.blocks_count) {
                return false;
        }

        struct ext2_inode root;
        return read_inode(&filesystem, EXT2_ROOT_INODE, &root) &&
               mode_type(root.mode) == VFS_NODE_DIRECTORY &&
               vfs_mount_filesystem(&filesystem, &ext2_operations);
}
