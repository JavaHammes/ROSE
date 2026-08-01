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
        SHELL_PIPELINE_LIMIT = 6,
        SHELL_ENVIRONMENT_LIMIT = 16,
        SHELL_ENVIRONMENT_ENTRY_SIZE = 64,
        SHELL_PATH_SIZE = 64,
};

struct shell_command {
        char *arguments[SHELL_ARGUMENT_LIMIT + 1U];
        int argument_count;
        char *input_path;
        char *output_path;
};

struct shell_pipeline {
        struct shell_command commands[SHELL_PIPELINE_LIMIT];
        size_t command_count;
        char storage[SHELL_LINE_SIZE];
};

struct shell_saved_descriptors {
        int input;
        int output;
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

static bool shell_is_operator(char character) {
        return character == '|' || character == '<' || character == '>';
}

/* Tokenize words while quotes and escapes are still visible, then build a
 * bounded pipeline description. Operators are special only outside quotes. */
static bool shell_parse_pipeline(const char *line,
                                 struct shell_pipeline *pipeline) {
        for (size_t index = 0U; index < SHELL_PIPELINE_LIMIT; index++) {
                pipeline->commands[index].argument_count = 0;
                pipeline->commands[index].arguments[0] = NULL;
                pipeline->commands[index].input_path = NULL;
                pipeline->commands[index].output_path = NULL;
        }
        pipeline->command_count = 0U;

        const char *source = line;
        char *destination = pipeline->storage;
        char *storage_end = &pipeline->storage[sizeof(pipeline->storage)];
        struct shell_command *command = &pipeline->commands[0];
        char pending_redirection = '\0';
        bool saw_token = false;

