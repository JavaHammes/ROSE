/*
 * Small interactive userspace shell.
 *
 * The kernel exposes the console as standard descriptors and has no knowledge
 * of command syntax. This process owns line editing, tokenization, built-ins,
 * PATH lookup, and the spawn/wait lifecycle for foreground programs.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/syscall.h"
#include "user_abi.h"

enum {
        SHELL_LINE_SIZE = 128,
        SHELL_ARGUMENT_LIMIT = 16,
        SHELL_ENVIRONMENT_LIMIT = 16,
        SHELL_ENVIRONMENT_ENTRY_SIZE = 64,
        SHELL_PATH_SIZE = 64,
};

static char shell_environment_storage[SHELL_ENVIRONMENT_LIMIT]
                                     [SHELL_ENVIRONMENT_ENTRY_SIZE];
static char *shell_environment[SHELL_ENVIRONMENT_LIMIT + 1U];
static size_t shell_environment_count;

static size_t string_length(const char *text) {
        size_t length = 0U;

        while (text[length] != '\0') {
                length++;
        }

        return length;
}

static bool strings_equal(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }

        return *left == *right;
}

static bool string_contains(const char *text, char character) {
        while (*text != '\0') {
                if (*text == character) {
                        return true;
                }
                text++;
        }

        return false;
}

static void print(const char *text) {
        (void)rose_write(USER_STDOUT_FILENO, text, string_length(text));
}

static void print_character(char character) {
        (void)rose_write(USER_STDOUT_FILENO, &character, 1U);
}

static void copy_string(char *destination, const char *source) {
        size_t index = 0U;

        do {
                destination[index] = source[index];
                index++;
        } while (source[index - 1U] != '\0');
}

static bool environment_name_is_valid(const char *name) {
        if (*name == '\0') {
                return false;
        }

        while (*name != '\0') {
                if (*name == '=') {
                        return false;
                }
                name++;
        }

        return true;
}

static bool environment_entry_has_name(const char *entry, const char *name) {
        while (*name != '\0' && *entry == *name) {
                entry++;
                name++;
        }

        return *name == '\0' && *entry == '=';
}

static const char *environment_value(const char *name) {
        for (size_t index = 0U; index < shell_environment_count; index++) {
                const char *entry = shell_environment[index];

                if (environment_entry_has_name(entry, name)) {
                        return &entry[string_length(name) + 1U];
                }
        }

        return NULL;
}

static bool environment_set(const char *name, const char *value) {
        if (!environment_name_is_valid(name)) {
                return false;
        }

        size_t name_length = string_length(name);
        size_t value_length = string_length(value);
        if (name_length + 1U + value_length + 1U >
            SHELL_ENVIRONMENT_ENTRY_SIZE) {
                return false;
        }

        size_t destination = shell_environment_count;
        for (size_t index = 0U; index < shell_environment_count; index++) {
                if (environment_entry_has_name(shell_environment[index],
                                               name)) {
                        destination = index;
                        break;
                }
        }

        if (destination == SHELL_ENVIRONMENT_LIMIT) {
                return false;
        }
        if (destination == shell_environment_count) {
                shell_environment[destination] =
                    shell_environment_storage[destination];
                shell_environment_count++;
                shell_environment[shell_environment_count] = NULL;
        }

        size_t offset = 0U;
        for (size_t index = 0U; index < name_length; index++) {
                shell_environment[destination][offset++] = name[index];
        }
        shell_environment[destination][offset++] = '=';
        for (size_t index = 0U; index < value_length; index++) {
                shell_environment[destination][offset++] = value[index];
        }
        shell_environment[destination][offset] = '\0';
        return true;
}

static void environment_unset(const char *name) {
        size_t removed = shell_environment_count;

        for (size_t index = 0U; index < shell_environment_count; index++) {
                if (environment_entry_has_name(shell_environment[index],
                                               name)) {
                        removed = index;
                        break;
                }
        }

        if (removed == shell_environment_count) {
                return;
        }

        for (size_t index = removed; index + 1U < shell_environment_count;
             index++) {
                copy_string(shell_environment_storage[index],
                            shell_environment_storage[index + 1U]);
        }
        shell_environment_count--;
        shell_environment_storage[shell_environment_count][0] = '\0';
        shell_environment[shell_environment_count] = NULL;
}

static void environment_initialize(char **environment) {
        shell_environment_count = 0U;
        shell_environment[0] = NULL;

        if (environment != NULL) {
                for (size_t index = 0U;
                     environment[index] != NULL &&
                     shell_environment_count < SHELL_ENVIRONMENT_LIMIT;
                     index++) {
                        size_t length = string_length(environment[index]);

                        if (length + 1U > SHELL_ENVIRONMENT_ENTRY_SIZE) {
                                continue;
                        }
                        shell_environment[shell_environment_count] =
                            shell_environment_storage[shell_environment_count];
                        copy_string(
                            shell_environment_storage[shell_environment_count],
                            environment[index]);
                        shell_environment_count++;
                }
                shell_environment[shell_environment_count] = NULL;
        }

        if (environment_value("HOME") == NULL) {
                (void)environment_set("HOME", "/");
        }
        if (environment_value("PATH") == NULL) {
                (void)environment_set("PATH", "/bin:/sbin");
        }
        if (environment_value("TERM") == NULL) {
                (void)environment_set("TERM", "rose");
        }
}

/* Read and echo one editable command line from the console. */
static bool shell_read_line(char line[SHELL_LINE_SIZE]) {
        size_t length = 0U;

        print("rose> ");

        while (true) {
                char character;
                if (rose_read(USER_STDIN_FILENO, &character, 1U) != 1) {
                        return false;
                }

                if (character == '\r' || character == '\n') {
                        line[length] = '\0';
                        print("\n");
                        return true;
                }
                if (character == '\x04') {
                        if (length == 0U) {
                                print("\n");
                                return false;
                        }
                        line[length] = '\0';
                        print("\n");
                        return true;
                }
                if (character == '\b' || character == '\x7f') {
                        if (length != 0U) {
                                length--;
                                print("\b \b");
                        }
                        continue;
                }
                if (character < ' ' || character > '~' ||
                    length >= SHELL_LINE_SIZE - 1U) {
                        continue;
                }

                line[length++] = character;
                print_character(character);
        }
}

