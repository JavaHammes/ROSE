#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vfs_node_type {
        VFS_NODE_DIRECTORY,
        VFS_NODE_REGULAR,
        VFS_NODE_CHARACTER_DEVICE,
};

enum vfs_device_id {
        VFS_DEVICE_NONE,
        VFS_DEVICE_CONSOLE,
};

enum vfs_open_flags {
        VFS_OPEN_READ = (1U << 0),
        VFS_OPEN_WRITE = (1U << 1),
        VFS_OPEN_CREATE = (1U << 2),
        VFS_OPEN_TRUNCATE = (1U << 3),
        VFS_OPEN_DIRECTORY = (1U << 4),
        VFS_OPEN_APPEND = (1U << 5),
};

/* Values intentionally match the stable user ABI errno numbers. */
enum vfs_error {
        VFS_ERROR_NO_ENTRY = 2,
        VFS_ERROR_IO = 5,
        VFS_ERROR_BAD_DESCRIPTOR = 9,
        VFS_ERROR_PERMISSION = 13,
        VFS_ERROR_EXISTS = 17,
        VFS_ERROR_NOT_DIRECTORY = 20,
        VFS_ERROR_IS_DIRECTORY = 21,
        VFS_ERROR_INVALID = 22,
        VFS_ERROR_NO_SPACE = 28,
        VFS_ERROR_READ_ONLY = 30,
        VFS_ERROR_NOT_EMPTY = 39,
};

enum { VFS_DIRECTORY_NAME_MAX = 56 };

struct vfs_character_device_operations {
        bool (*read_byte)(char *character);
        bool (*write_byte)(char character);
};

/* Static node representation used by the linker-embedded fallback ramfs. */
struct vfs_node {
        const char *name;
        enum vfs_node_type type;
        const uint8_t *data;
        size_t size;
        enum vfs_device_id device;
        const struct vfs_character_device_operations *operations;
        const struct vfs_node *children;
        size_t child_count;
};

struct vfs_file;

struct vfs_stat {
        uint32_t inode;
        uint32_t mode;
        uint64_t size;
        enum vfs_node_type type;
};

struct vfs_directory_entry {
        uint32_t inode;
        enum vfs_node_type type;
        char name[VFS_DIRECTORY_NAME_MAX];
};

struct vfs_filesystem_operations {
        int (*open)(void *context, const char *path, uint32_t flags,
                    struct vfs_file *file);
        long (*read)(void *context, struct vfs_file *file, uint64_t offset,
                     void *buffer, size_t length);
        long (*write)(void *context, struct vfs_file *file, uint64_t offset,
                      const void *buffer, size_t length);
        int (*stat)(void *context, struct vfs_file *file,
                    struct vfs_stat *status);
        long (*read_directory)(void *context, struct vfs_file *directory,
                               uint64_t offset,
                               struct vfs_directory_entry *entry);
        int (*make_directory)(void *context, const char *path);
        int (*unlink)(void *context, const char *path);
};

struct vfs_file {
        enum vfs_node_type type;
        uint64_t size;
        uint32_t inode;
        const uint8_t *data;
        enum vfs_device_id device;
        const struct vfs_character_device_operations *operations;
        void *filesystem_context;
        const struct vfs_filesystem_operations *filesystem_operations;
        const struct vfs_node *fallback_node;
};

/* Mount the immutable fallback first, then optionally replace it with disk. */
bool vfs_mount_root(const struct vfs_node *root);
bool vfs_mount_filesystem(void *context,
                          const struct vfs_filesystem_operations *operations);
bool vfs_uses_disk_root(void);

int vfs_open(const char *path, uint32_t flags, struct vfs_file *file);
long vfs_read(struct vfs_file *file, uint64_t offset, void *buffer,
              size_t length);
long vfs_write(struct vfs_file *file, uint64_t offset, const void *buffer,
               size_t length);
int vfs_stat_file(struct vfs_file *file, struct vfs_stat *status);
int vfs_stat_path(const char *path, struct vfs_stat *status);
long vfs_read_directory(struct vfs_file *directory, uint64_t offset,
                        struct vfs_directory_entry *entry);
int vfs_make_directory(const char *path);
int vfs_unlink(const char *path);

#endif