        while (true) {
                while (*source == ' ' || *source == '\t') {
                        source++;
                }
                if (*source == '\0') {
                        break;
                }

                if (shell_is_operator(*source)) {
                        char operator = *source++;
                        saw_token = true;

                        if (operator == '|') {
                                if (pending_redirection != '\0' ||
                                    command->argument_count == 0 ||
                                    pipeline->command_count + 1U >=
                                        SHELL_PIPELINE_LIMIT) {
                                        return false;
                                }
                                command->arguments[command->argument_count] =
                                    NULL;
                                pipeline->command_count++;
                                command = &pipeline->commands
                                               [pipeline->command_count];
                                continue;
                        }
                        if (pending_redirection != '\0' ||
                            (operator == '<' &&
                             command->input_path != NULL) ||
                            (operator == '>' &&
                             command->output_path != NULL)) {
                                return false;
                        }
                        pending_redirection = operator;
                        continue;
                }

                if (pending_redirection == '\0' &&
                    command->argument_count == SHELL_ARGUMENT_LIMIT) {
                        return false;
                }
                if (destination == storage_end) {
                        return false;
                }

                char *word = destination;
                char quote = '\0';
                while (*source != '\0') {
                        char character = *source;

                        if (quote == '\0' &&
                            (character == ' ' || character == '\t' ||
                             shell_is_operator(character))) {
                                break;
                        }
                        source++;

                        if (character == '\\' && quote != '\'') {
                                if (*source == '\0' ||
                                    destination == storage_end) {
                                        return false;
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
                        if (destination == storage_end) {
                                return false;
                        }
                        *destination++ = character;
                }

                if (quote != '\0' || destination == storage_end) {
                        return false;
                }
                *destination++ = '\0';
                saw_token = true;

                if (pending_redirection == '<') {
                        command->input_path = word;
                        pending_redirection = '\0';
                } else if (pending_redirection == '>') {
                        command->output_path = word;
                        pending_redirection = '\0';
                } else {
                        command->arguments[command->argument_count++] = word;
                }
        }

        if (!saw_token) {
                return true;
        }
        if (pending_redirection != '\0' || command->argument_count == 0) {
                return false;
        }
        command->arguments[command->argument_count] = NULL;
        pipeline->command_count++;
        return true;
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
        print("Built-ins: cd pwd echo env setenv unsetenv clear help exit\n");
        print("Commands: ls cat echo pwd env mkdir rm\n");
        print("Syntax: command [ARG...] [< FILE] [> FILE] [| command...]\n");
}

static bool shell_is_builtin(const char *name) {
        return strings_equal(name, "exit") || strings_equal(name, "help") ||
               strings_equal(name, "echo") || strings_equal(name, "clear") ||
               strings_equal(name, "pwd") || strings_equal(name, "cd") ||
               strings_equal(name, "env") || strings_equal(name, "setenv") ||
               strings_equal(name, "unsetenv") || strings_equal(name, "run");
}

static bool shell_execute_builtin(int count, char **arguments,
                                  bool *keep_running) {
        *keep_running = true;

        if (strings_equal(arguments[0], "exit")) {
                print("Shutting down...\n");
                *keep_running = false;
                return true;
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
        return false;
}

static bool shell_save_standard_descriptors(
    struct shell_saved_descriptors *saved) {
        saved->input = (int)rose_dup(USER_STDIN_FILENO);
        if (saved->input < 0) {
                return false;
        }
        saved->output = (int)rose_dup(USER_STDOUT_FILENO);
        if (saved->output < 0) {
                (void)rose_close(saved->input);
                return false;
        }
        if (rose_set_descriptor_flags(saved->input,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0 ||
            rose_set_descriptor_flags(saved->output,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0) {
                (void)rose_close(saved->input);
                (void)rose_close(saved->output);
                return false;
        }
        return true;
}

static bool shell_restore_standard_descriptors(
    const struct shell_saved_descriptors *saved) {
        return rose_dup2(saved->input, USER_STDIN_FILENO) ==
                   USER_STDIN_FILENO &&
               rose_dup2(saved->output, USER_STDOUT_FILENO) ==
                   USER_STDOUT_FILENO;
}

static void shell_release_standard_descriptors(
    const struct shell_saved_descriptors *saved) {
        (void)rose_close(saved->input);
        (void)rose_close(saved->output);
}

static bool shell_apply_redirections(const struct shell_command *command) {
        if (command->input_path != NULL) {
                long descriptor =
                    rose_open(command->input_path, USER_OPEN_READ);
                if (descriptor < 0) {
                        print("sh: unable to open input: ");
                        print(command->input_path);
                        print("\n");
                        return false;
                }
                bool duplicated =
                    rose_dup2((int)descriptor, USER_STDIN_FILENO) ==
                    USER_STDIN_FILENO;
                (void)rose_close((int)descriptor);
                if (!duplicated) {
                        print("sh: unable to redirect standard input\n");
                        return false;
                }
        }

        if (command->output_path != NULL) {
                long descriptor = rose_open(
                    command->output_path,
                    USER_OPEN_WRITE | USER_OPEN_CREATE | USER_OPEN_TRUNCATE);
                if (descriptor < 0) {
                        print("sh: unable to open output: ");
                        print(command->output_path);
                        print("\n");
                        return false;
                }
                bool duplicated =
                    rose_dup2((int)descriptor, USER_STDOUT_FILENO) ==
                    USER_STDOUT_FILENO;
                (void)rose_close((int)descriptor);
                if (!duplicated) {
                        print("sh: unable to redirect standard output\n");
                        return false;
                }
        }
        return true;
}

static int shell_run_pipeline(struct shell_pipeline *pipeline) {
        struct shell_saved_descriptors saved;
        if (!shell_save_standard_descriptors(&saved)) {
                print("sh: unable to save standard descriptors\n");
                return 1;
        }

        long children[SHELL_PIPELINE_LIMIT];
        size_t child_count = 0U;
        int previous_read = -1;
        int result_status = 0;

        for (size_t index = 0U; index < pipeline->command_count; index++) {
                struct shell_command *command = &pipeline->commands[index];
                bool has_next = index + 1U < pipeline->command_count;
                int next_read = -1;

                if ((previous_read >= 0 && command->input_path != NULL) ||
                    (has_next && command->output_path != NULL)) {
                        result_status = 1;
                        break;
                }

                if (previous_read >= 0) {
                        if (rose_dup2(previous_read, USER_STDIN_FILENO) !=
                            USER_STDIN_FILENO) {
                                result_status = 1;
                                break;
                        }
                        (void)rose_close(previous_read);
                        previous_read = -1;
                }
                if (!shell_apply_redirections(command)) {
                        result_status = 1;
                        break;
                }

                if (has_next) {
                        int descriptors[2] = {-1, -1};
                        if (rose_pipe(descriptors) != 0 ||
                            rose_set_descriptor_flags(
                                descriptors[0],
                                USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0 ||
                            rose_dup2(descriptors[1], USER_STDOUT_FILENO) !=
                                USER_STDOUT_FILENO) {
                                if (descriptors[0] >= 0) {
                                        (void)rose_close(descriptors[0]);
                                }
                                if (descriptors[1] >= 0) {
                                        (void)rose_close(descriptors[1]);
                                }
                                print("sh: unable to configure pipeline\n");
                                result_status = 1;
                                break;
                        }
                        next_read = descriptors[0];
                        (void)rose_close(descriptors[1]);
                }

                long child = spawn_command(command->arguments);
                if (!shell_restore_standard_descriptors(&saved)) {
                        print("sh: unable to restore standard descriptors\n");
                        result_status = 1;
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        break;
                }
                if (child < 0) {
                        print("sh: command not found: ");
                        print(command->arguments[0]);
                        print("\n");
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        result_status = 127;
                        break;
                }

                children[child_count++] = child;
                previous_read = next_read;
        }

        if (!shell_restore_standard_descriptors(&saved)) {
                result_status = 1;
        }
        if (previous_read >= 0) {
                (void)rose_close(previous_read);
        }
        shell_release_standard_descriptors(&saved);

        for (size_t index = 0U; index < child_count; index++) {
                int status = 0;
                if (rose_waitpid(children[index], &status, 0U) !=
                    children[index]) {
                        result_status = 1;
                } else if (index + 1U == child_count && result_status == 0) {
                        result_status =
                            (int)USER_WAIT_STATUS_EXIT_CODE(status);
                }
        }
        return result_status;
}

static bool shell_execute(struct shell_pipeline *pipeline) {
        if (pipeline->command_count == 0U) {
                return true;
        }

        struct shell_command *command = &pipeline->commands[0];
        if (pipeline->command_count == 1U &&
            shell_is_builtin(command->arguments[0])) {
                struct shell_saved_descriptors saved;
                if (!shell_save_standard_descriptors(&saved)) {
                        print("sh: unable to save standard descriptors\n");
                        return true;
                }

                bool keep_running = true;
                if (!shell_apply_redirections(command)) {
                        (void)shell_restore_standard_descriptors(&saved);
                        print("sh: unable to redirect command\n");
                } else {
                        (void)shell_execute_builtin(command->argument_count,
                                                    command->arguments,
                                                    &keep_running);
                        (void)shell_restore_standard_descriptors(&saved);
                }
                shell_release_standard_descriptors(&saved);
                return keep_running;
        }

        if (pipeline->command_count > 1U) {
                for (size_t index = 0U; index < pipeline->command_count;
                     index++) {
                        const struct shell_command *item =
                            &pipeline->commands[index];
                        if ((index != 0U && item->input_path != NULL) ||
                            (index + 1U != pipeline->command_count &&
                             item->output_path != NULL)) {
                                print("sh: redirection conflicts with pipeline\n");
                                return true;
                        }
                }
        }

        (void)shell_run_pipeline(pipeline);
        return true;
}

int rose_shell_main(char **environment) { // NOLINT(misc-use-internal-linkage)
        environment_initialize(environment);
        print("ROSE userspace shell\n");

        while (true) {
                char line[SHELL_LINE_SIZE];
                struct shell_pipeline pipeline;

                if (!shell_read_line(line)) {
                        print("Shutting down...\n");
                        return 0;
                }

                if (!shell_parse_pipeline(line, &pipeline)) {
                        print("sh: invalid or too long command line\n");
                        continue;
                }
                if (!shell_execute(&pipeline)) {
                        return 0;
                }
        }
}