/*
 * Tokenize in place. Spaces separate words outside quotes; quote delimiters
 * are removed, and a backslash quotes the following character. This remains a
 * deliberately small shell grammar without expansion or control operators.
 */
static int shell_parse_arguments(char *line,
                                 char *arguments[SHELL_ARGUMENT_LIMIT + 1U]) {
        char *source = line;
        char *destination = line;
        size_t count = 0U;

        while (*source != '\0') {
                while (*source == ' ' || *source == '\t') {
                        source++;
                }
                if (*source == '\0') {
                        break;
                }
                if (count == SHELL_ARGUMENT_LIMIT) {
                        return -1;
                }

                arguments[count++] = destination;
                char quote = '\0';

                while (*source != '\0') {
                        char character = *source;

                        if (quote == '\0' &&
                            (character == ' ' || character == '\t')) {
                                break;
                        }
                        source++;

                        if (character == '\\' && quote != '\'') {
                                if (*source == '\0') {
                                        return -1;
                                }
                                *destination++ = *source++;
                                continue;
                        }
                        if (character == '\'' || character == '"') {
                                if (quote == '\0') {
                                        quote = character;
                                        continue;
                                }
                                if (quote == character) {
                                        quote = '\0';
                                        continue;
                                }
                        }

                        *destination++ = character;
                }

                if (quote != '\0') {
                        return -1;
                }
                while (*source == ' ' || *source == '\t') {
                        source++;
                }
                *destination++ = '\0';
        }

        arguments[count] = NULL;
        return (int)count;
}

static void print_exit_status(int status) {
        static const char digits[] = "0123456789";
        char text[4];
        uint32_t value = USER_WAIT_STATUS_EXIT_CODE(status);
        size_t length = 0U;

        do {
                text[length++] = digits[value % 10U];
                value /= 10U;
        } while (value != 0U);

        while (length != 0U) {
                print_character(text[--length]);
        }
}

static long spawn_path(const char *path, char **arguments) {
        return rose_spawn(path, arguments, shell_environment);
}

static long spawn_command(char **arguments) {
        if (string_contains(arguments[0], '/')) {
                return spawn_path(arguments[0], arguments);
        }

        const char *path = environment_value("PATH");
        if (path == NULL) {
                return -USER_ERROR_NO_ENTRY;
        }

        while (true) {
                char candidate[SHELL_PATH_SIZE];
                size_t length = 0U;

                while (*path != '\0' && *path != ':') {
                        if (length + 1U >= sizeof(candidate)) {
                                length = sizeof(candidate);
                        } else {
                                candidate[length++] = *path;
                        }
                        path++;
                }
                if (length != 0U && length < sizeof(candidate)) {
                        if (candidate[length - 1U] != '/') {
                                if (length + 1U >= sizeof(candidate)) {
                                        length = sizeof(candidate);
                                } else {
                                        candidate[length++] = '/';
                                }
                        }

                        const char *name = arguments[0];
                        while (*name != '\0' && length + 1U < sizeof(candidate)) {
                                candidate[length++] = *name++;
                        }
                        if (*name == '\0' && length < sizeof(candidate)) {
                                candidate[length] = '\0';
                                char *command_name = arguments[0];
                                arguments[0] = candidate;
                                long result = spawn_path(candidate, arguments);
                                arguments[0] = command_name;

                                if (result != -USER_ERROR_NO_ENTRY) {
                                        return result;
                                }
                        }
                }

                if (*path == '\0') {
                        break;
                }
                path++;
        }

        return -USER_ERROR_NO_ENTRY;
}

