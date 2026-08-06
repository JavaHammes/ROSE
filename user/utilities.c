#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/runtime.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum { UTILITY_BUFFER_SIZE = 256, UTILITY_PATH_SIZE = 64 };

static void utility_error_path(const char *command, const char *message,
                               const char *path) {
        rose_print_error(command);
        rose_print_error(": ");
        rose_print_error(message);
        rose_print_error(": ");
        rose_print_error(path);
        rose_print_error("\n");
}

static bool utility_copy_data(int source, int destination) {
        char buffer[UTILITY_BUFFER_SIZE];
        while (true) {
                long count = rose_read(source, buffer, sizeof(buffer));
                if (count < 0) {
                        return false;
                }
                if (count == 0) {
                        return true;
                }
                if (!rose_write_all(destination, buffer, (size_t)count)) {
                        return false;
                }
        }
}

static const char *utility_basename(const char *path) {
        const char *name = path;
        while (*path != '\0') {
                if (*path++ == '/') {
                        name = path;
                }
        }
        return name;
}

static bool utility_destination_path(const char *source, const char *requested,
                                     char result[UTILITY_PATH_SIZE]) {
        struct user_file_status status;
        if (rose_stat(requested, &status) != 0 ||
            status.type != USER_FILE_DIRECTORY) {
                if (rose_string_length(requested) >= UTILITY_PATH_SIZE) {
                        return false;
                }
                rose_string_copy(result, requested);
                return true;
        }

        size_t length = rose_string_length(requested);
        const char *name = utility_basename(source);
        size_t name_length = rose_string_length(name);
        bool slash = length != 0U && requested[length - 1U] != '/';
        if (length + (slash ? 1U : 0U) + name_length + 1U > UTILITY_PATH_SIZE) {
                return false;
        }
        rose_string_copy(result, requested);
        if (slash) {
                result[length++] = '/';
        }
        rose_string_copy(&result[length], name);
        return true;
}

static int utility_copy_file(const char *source_path,
                             const char *requested_destination) {
        struct user_file_status source_status;
        if (rose_stat(source_path, &source_status) != 0 ||
            source_status.type != USER_FILE_REGULAR) {
                utility_error_path("cp", "not a regular file", source_path);
                return 1;
        }

        char destination_path[UTILITY_PATH_SIZE];
        if (!utility_destination_path(source_path, requested_destination,
                                      destination_path)) {
                utility_error_path("cp", "invalid destination",
                                   requested_destination);
                return 1;
        }
        struct user_file_status destination_status;
        if (rose_stat(destination_path, &destination_status) == 0 &&
            destination_status.inode == source_status.inode) {
                utility_error_path("cp", "source and destination are the same",
                                   source_path);
                return 1;
        }

        long source = rose_open(source_path, USER_OPEN_READ);
        if (source < 0) {
                utility_error_path("cp", "unable to open", source_path);
                return 1;
        }
        long destination =
            rose_open(destination_path,
                      USER_OPEN_WRITE | USER_OPEN_CREATE | USER_OPEN_TRUNCATE);
        if (destination < 0) {
                (void)rose_close((int)source);
                utility_error_path("cp", "unable to create", destination_path);
                return 1;
        }

        bool copied = utility_copy_data((int)source, (int)destination);
        bool closed =
            rose_close((int)source) == 0 && rose_close((int)destination) == 0;
        if (!copied || !closed) {
                utility_error_path("cp", "copy failed", source_path);
                return 1;
        }
        return 0;
}

int rose_cp_main(int argc, char **argv) {
        if (argc != 3) {
                rose_print_error("Usage: cp SOURCE DESTINATION\n");
                return 1;
        }
        return utility_copy_file(argv[1], argv[2]);
}

int rose_mv_main(int argc, char **argv) {
        if (argc != 3) {
                rose_print_error("Usage: mv SOURCE DESTINATION\n");
                return 1;
        }
        char destination_path[UTILITY_PATH_SIZE];
        if (!utility_destination_path(argv[1], argv[2], destination_path)) {
                utility_error_path("mv", "invalid destination", argv[2]);
                return 1;
        }
        if (rose_rename(argv[1], destination_path) != 0) {
                utility_error_path("mv", "unable to rename", argv[1]);
                return 1;
        }
        return 0;
}

