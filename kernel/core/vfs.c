/* Mount routing and common file operations for ramfs, ext2, and devices. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uart.h"
#include "vfs.h"

static const struct vfs_node *fallback_root;
static void *root_context;
static const struct vfs_filesystem_operations *root_operations;

static const struct vfs_character_device_operations console_operations = {
    .read_byte = uart_getc,
    .write_byte = uart_tx_submit,
};

static void copy_bytes(void *destination, const void *source, size_t length) {
        uint8_t *output = destination;
        const uint8_t *input = source;

        for (size_t index = 0U; index < length; index++) {
                output[index] = input[index];
        }
}

static bool strings_equal(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }
        return *left == *right;
}

static bool segment_matches(const char *name, const char *segment,
                            size_t segment_length) {
        size_t index = 0U;
        while (index < segment_length && name[index] != '\0' &&
               name[index] == segment[index]) {
                index++;
        }
        return index == segment_length && name[index] == '\0';
}

bool vfs_mount_root(const struct vfs_node *root) {
        if (root == NULL || root->type != VFS_NODE_DIRECTORY ||
            root->name == NULL || root->name[0] != '\0' ||
            (root->child_count != 0U && root->children == NULL)) {
                return false;
        }
        fallback_root = root;
        return true;
}

bool vfs_mount_filesystem(void *context,
                          const struct vfs_filesystem_operations *operations) {
        if (context == NULL || operations == NULL || operations->open == NULL ||
            operations->read == NULL || operations->stat == NULL) {
                return false;
        }
        root_context = context;
        root_operations = operations;
        return true;
}

bool vfs_uses_disk_root(void) { return root_operations != NULL; }

static const struct vfs_node *fallback_lookup(const char *path) {
        if (fallback_root == NULL || path == NULL || path[0] != '/') {
                return NULL;
        }
        if (path[1] == '\0') {
                return fallback_root;
        }

        const struct vfs_node *node = fallback_root;
        const char *cursor = &path[1];
        while (*cursor != '\0') {
                const char *segment = cursor;
                size_t length = 0U;
                while (cursor[length] != '\0' && cursor[length] != '/') {
                        length++;
                }
                if (length == 0U ||
                    (length == 1U && segment[0] == '.') ||
                    (length == 2U && segment[0] == '.' && segment[1] == '.') ||
                    node->type != VFS_NODE_DIRECTORY) {
                        return NULL;
                }

                const struct vfs_node *child = NULL;
                for (size_t index = 0U; index < node->child_count; index++) {
                        if (segment_matches(node->children[index].name, segment,
                                            length)) {
                                child = &node->children[index];
                                break;
                        }
                }
                if (child == NULL) {
                        return NULL;
                }
                node = child;
                cursor += length;
                if (*cursor == '/') {
                        cursor++;
                        if (*cursor == '\0') {
                                return NULL;
                        }
                }
        }
        return node;
}

static int open_console(uint32_t flags, struct vfs_file *file) {
        if ((flags & (VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE |
                      VFS_OPEN_DIRECTORY)) != 0U ||
            (flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U) {
                return -VFS_ERROR_INVALID;
        }
        *file = (struct vfs_file){.type = VFS_NODE_CHARACTER_DEVICE,
                                  .device = VFS_DEVICE_CONSOLE,
                                  .operations = &console_operations};
        return 0;
}

int vfs_open(const char *path, uint32_t flags, struct vfs_file *file) {
        if (path == NULL || file == NULL || path[0] != '/') {
                return -VFS_ERROR_INVALID;
        }
        if (strings_equal(path, "/dev/console")) {
                return open_console(flags, file);
        }
        if (root_operations != NULL) {
                int result = root_operations->open(root_context, path, flags,
                                                   file);
                if (result == 0) {
                        file->filesystem_context = root_context;
                        file->filesystem_operations = root_operations;
                }
                return result;
        }

        const struct vfs_node *node = fallback_lookup(path);
        if (node == NULL) {
                return -VFS_ERROR_NO_ENTRY;
        }
        if ((flags & (VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                      VFS_OPEN_TRUNCATE)) != 0U) {
                return -VFS_ERROR_READ_ONLY;
        }
        if (((flags & VFS_OPEN_DIRECTORY) != 0U) !=
            (node->type == VFS_NODE_DIRECTORY)) {
                return node->type == VFS_NODE_DIRECTORY
                           ? -VFS_ERROR_IS_DIRECTORY
                           : -VFS_ERROR_NOT_DIRECTORY;
        }
        *file = (struct vfs_file){.type = node->type,
                                  .size = node->size,
                                  .data = node->data,
                                  .device = node->device,
                                  .operations = node->operations,
                                  .fallback_node = node};
        return 0;
}

long vfs_read(struct vfs_file *file, uint64_t offset, void *buffer,
              size_t length) {
        if (file == NULL || (buffer == NULL && length != 0U)) {
                return -VFS_ERROR_INVALID;
        }
        if (file->type == VFS_NODE_DIRECTORY) {
                return -VFS_ERROR_IS_DIRECTORY;
        }
        if (file->type != VFS_NODE_REGULAR) {
                return -VFS_ERROR_BAD_DESCRIPTOR;
        }
        if (file->filesystem_operations != NULL) {
                return file->filesystem_operations->read(
                    file->filesystem_context, file, offset, buffer, length);
        }
        if (offset >= file->size) {
                return 0;
        }
        size_t count = length;
        if (count > file->size - offset) {
                count = (size_t)(file->size - offset);
        }
        copy_bytes(buffer, &file->data[offset], count);
        return (long)count;
}

long vfs_write(struct vfs_file *file, uint64_t offset, const void *buffer,
               size_t length) {
        if (file == NULL || (buffer == NULL && length != 0U)) {
                return -VFS_ERROR_INVALID;
        }
        if (file->filesystem_operations == NULL ||
            file->filesystem_operations->write == NULL) {
                return -VFS_ERROR_READ_ONLY;
        }
        return file->filesystem_operations->write(file->filesystem_context,
                                                  file, offset, buffer,
                                                  length);
}

int vfs_stat_file(struct vfs_file *file, struct vfs_stat *status) {
        if (file == NULL || status == NULL) {
                return -VFS_ERROR_INVALID;
        }
        if (file->filesystem_operations != NULL) {
                return file->filesystem_operations->stat(
                    file->filesystem_context, file, status);
        }
        *status = (struct vfs_stat){.inode = file->inode,
                                    .size = file->size,
                                    .type = file->type};
        return 0;
}

int vfs_stat_path(const char *path, struct vfs_stat *status) {
        struct vfs_file file;
        int result = vfs_open(path, VFS_OPEN_READ, &file);
        if (result == -VFS_ERROR_IS_DIRECTORY) {
                result = vfs_open(path, VFS_OPEN_READ | VFS_OPEN_DIRECTORY,
                                  &file);
        }
        return result == 0 ? vfs_stat_file(&file, status) : result;
}

long vfs_read_directory(struct vfs_file *directory, uint64_t offset,
                        struct vfs_directory_entry *entry) {
        if (directory == NULL || entry == NULL ||
            directory->type != VFS_NODE_DIRECTORY) {
                return -VFS_ERROR_NOT_DIRECTORY;
        }
        if (directory->filesystem_operations == NULL ||
            directory->filesystem_operations->read_directory == NULL) {
                const struct vfs_node *node = directory->fallback_node;
                if (node == NULL || offset >= node->child_count) {
                        return 0;
                }

                const struct vfs_node *child = &node->children[offset];
                entry->inode = 0U;
                entry->type = child->type;
                size_t index = 0U;
                while (child->name[index] != '\0' &&
                       index + 1U < sizeof(entry->name)) {
                        entry->name[index] = child->name[index];
                        index++;
                }
                entry->name[index] = '\0';
                return (long)(offset + 1U);
        }
        return directory->filesystem_operations->read_directory(
            directory->filesystem_context, directory, offset, entry);
}

int vfs_make_directory(const char *path) {
        if (root_operations == NULL || root_operations->make_directory == NULL) {
                return -VFS_ERROR_READ_ONLY;
        }
        return root_operations->make_directory(root_context, path);
}

int vfs_unlink(const char *path) {
        if (root_operations == NULL || root_operations->unlink == NULL) {
                return -VFS_ERROR_READ_ONLY;
        }
        return root_operations->unlink(root_context, path);
}
