#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The first VFS deliberately exposes only directories and immutable files. */
enum vfs_node_type {
        VFS_NODE_DIRECTORY,
        VFS_NODE_REGULAR,
};

struct vfs_node {
        const char *name;
        enum vfs_node_type type;

        /* Regular-file payload. */
        const uint8_t *data;
        size_t size;

        /* Directory payload. */
        const struct vfs_node *children;
        size_t child_count;
};

struct vfs_file {
        const uint8_t *data;
        size_t size;
};

/* Mount one directory as /, then resolve canonical absolute paths below it. */
bool vfs_mount_root(const struct vfs_node *root);
const struct vfs_node *vfs_lookup(const char *path);
bool vfs_open(const char *path, struct vfs_file *file);

#endif