int rose_touch_main(int argc, char **argv) {
        if (argc < 2) {
                rose_print_error("Usage: touch FILE...\n");
                return 1;
        }
        int status = 0;
        for (int index = 1; index < argc; index++) {
                long descriptor =
                    rose_open(argv[index], USER_OPEN_WRITE | USER_OPEN_CREATE);
                if (descriptor < 0 || rose_close((int)descriptor) != 0) {
                        utility_error_path("touch", "unable to create",
                                           argv[index]);
                        status = 1;
                }
        }
        return status;
}

static int utility_head_descriptor(int descriptor, uint64_t line_limit) {
        char buffer[UTILITY_BUFFER_SIZE];
        uint64_t lines = 0U;
        while (lines < line_limit) {
                long count = rose_read(descriptor, buffer, sizeof(buffer));
                if (count < 0) {
                        return 1;
                }
                if (count == 0) {
                        return 0;
                }
                size_t output = 0U;
                while (output < (size_t)count) {
                        if (buffer[output++] == '\n' && ++lines == line_limit) {
                                break;
                        }
                }
                if (!rose_write_all(USER_STDOUT_FILENO, buffer, output)) {
                        return 1;
                }
        }
        return 0;
}

int rose_head_main(int argc, char **argv) {
        uint64_t lines = 10U;
        int first_path = 1;
        if (argc >= 3 && rose_strings_equal(argv[1], "-n")) {
                if (!rose_parse_u64(argv[2], &lines)) {
                        rose_print_error("head: invalid line count\n");
                        return 1;
                }
                first_path = 3;
        }
        if (first_path == argc) {
                return utility_head_descriptor(USER_STDIN_FILENO, lines);
        }

        int status = 0;
        for (int index = first_path; index < argc; index++) {
                long descriptor = rose_open(argv[index], USER_OPEN_READ);
                if (descriptor < 0) {
                        utility_error_path("head", "unable to open",
                                           argv[index]);
                        status = 1;
                        continue;
                }
                if (utility_head_descriptor((int)descriptor, lines) != 0 ||
                    rose_close((int)descriptor) != 0) {
                        utility_error_path("head", "read failed", argv[index]);
                        status = 1;
                }
        }
        return status;
}

static int utility_wc_descriptor(int descriptor, const char *name) {
        char buffer[UTILITY_BUFFER_SIZE];
        uint64_t bytes = 0U;
        uint64_t lines = 0U;
        uint64_t words = 0U;
        bool in_word = false;
        while (true) {
                long count = rose_read(descriptor, buffer, sizeof(buffer));
                if (count < 0) {
                        return 1;
                }
                if (count == 0) {
                        break;
                }
                bytes += (uint64_t)count;
                for (long index = 0; index < count; index++) {
                        char character = buffer[index];
                        if (character == '\n') {
                                lines++;
                        }
                        bool space = character == ' ' || character == '\t' ||
                                     character == '\r' || character == '\n';
                        if (!space && !in_word) {
                                words++;
                        }
                        in_word = !space;
                }
        }
        rose_print_u64(lines);
        rose_print(" ");
        rose_print_u64(words);
        rose_print(" ");
        rose_print_u64(bytes);
        if (name != NULL) {
                rose_print(" ");
                rose_print(name);
        }
        rose_print("\n");
        return 0;
}

int rose_wc_main(int argc, char **argv) {
        if (argc == 1) {
                return utility_wc_descriptor(USER_STDIN_FILENO, NULL);
        }
        int status = 0;
        for (int index = 1; index < argc; index++) {
                long descriptor = rose_open(argv[index], USER_OPEN_READ);
                if (descriptor < 0) {
                        utility_error_path("wc", "unable to open", argv[index]);
                        status = 1;
                        continue;
                }
                if (utility_wc_descriptor((int)descriptor, argv[index]) != 0 ||
                    rose_close((int)descriptor) != 0) {
                        utility_error_path("wc", "read failed", argv[index]);
                        status = 1;
                }
        }
        return status;
}

