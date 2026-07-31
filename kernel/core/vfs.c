/*
 * Minimal mount and pathname layer.
 *
 * Nodes and file bytes are owned by the mounted filesystem. The VFS performs
 * no allocation and returns immutable file views or nonblocking character
 * operations. Descriptor offsets and scheduler waits remain process concerns.
 */
#include <stdbool.h>
#include <stddef.h>

#include "vfs.h"

static const struct vfs_node *vfs_root;

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

        vfs_root = root;
        return true;
}

const struct vfs_node *vfs_lookup(const char *path) {
        if (vfs_root == NULL || path == NULL || path[0] != '/') {
                return NULL;
        }
        if (path[1] == '\0') {
                return vfs_root;
        }

        const struct vfs_node *node = vfs_root;
        const char *cursor = &path[1];

        while (*cursor != '\0') {
                const char *segment = cursor;
                size_t segment_length = 0U;

                while (cursor[segment_length] != '\0' &&
                       cursor[segment_length] != '/') {
                        segment_length++;
                }

                /* Require canonical paths: no //, trailing slash, . or ... */
                if (segment_length == 0U ||
                    (segment_length == 1U && segment[0] == '.') ||
                    (segment_length == 2U && segment[0] == '.' &&
                     segment[1] == '.') ||
                    node->type != VFS_NODE_DIRECTORY) {
                        return NULL;
                }

                const struct vfs_node *child = NULL;

                for (size_t index = 0U; index < node->child_count; index++) {
                        const struct vfs_node *candidate = &node->children[index];

                        if (segment_matches(candidate->name, segment,
                                            segment_length)) {
                                child = candidate;
                                break;
                        }
                }

                if (child == NULL) {
                        return NULL;
                }

                node = child;
                cursor += segment_length;
                if (*cursor == '/') {
                        cursor++;
                        if (*cursor == '\0') {
                                return NULL;
                        }
                }
        }

        return node;
}

bool vfs_open(const char *path, struct vfs_file *file) {
        const struct vfs_node *node = vfs_lookup(path);

        if (node == NULL || node->type == VFS_NODE_DIRECTORY || file == NULL ||
            (node->type == VFS_NODE_REGULAR && node->size != 0U &&
             node->data == NULL) ||
            (node->type == VFS_NODE_CHARACTER_DEVICE &&
             (node->device == VFS_DEVICE_NONE || node->operations == NULL))) {
                return false;
        }

        file->type = node->type;
        file->data = node->data;
        file->size = node->size;
        file->device = node->device;
        file->operations = node->operations;
        return true;
}