static int run_foreground(char **arguments, bool report_status) {
        long child = spawn_command(arguments);

        if (child < 0) {
                print("sh: command not found: ");
                print(arguments[0]);
                print("\n");
                return 127;
        }

        int status = 0;
        long waited = rose_waitpid(child, &status, 0U);
        if (waited != child) {
                print("sh: wait failed\n");
                return 1;
        }

        if (report_status) {
                print("Process exited with status ");
                print_exit_status(status);
                print("\n");
        }

        return (int)USER_WAIT_STATUS_EXIT_CODE(status);
}

static void shell_help(void) {
        print("Built-ins: cd pwd echo env setenv unsetenv clear exit\n");
        print("Programs are loaded from PATH and run in the foreground.\n");
}

static bool shell_execute(int count, char **arguments) {
        if (count == 0) {
                return true;
        }

        if (strings_equal(arguments[0], "exit")) {
                print("Shutting down...\n");
                return false;
        }
        if (strings_equal(arguments[0], "help")) {
                shell_help();
                return true;
        }
        if (strings_equal(arguments[0], "echo")) {
                for (int index = 1; index < count; index++) {
                        print(arguments[index]);
                        if (index + 1 < count) {
                                print(" ");
                        }
                }
                print("\n");
                return true;
        }
        if (strings_equal(arguments[0], "clear")) {
                print("\x1b[2J\x1b[H");
                return true;
        }
        if (strings_equal(arguments[0], "pwd")) {
                char directory[SHELL_PATH_SIZE];
                if (rose_getcwd(directory, sizeof(directory)) < 0) {
                        print("pwd: unable to read the working directory\n");
                } else {
                        print(directory);
                        print("\n");
                }
                return true;
        }
        if (strings_equal(arguments[0], "cd")) {
                const char *directory = count == 1 ? environment_value("HOME")
                                                   : arguments[1];
                if (count > 2 || directory == NULL) {
                        print("Usage: cd [DIR]\n");
                } else if (rose_chdir(directory) < 0) {
                        print("cd: unable to change directory: ");
                        print(directory);
                        print("\n");
                }
                return true;
        }
        if (strings_equal(arguments[0], "env")) {
                if (count != 1) {
                        print("Usage: env\n");
                } else {
                        for (size_t index = 0U;
                             index < shell_environment_count; index++) {
                                print(shell_environment[index]);
                                print("\n");
                        }
                }
                return true;
        }
        if (strings_equal(arguments[0], "setenv")) {
                if (count != 3 ||
                    !environment_set(arguments[1], arguments[2])) {
                        print("Usage: setenv NAME VALUE\n");
                }
                return true;
        }
        if (strings_equal(arguments[0], "unsetenv")) {
                if (count != 2 ||
                    !environment_name_is_valid(arguments[1])) {
                        print("Usage: unsetenv NAME\n");
                } else {
                        environment_unset(arguments[1]);
                }
                return true;
        }

        /* Keep the former terminal's run command as a compatibility alias,
         * while normal shell use executes a path or PATH entry directly. */
        if (strings_equal(arguments[0], "run")) {
                char *default_arguments[] = {"/bin/hello", NULL};
                if (count == 1) {
                        (void)run_foreground(default_arguments, true);
                } else {
                        (void)run_foreground(&arguments[1], true);
                }
                return true;
        }

        (void)run_foreground(arguments, false);
        return true;
}

int rose_shell_main(char **environment) { // NOLINT(misc-use-internal-linkage)
        environment_initialize(environment);
        print("ROSE userspace shell\n");

        while (true) {
                char line[SHELL_LINE_SIZE];
                char *arguments[SHELL_ARGUMENT_LIMIT + 1U];

                if (!shell_read_line(line)) {
                        print("Shutting down...\n");
                        return 0;
                }

                int count = shell_parse_arguments(line, arguments);
                if (count < 0) {
                        print("sh: invalid or too long command line\n");
                        continue;
                }
                if (!shell_execute(count, arguments)) {
                        return 0;
                }
        }
}