static int utility_find_path(const char *path) {
        struct user_file_status status;
        if (rose_stat(path, &status) != 0) {
                utility_error_path("find", "unable to stat", path);
                return 1;
        }
        rose_print(path);
        rose_print("\n");
        if (status.type != USER_FILE_DIRECTORY) {
                return 0;
        }

        long descriptor = rose_open(path, USER_OPEN_READ | USER_OPEN_DIRECTORY);
        if (descriptor < 0) {
                utility_error_path("find", "unable to open", path);
                return 1;
        }
        int result_status = 0;
        struct user_directory_entry entry;
        long result;
        while ((result = rose_read_directory((int)descriptor, &entry)) > 0) {
                if (rose_strings_equal(entry.name, ".") ||
                    rose_strings_equal(entry.name, "..")) {
                        continue;
                }
                char child[UTILITY_PATH_SIZE];
                size_t length = rose_string_length(path);
                size_t name_length = rose_string_length(entry.name);
                bool slash = length == 0U || path[length - 1U] != '/';
                if (length + (slash ? 1U : 0U) + name_length + 1U >
                    sizeof(child)) {
                        utility_error_path("find", "path too long", path);
                        result_status = 1;
                        continue;
                }
                rose_string_copy(child, path);
                if (slash) {
                        child[length++] = '/';
                }
                rose_string_copy(&child[length], entry.name);
                if (utility_find_path(child) != 0) {
                        result_status = 1;
                }
        }
        if (result < 0) {
                utility_error_path("find", "unable to read", path);
                result_status = 1;
        }
        if (rose_close((int)descriptor) != 0) {
                result_status = 1;
        }
        return result_status;
}

int rose_find_main(int argc, char **argv) {
        if (argc > 2) {
                rose_print_error("Usage: find [PATH]\n");
                return 1;
        }
        return utility_find_path(argc == 2 ? argv[1] : ".");
}

int rose_kill_main(int argc, char **argv) {
        int signal = USER_SIGNAL_TERMINATE;
        int first_pid = 1;
        if (argc >= 2 && argv[1][0] == '-') {
                uint64_t parsed;
                if (!rose_parse_u64(&argv[1][1], &parsed) || parsed == 0U ||
                    parsed > USER_SIGNAL_MAX) {
                        rose_print_error("kill: invalid signal\n");
                        return 1;
                }
                signal = (int)parsed;
                first_pid = 2;
        }
        if (first_pid == argc) {
                rose_print_error("Usage: kill [-SIGNAL] PID...\n");
                return 1;
        }
        int status = 0;
        for (int index = first_pid; index < argc; index++) {
                uint64_t pid;
                if (!rose_parse_u64(argv[index], &pid) || pid == 0U ||
                    pid > INT64_MAX || rose_kill((int64_t)pid, signal) != 0) {
                        utility_error_path("kill", "unable to signal",
                                           argv[index]);
                        status = 1;
                }
        }
        return status;
}

int rose_sleep_main(int argc, char **argv) {
        uint64_t seconds;
        if (argc != 2 || !rose_parse_u64(argv[1], &seconds) ||
            seconds > UINT64_MAX / UINT64_C(1000000000)) {
                rose_print_error("Usage: sleep SECONDS\n");
                return 1;
        }
        return rose_sleep(seconds * UINT64_C(1000000000)) == 0 ? 0 : 1;
}

static struct user_process_info utility_processes[USER_PROCESS_INFO_LIMIT];

int rose_ps_main(int argc, char **argv) {
        (void)argv;
        if (argc != 1) {
                rose_print_error("Usage: ps\n");
                return 1;
        }
        long count =
            rose_process_list(utility_processes, USER_PROCESS_INFO_LIMIT);
        if (count < 0) {
                rose_print_error("ps: unable to read process table\n");
                return 1;
        }
        rose_print("PID PPID PGID STATE COMMAND\n");
        for (long index = 0; index < count; index++) {
                const struct user_process_info *process =
                    &utility_processes[index];
                rose_print_u64(process->pid);
                rose_print(" ");
                rose_print_u64(process->parent_pid);
                rose_print(" ");
                rose_print_u64(process->process_group);
                rose_print(" ");
                static const char *const states[] = {"ready", "running",
                                                     "blocked", "stopped"};
                if (process->state > USER_PROCESS_STOPPED) {
                        rose_print("unknown");
                } else {
                        rose_print(states[process->state]);
                }
                rose_print(" ");
                rose_print(process->name);
                rose_print("\n");
        }
        return 0;
}
