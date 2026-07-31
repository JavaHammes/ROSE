#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The first VFS exposes directories, immutable files, and character devices. */
enum vfs_node_type {
        VFS_NODE_DIRECTORY,
        VFS_NODE_REGULAR,
        VFS_NODE_CHARACTER_DEVICE,
};

enum vfs_device_id {
        VFS_DEVICE_NONE,
        VFS_DEVICE_CONSOLE,
};

/* Character-device operations are nonblocking; callers decide how to wait. */
struct vfs_character_device_operations {
        bool (*read_byte)(char *character);
        bool (*write_byte)(char character);
};

struct vfs_node {
        const char *name;
        enum vfs_node_type type;

        /* Regular-file payload. */
        const uint8_t *data;
        size_t size;

        /* Character-device identity. */
        enum vfs_device_id device;
        const struct vfs_character_device_operations *operations;

        /* Directory payload. */
        const struct vfs_node *children;
        size_t child_count;
};

struct vfs_file {
        enum vfs_node_type type;
        const uint8_t *data;
        size_t size;
        enum vfs_device_id device;
        const struct vfs_character_device_operations *operations;
};

/* Mount one directory as /, then resolve canonical absolute paths below it. */
bool vfs_mount_root(const struct vfs_node *root);
const struct vfs_node *vfs_lookup(const char *path);
bool vfs_open(const char *path, struct vfs_file *file);

#endif
