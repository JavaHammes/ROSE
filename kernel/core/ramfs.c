/* Mount the immutable diagnostic fallback used when no ext2 disk is valid. */
#include <stddef.h>
#include <stdint.h>

#include "panic.h"
#include "ramfs.h"
#include "vfs.h"

extern uint8_t user_hello_elf_start[];
extern uint8_t user_hello_elf_end[];
extern uint8_t user_fault_elf_start[];
extern uint8_t user_fault_elf_end[];
extern uint8_t user_process_a_elf_start[];
extern uint8_t user_process_a_elf_end[];
extern uint8_t user_process_b_elf_start[];
extern uint8_t user_process_b_elf_end[];
extern uint8_t user_syscall_test_elf_start[];
extern uint8_t user_syscall_test_elf_end[];
extern uint8_t user_cat_elf_start[];
extern uint8_t user_cat_elf_end[];
extern uint8_t user_console_read_elf_start[];
extern uint8_t user_console_read_elf_end[];
extern uint8_t user_sh_elf_start[];
extern uint8_t user_sh_elf_end[];
extern uint8_t user_ls_elf_start[];
extern uint8_t user_ls_elf_end[];
extern uint8_t user_echo_elf_start[];
extern uint8_t user_echo_elf_end[];
extern uint8_t user_pwd_elf_start[];
extern uint8_t user_pwd_elf_end[];
extern uint8_t user_env_elf_start[];
extern uint8_t user_env_elf_end[];
extern uint8_t user_mkdir_elf_start[];
extern uint8_t user_mkdir_elf_end[];
extern uint8_t user_rm_elf_start[];
extern uint8_t user_rm_elf_end[];
extern uint8_t user_descriptor_test_elf_start[];
extern uint8_t user_descriptor_test_elf_end[];
extern uint8_t user_signal_exec_test_elf_start[];
extern uint8_t user_signal_exec_test_elf_end[];

enum { RAMFS_PROGRAM_COUNT = 16 };

static const uint8_t motd_data[] =
    "Welcome to ROSE. Files now have descriptors and independent offsets.\n";

static struct vfs_node bin_nodes[RAMFS_PROGRAM_COUNT] = {
    {.name = "hello", .type = VFS_NODE_REGULAR},
    {.name = "fault", .type = VFS_NODE_REGULAR},
    {.name = "process-a", .type = VFS_NODE_REGULAR},
    {.name = "process-b", .type = VFS_NODE_REGULAR},
    {.name = "syscall-test", .type = VFS_NODE_REGULAR},
    {.name = "cat", .type = VFS_NODE_REGULAR},
    {.name = "console-read", .type = VFS_NODE_REGULAR},
    {.name = "sh", .type = VFS_NODE_REGULAR},
    {.name = "ls", .type = VFS_NODE_REGULAR},
    {.name = "echo", .type = VFS_NODE_REGULAR},
    {.name = "pwd", .type = VFS_NODE_REGULAR},
    {.name = "env", .type = VFS_NODE_REGULAR},
    {.name = "mkdir", .type = VFS_NODE_REGULAR},
    {.name = "rm", .type = VFS_NODE_REGULAR},
    {.name = "descriptor-test", .type = VFS_NODE_REGULAR},
    {.name = "signal-exec-test", .type = VFS_NODE_REGULAR},
};

static const struct vfs_node etc_nodes[] = {
    {.name = "motd",
     .type = VFS_NODE_REGULAR,
     .data = motd_data,
     .size = sizeof(motd_data) - 1U},
};

static const struct vfs_node root_nodes[] = {
    {.name = "bin",
     .type = VFS_NODE_DIRECTORY,
     .children = bin_nodes,
     .child_count = RAMFS_PROGRAM_COUNT},
    {.name = "etc",
     .type = VFS_NODE_DIRECTORY,
     .children = etc_nodes,
     .child_count = sizeof(etc_nodes) / sizeof(etc_nodes[0])},
    {.name = "dev",
     .type = VFS_NODE_DIRECTORY,
     .children = NULL,
     .child_count = 0U},
};

static const struct vfs_node ramfs_root = {
    .name = "",
    .type = VFS_NODE_DIRECTORY,
    .children = root_nodes,
    .child_count = sizeof(root_nodes) / sizeof(root_nodes[0]),
};

static void ramfs_set_file(size_t index, uint8_t *start, uint8_t *end) {
        uintptr_t start_address = (uintptr_t)start;
        uintptr_t end_address = (uintptr_t)end;

        if (index >= RAMFS_PROGRAM_COUNT || end_address <= start_address) {
                panic("Invalid embedded ramfs file");
        }

        bin_nodes[index].data = start;
        bin_nodes[index].size = (size_t)(end_address - start_address);
}

void ramfs_init(void) {
        ramfs_set_file(0U, user_hello_elf_start, user_hello_elf_end);
        ramfs_set_file(1U, user_fault_elf_start, user_fault_elf_end);
        ramfs_set_file(2U, user_process_a_elf_start,
                       user_process_a_elf_end);
        ramfs_set_file(3U, user_process_b_elf_start,
                       user_process_b_elf_end);
        ramfs_set_file(4U, user_syscall_test_elf_start,
                       user_syscall_test_elf_end);
        ramfs_set_file(5U, user_cat_elf_start, user_cat_elf_end);
        ramfs_set_file(6U, user_console_read_elf_start,
                       user_console_read_elf_end);
        ramfs_set_file(7U, user_sh_elf_start, user_sh_elf_end);
        ramfs_set_file(8U, user_ls_elf_start, user_ls_elf_end);
        ramfs_set_file(9U, user_echo_elf_start, user_echo_elf_end);
        ramfs_set_file(10U, user_pwd_elf_start, user_pwd_elf_end);
        ramfs_set_file(11U, user_env_elf_start, user_env_elf_end);
        ramfs_set_file(12U, user_mkdir_elf_start, user_mkdir_elf_end);
        ramfs_set_file(13U, user_rm_elf_start, user_rm_elf_end);
        ramfs_set_file(14U, user_descriptor_test_elf_start,
                       user_descriptor_test_elf_end);
        ramfs_set_file(15U, user_signal_exec_test_elf_start,
                       user_signal_exec_test_elf_end);

        if (!vfs_mount_root(&ramfs_root)) {
                panic("Failed to mount initial ramfs");
        }
}
