/*
 * Small interactive userspace shell.
 *
 * The kernel exposes the console as standard descriptors and has no knowledge
 * of command syntax. This process owns line editing, tokenization, built-ins,
 * PATH lookup, process-group construction, and foreground/background jobs.
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
        SHELL_JOB_LIMIT = 4,
        SHELL_REDIRECTION_LIMIT = 6,
};

enum shell_redirection_operation {
        SHELL_REDIRECTION_READ,
        SHELL_REDIRECTION_WRITE,
        SHELL_REDIRECTION_APPEND,
        SHELL_REDIRECTION_DUPLICATE,
};

struct shell_redirection {
        enum shell_redirection_operation operation;
        int destination;
        int source;
        char *path;
};

struct shell_command {
        char *arguments[SHELL_ARGUMENT_LIMIT + 1U];
        int argument_count;
        struct shell_redirection redirections[SHELL_REDIRECTION_LIMIT];
        size_t redirection_count;
};

struct shell_pipeline {
        struct shell_command commands[SHELL_PIPELINE_LIMIT];
        size_t command_count;
        bool background;
        char storage[SHELL_LINE_SIZE];
};

struct shell_job {
        bool used;
        uint64_t process_group;
        long children[SHELL_PIPELINE_LIMIT];
        bool finished[SHELL_PIPELINE_LIMIT];
        bool stopped[SHELL_PIPELINE_LIMIT];
        int statuses[SHELL_PIPELINE_LIMIT];
        size_t child_count;
};

struct shell_saved_descriptors {
        int input;
        int output;
        int error;
};

static char shell_environment_storage[SHELL_ENVIRONMENT_LIMIT]
                                     [SHELL_ENVIRONMENT_ENTRY_SIZE];
static char *shell_environment[SHELL_ENVIRONMENT_LIMIT + 1U];
static size_t shell_environment_count;
static struct shell_job shell_jobs[SHELL_JOB_LIMIT];
static uint64_t shell_process_group;
static int shell_last_status;

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

static void print_error(const char *text) {
        (void)rose_write(USER_STDERR_FILENO, text, string_length(text));
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

static const char *environment_value_length(const char *name, size_t length) {
        for (size_t index = 0U; index < shell_environment_count; index++) {
                const char *entry = shell_environment[index];
                size_t character = 0U;

                while (character < length &&
                       entry[character] == name[character]) {
                        character++;
                }
                if (character == length && entry[character] == '=') {
                        return &entry[character + 1U];
                }
        }

        return NULL;
}

static const char *environment_value(const char *name) {
        return environment_value_length(name, string_length(name));
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
        return character == '|' || character == '<' || character == '>' ||
               character == '&';
}

static bool
shell_command_redirects_descriptor(const struct shell_command *command,
                                   int descriptor) {
        for (size_t index = 0U; index < command->redirection_count; index++) {
                if (command->redirections[index].destination == descriptor) {
                        return true;
                }
        }
        return false;
}

static bool shell_parameter_name_start(char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') || character == '_';
}

static bool shell_parameter_name_character(char character) {
        return shell_parameter_name_start(character) ||
               (character >= '0' && character <= '9');
}

static bool shell_append_character(char **destination, char *storage_end,
                                   char character) {
        if (*destination == storage_end) {
                return false;
        }
        *(*destination)++ = character;
        return true;
}

static bool shell_append_text(char **destination, char *storage_end,
                              const char *text) {
        while (*text != '\0') {
                if (!shell_append_character(destination, storage_end,
                                            *text++)) {
                        return false;
                }
        }
        return true;
}

static bool shell_append_unsigned(char **destination, char *storage_end,
                                  uint64_t value) {
        static const char digits[] = "0123456789";
        char reversed[20];
        size_t length = 0U;

        do {
                reversed[length++] = digits[value % 10U];
                value /= 10U;
        } while (value != 0U);

        while (length != 0U) {
                if (!shell_append_character(destination, storage_end,
                                            reversed[--length])) {
                        return false;
                }
        }
        return true;
}

/* Expand one parameter beginning at *source and leave source at the first
 * unconsumed character. Expansion is deliberately substitution-only: the
 * compact shell does not perform field splitting or pathname expansion. */
static bool shell_expand_parameter(const char **source, char **destination,
                                   char *storage_end) {
        const char *cursor = *source + 1;

        if (*cursor == '?') {
                cursor++;
                *source = cursor;
                return shell_append_unsigned(destination, storage_end,
                                             (uint64_t)shell_last_status);
        }
        if (*cursor == '$') {
                long process_identifier = rose_getpid();
                if (process_identifier <= 0) {
                        return false;
                }
                cursor++;
                *source = cursor;
                return shell_append_unsigned(destination, storage_end,
                                             (uint64_t)process_identifier);
        }

        const char *name = cursor;
        size_t name_length = 0U;
        if (*cursor == '{') {
                cursor++;
                name = cursor;
                if (!shell_parameter_name_start(*cursor)) {
                        return false;
                }
                while (shell_parameter_name_character(*cursor)) {
                        cursor++;
                }
                name_length = (size_t)(cursor - name);
                if (*cursor != '}') {
                        return false;
                }
                cursor++;
        } else if (shell_parameter_name_start(*cursor)) {
                while (shell_parameter_name_character(*cursor)) {
                        cursor++;
                }
                name_length = (size_t)(cursor - name);
        } else {
                *source = cursor;
                return shell_append_character(destination, storage_end, '$');
        }

        const char *value = environment_value_length(name, name_length);
        *source = cursor;
        return value == NULL ||
               shell_append_text(destination, storage_end, value);
}

/* Tokenize words while quotes and escapes are still visible, then build a
 * bounded pipeline description. Operators are special only outside quotes;
 * parameter expansion is disabled by single quotes and backslash escaping. */
static bool shell_parse_pipeline(const char *line,
                                 struct shell_pipeline *pipeline) {
        for (size_t index = 0U; index < SHELL_PIPELINE_LIMIT; index++) {
                pipeline->commands[index].argument_count = 0;
                pipeline->commands[index].arguments[0] = NULL;
                pipeline->commands[index].redirection_count = 0U;
        }
        pipeline->command_count = 0U;
        pipeline->background = false;

        const char *source = line;
        char *destination = pipeline->storage;
        char *storage_end = &pipeline->storage[sizeof(pipeline->storage)];
        struct shell_command *command = &pipeline->commands[0];
        size_t pending_redirection = SHELL_REDIRECTION_LIMIT;
        bool saw_token = false;

        while (true) {
                while (*source == ' ' || *source == '\t') {
                        source++;
                }
                if (*source == '\0') {
                        break;
                }

                int explicit_descriptor = -1;
                if (*source >= '0' && *source <= '9') {
                        const char *number_end = source;
                        unsigned int descriptor = 0U;
                        bool descriptor_supported = true;
                        while (*number_end >= '0' && *number_end <= '9') {
                                if (descriptor > USER_STDERR_FILENO) {
                                        descriptor_supported = false;
                                } else {
                                        descriptor =
                                            descriptor * 10U +
                                            (unsigned int)(*number_end - '0');
                                }
                                number_end++;
                        }
                        if (*number_end == '<' || *number_end == '>') {
                                if (!descriptor_supported ||
                                    descriptor > USER_STDERR_FILENO) {
                                        return false;
                                }
                                explicit_descriptor = (int)descriptor;
                                source = number_end;
                        }
                }

                if (shell_is_operator(*source)) {
                        char redirection_operator = *source++;
                        saw_token = true;

                        bool append =
                            redirection_operator == '>' && *source == '>';
                        if (append) {
                                source++;
                        }

                        if (redirection_operator == '&') {
                                while (*source == ' ' || *source == '\t') {
                                        source++;
                                }
                                if (*source != '\0' ||
                                    pending_redirection !=
                                        SHELL_REDIRECTION_LIMIT ||
                                    command->argument_count == 0) {
                                        return false;
                                }
                                pipeline->background = true;
                                break;
                        }

                        if (redirection_operator == '|') {
                                if (pending_redirection !=
                                        SHELL_REDIRECTION_LIMIT ||
                                    command->argument_count == 0 ||
                                    pipeline->command_count + 1U >=
                                        SHELL_PIPELINE_LIMIT) {
                                        return false;
                                }
                                command->arguments[command->argument_count] =
                                    NULL;
                                pipeline->command_count++;
                                command =
                                    &pipeline
                                         ->commands[pipeline->command_count];
                                continue;
                        }
                        if (pending_redirection != SHELL_REDIRECTION_LIMIT ||
                            command->redirection_count ==
                                SHELL_REDIRECTION_LIMIT) {
                                return false;
                        }

                        int destination_descriptor = explicit_descriptor;
                        if (destination_descriptor < 0) {
                                destination_descriptor =
                                    redirection_operator == '<'
                                        ? USER_STDIN_FILENO
                                        : USER_STDOUT_FILENO;
                        }

                        struct shell_redirection *redirection =
                            &command
                                 ->redirections[command->redirection_count++];
                        redirection->destination = destination_descriptor;
                        redirection->source = -1;
                        redirection->path = NULL;

                        if (!append && *source == '&') {
                                source++;
                                if (*source < '0' || *source > '2') {
                                        return false;
                                }
                                redirection->operation =
                                    SHELL_REDIRECTION_DUPLICATE;
                                redirection->source = *source++ - '0';
                                if ((*source >= '0' && *source <= '9') ||
                                    (*source != '\0' && *source != ' ' &&
                                     *source != '\t' &&
                                     !shell_is_operator(*source))) {
                                        return false;
                                }
                                continue;
                        }

                        redirection->operation =
                            redirection_operator == '<'
                                ? SHELL_REDIRECTION_READ
                                : (append ? SHELL_REDIRECTION_APPEND
                                          : SHELL_REDIRECTION_WRITE);
                        pending_redirection = command->redirection_count - 1U;
                        continue;
                }

                if (pending_redirection == SHELL_REDIRECTION_LIMIT &&
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
                        if (character == '$' && quote != '\'') {
                                const char *parameter = source - 1;
                                if (!shell_expand_parameter(&parameter,
                                                            &destination,
                                                            storage_end)) {
                                        return false;
                                }
                                source = parameter;
                                continue;
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

                if (pending_redirection != SHELL_REDIRECTION_LIMIT) {
                        command->redirections[pending_redirection].path = word;
                        pending_redirection = SHELL_REDIRECTION_LIMIT;
                } else {
                        command->arguments[command->argument_count++] = word;
                }
        }

        if (!saw_token) {
                return true;
        }
        if (pending_redirection != SHELL_REDIRECTION_LIMIT ||
            command->argument_count == 0) {
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

static void print_unsigned(uint64_t value) {
        static const char digits[] = "0123456789";
        char text[20];
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
                        while (*name != '\0' &&
                               length + 1U < sizeof(candidate)) {
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

static void shell_job_initialize(struct shell_job *job, uint64_t process_group,
                                 const long *children, size_t child_count) {
        job->used = true;
        job->process_group = process_group;
        job->child_count = child_count;
        for (size_t index = 0U; index < SHELL_PIPELINE_LIMIT; index++) {
                job->children[index] =
                    index < child_count ? children[index] : 0;
                job->finished[index] = false;
                job->stopped[index] = false;
                job->statuses[index] = 0;
        }
}

static bool shell_job_is_complete(const struct shell_job *job) {
        for (size_t index = 0U; index < job->child_count; index++) {
                if (!job->finished[index]) {
                        return false;
                }
        }
        return true;
}

static bool shell_job_is_stopped(const struct shell_job *job) {
        bool has_live_child = false;

        for (size_t index = 0U; index < job->child_count; index++) {
                if (job->finished[index]) {
                        continue;
                }
                has_live_child = true;
                if (!job->stopped[index]) {
                        return false;
                }
        }
        return has_live_child;
}

static void shell_job_record_status(struct shell_job *job, size_t index,
                                    int status) {
        job->statuses[index] = status;
        if (USER_WAIT_STATUS_STOPPED(status)) {
                job->stopped[index] = true;
        } else if (USER_WAIT_STATUS_CONTINUED(status)) {
                job->stopped[index] = false;
        } else {
                job->finished[index] = true;
                job->stopped[index] = false;
        }
}

static void shell_job_poll(struct shell_job *job) {
        uint32_t options =
            USER_WAIT_NO_HANG | USER_WAIT_UNTRACED | USER_WAIT_CONTINUED;

        for (size_t index = 0U; index < job->child_count; index++) {
                while (!job->finished[index]) {
                        int status = 0;
                        long waited = rose_waitpid(job->children[index],
                                                   &status, options);
                        if (waited == 0) {
                                break;
                        }
                        if (waited < 0) {
                                job->finished[index] = true;
                                job->stopped[index] = false;
                                break;
                        }
                        shell_job_record_status(job, index, status);
                }
        }
}

/* Wait until every live member has either exited or reported a stop. */
static void shell_job_wait(struct shell_job *job) {
        while (!shell_job_is_complete(job) && !shell_job_is_stopped(job)) {
                bool waited_for_child = false;

                for (size_t index = 0U; index < job->child_count; index++) {
                        if (job->finished[index] || job->stopped[index]) {
                                continue;
                        }

                        int status = 0;
                        long waited = rose_waitpid(job->children[index],
                                                   &status, USER_WAIT_UNTRACED);
                        if (waited == job->children[index]) {
                                shell_job_record_status(job, index, status);
                        } else {
                                job->finished[index] = true;
                                job->stopped[index] = false;
                        }
                        waited_for_child = true;
                }

                if (!waited_for_child) {
                        break;
                }
        }
}

static size_t shell_job_store(const struct shell_job *job) {
        for (size_t index = 0U; index < SHELL_JOB_LIMIT; index++) {
                if (!shell_jobs[index].used) {
                        struct shell_job *destination = &shell_jobs[index];
                        destination->used = job->used;
                        destination->process_group = job->process_group;
                        destination->child_count = job->child_count;
                        for (size_t child = 0U; child < SHELL_PIPELINE_LIMIT;
                             child++) {
                                destination->children[child] =
                                    job->children[child];
                                destination->finished[child] =
                                    job->finished[child];
                                destination->stopped[child] =
                                    job->stopped[child];
                                destination->statuses[child] =
                                    job->statuses[child];
                        }
                        return index + 1U;
                }
        }
        return 0U;
}

static void shell_print_job(size_t identifier, const char *state,
                            uint64_t process_group) {
        print("[");
        print_unsigned(identifier);
        print("] ");
        print(state);
        print(" ");
        print_unsigned(process_group);
        print("\n");
}

static void shell_restore_foreground(void) {
        if (rose_tcsetpgrp(USER_STDIN_FILENO, (int64_t)shell_process_group) <
            0) {
                print_error("sh: unable to reclaim the console\n");
        }
}

static int run_foreground(char **arguments, bool report_status) {
        long child = spawn_command(arguments);

        if (child < 0) {
                print_error("sh: command not found: ");
                print_error(arguments[0]);
                print_error("\n");
                return 127;
        }

        if (rose_setpgid(child, child) < 0 ||
            rose_tcsetpgrp(USER_STDIN_FILENO, child) < 0) {
                (void)rose_kill(child, USER_SIGNAL_KILL);
                (void)rose_waitpid(child, NULL, 0U);
                print_error("sh: unable to foreground command\n");
                return 1;
        }

        long children[] = {child};
        struct shell_job job;
        shell_job_initialize(&job, (uint64_t)child, children, 1U);
        shell_job_wait(&job);
        shell_restore_foreground();

        if (shell_job_is_stopped(&job)) {
                size_t identifier = shell_job_store(&job);
                if (identifier != 0U) {
                        shell_print_job(identifier, "Stopped",
                                        job.process_group);
                } else {
                        (void)rose_kill(-(int64_t)job.process_group,
                                        USER_SIGNAL_KILL);
                        job.stopped[0] = false;
                        shell_job_wait(&job);
                }
                return 128 + USER_SIGNAL_TERMINAL_STOP;
        }

        int status = job.statuses[0];

        if (report_status) {
                print("Process exited with status ");
                if (USER_WAIT_STATUS_EXITED(status)) {
                        print_exit_status(status);
                } else {
                        print_unsigned(
                            128U + USER_WAIT_STATUS_TERMINATION_SIGNAL(status));
                }
                print("\n");
        }

        return USER_WAIT_STATUS_EXITED(status)
                   ? (int)USER_WAIT_STATUS_EXIT_CODE(status)
                   : 128 + (int)USER_WAIT_STATUS_TERMINATION_SIGNAL(status);
}

static void shell_help(void) {
        print("Built-ins: cd pwd echo env setenv unsetenv jobs fg clear help "
              "exit\n");
        print("Commands: ls cat echo pwd env mkdir rm\n");
        print("Syntax: command [ARG...] [< FILE] [> FILE|>> FILE] [2> FILE] "
              "[2>&1] [| command...] [&]\n");
        print("Expansion: $NAME ${NAME} $? $$ (except in single quotes)\n");
}

static void shell_list_jobs(void) {
        bool found = false;

        for (size_t index = 0U; index < SHELL_JOB_LIMIT; index++) {
                struct shell_job *job = &shell_jobs[index];
                if (!job->used) {
                        continue;
                }
                found = true;
                shell_job_poll(job);
                if (shell_job_is_complete(job)) {
                        shell_print_job(index + 1U, "Done", job->process_group);
                        job->used = false;
                } else if (shell_job_is_stopped(job)) {
                        shell_print_job(index + 1U, "Stopped",
                                        job->process_group);
                } else {
                        shell_print_job(index + 1U, "Running",
                                        job->process_group);
                }
        }
        if (!found) {
                print("No jobs\n");
        }
}

static size_t shell_parse_job_identifier(const char *text) {
        if (*text == '%') {
                text++;
        }
        if (*text < '0' || *text > '9') {
                return 0U;
        }

        size_t value = 0U;
        while (*text >= '0' && *text <= '9') {
                value = value * 10U + (size_t)(*text - '0');
                text++;
        }
        return *text == '\0' ? value : 0U;
}

static int shell_foreground_job(int count, char **arguments) {
        if (count != 2) {
                print_error("Usage: fg JOB\n");
                return 1;
        }

        size_t identifier = shell_parse_job_identifier(arguments[1]);
        if (identifier == 0U || identifier > SHELL_JOB_LIMIT ||
            !shell_jobs[identifier - 1U].used) {
                print_error("fg: no such job\n");
                return 1;
        }

        struct shell_job *job = &shell_jobs[identifier - 1U];
        shell_job_poll(job);
        if (shell_job_is_complete(job)) {
                shell_print_job(identifier, "Done", job->process_group);
                job->used = false;
                int status = job->statuses[job->child_count - 1U];
                return USER_WAIT_STATUS_EXITED(status)
                           ? (int)USER_WAIT_STATUS_EXIT_CODE(status)
                           : 128 + (int)USER_WAIT_STATUS_TERMINATION_SIGNAL(
                                       status);
        }
        if (rose_tcsetpgrp(USER_STDIN_FILENO, (int64_t)job->process_group) <
            0) {
                print_error("fg: unable to foreground job\n");
                return 1;
        }

        for (size_t index = 0U; index < job->child_count; index++) {
                if (!job->finished[index]) {
                        job->stopped[index] = false;
                }
        }
        if (rose_kill(-(int64_t)job->process_group, USER_SIGNAL_CONTINUE) < 0) {
                shell_restore_foreground();
                print_error("fg: unable to continue job\n");
                return 1;
        }

        shell_job_wait(job);
        shell_restore_foreground();
        if (shell_job_is_complete(job)) {
                job->used = false;
                int status = job->statuses[job->child_count - 1U];
                return USER_WAIT_STATUS_EXITED(status)
                           ? (int)USER_WAIT_STATUS_EXIT_CODE(status)
                           : 128 + (int)USER_WAIT_STATUS_TERMINATION_SIGNAL(
                                       status);
        } else if (shell_job_is_stopped(job)) {
                shell_print_job(identifier, "Stopped", job->process_group);
                return 128 + USER_SIGNAL_TERMINAL_STOP;
        }
        return 1;
}

static bool shell_is_builtin(const char *name) {
        return strings_equal(name, "exit") || strings_equal(name, "help") ||
               strings_equal(name, "echo") || strings_equal(name, "clear") ||
               strings_equal(name, "pwd") || strings_equal(name, "cd") ||
               strings_equal(name, "env") || strings_equal(name, "setenv") ||
               strings_equal(name, "unsetenv") || strings_equal(name, "jobs") ||
               strings_equal(name, "fg") || strings_equal(name, "run");
}

static int shell_execute_builtin(int count, char **arguments,
                                 bool *keep_running) {
        *keep_running = true;

        if (strings_equal(arguments[0], "exit")) {
                print("Shutting down...\n");
                *keep_running = false;
                return 0;
        }
        if (strings_equal(arguments[0], "help")) {
                shell_help();
                return 0;
        }
        if (strings_equal(arguments[0], "echo")) {
                for (int index = 1; index < count; index++) {
                        print(arguments[index]);
                        if (index + 1 < count) {
                                print(" ");
                        }
                }
                print("\n");
                return 0;
        }
        if (strings_equal(arguments[0], "clear")) {
                print("\x1b[2J\x1b[H");
                return 0;
        }
        if (strings_equal(arguments[0], "pwd")) {
                char directory[SHELL_PATH_SIZE];
                if (rose_getcwd(directory, sizeof(directory)) < 0) {
                        print_error(
                            "pwd: unable to read the working directory\n");
                        return 1;
                } else {
                        print(directory);
                        print("\n");
                }
                return 0;
        }
        if (strings_equal(arguments[0], "cd")) {
                const char *directory =
                    count == 1 ? environment_value("HOME") : arguments[1];
                if (count > 2 || directory == NULL) {
                        print_error("Usage: cd [DIR]\n");
                        return 1;
                } else if (rose_chdir(directory) < 0) {
                        print_error("cd: unable to change directory: ");
                        print_error(directory);
                        print_error("\n");
                        return 1;
                }
                return 0;
        }
        if (strings_equal(arguments[0], "env")) {
                if (count != 1) {
                        print_error("Usage: env\n");
                        return 1;
                } else {
                        for (size_t index = 0U; index < shell_environment_count;
                             index++) {
                                print(shell_environment[index]);
                                print("\n");
                        }
                }
                return 0;
        }
        if (strings_equal(arguments[0], "setenv")) {
                if (count != 3 ||
                    !environment_set(arguments[1], arguments[2])) {
                        print_error("Usage: setenv NAME VALUE\n");
                        return 1;
                }
                return 0;
        }
        if (strings_equal(arguments[0], "unsetenv")) {
                if (count != 2 || !environment_name_is_valid(arguments[1])) {
                        print_error("Usage: unsetenv NAME\n");
                        return 1;
                } else {
                        environment_unset(arguments[1]);
                }
                return 0;
        }
        if (strings_equal(arguments[0], "jobs")) {
                if (count != 1) {
                        print_error("Usage: jobs\n");
                        return 1;
                } else {
                        shell_list_jobs();
                }
                return 0;
        }
        if (strings_equal(arguments[0], "fg")) {
                return shell_foreground_job(count, arguments);
        }

        /* Keep the former terminal's run command as a compatibility alias,
         * while normal shell use executes a path or PATH entry directly. */
        if (strings_equal(arguments[0], "run")) {
                char *default_arguments[] = {"/bin/hello", NULL};
                if (count == 1) {
                        return run_foreground(default_arguments, true);
                } else {
                        return run_foreground(&arguments[1], true);
                }
        }

        return run_foreground(arguments, false);
}

static bool
shell_save_standard_descriptors(struct shell_saved_descriptors *saved) {
        saved->input = (int)rose_dup(USER_STDIN_FILENO);
        if (saved->input < 0) {
                return false;
        }
        saved->output = (int)rose_dup(USER_STDOUT_FILENO);
        if (saved->output < 0) {
                (void)rose_close(saved->input);
                return false;
        }
        saved->error = (int)rose_dup(USER_STDERR_FILENO);
        if (saved->error < 0) {
                (void)rose_close(saved->input);
                (void)rose_close(saved->output);
                return false;
        }
        if (rose_set_descriptor_flags(saved->input,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0 ||
            rose_set_descriptor_flags(saved->output,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0 ||
            rose_set_descriptor_flags(saved->error,
                                      USER_DESCRIPTOR_CLOSE_ON_EXEC) != 0) {
                (void)rose_close(saved->input);
                (void)rose_close(saved->output);
                (void)rose_close(saved->error);
                return false;
        }
        return true;
}

static bool shell_restore_standard_descriptors(
    const struct shell_saved_descriptors *saved) {
        bool input_restored =
            rose_dup2(saved->input, USER_STDIN_FILENO) == USER_STDIN_FILENO;
        bool output_restored =
            rose_dup2(saved->output, USER_STDOUT_FILENO) == USER_STDOUT_FILENO;
        bool error_restored =
            rose_dup2(saved->error, USER_STDERR_FILENO) == USER_STDERR_FILENO;
        return input_restored && output_restored && error_restored;
}

static void shell_release_standard_descriptors(
    const struct shell_saved_descriptors *saved) {
        (void)rose_close(saved->input);
        (void)rose_close(saved->output);
        (void)rose_close(saved->error);
}

static bool shell_apply_redirections(const struct shell_command *command) {
        for (size_t index = 0U; index < command->redirection_count; index++) {
                const struct shell_redirection *redirection =
                    &command->redirections[index];

                if (redirection->operation == SHELL_REDIRECTION_DUPLICATE) {
                        if (rose_dup2(redirection->source,
                                      redirection->destination) !=
                            redirection->destination) {
                                print_error(
                                    "sh: unable to duplicate descriptor\n");
                                return false;
                        }
                        continue;
                }

                uint32_t flags = USER_OPEN_READ;
                if (redirection->operation != SHELL_REDIRECTION_READ) {
                        flags = USER_OPEN_WRITE | USER_OPEN_CREATE;
                        flags |=
                            redirection->operation == SHELL_REDIRECTION_APPEND
                                ? USER_OPEN_APPEND
                                : USER_OPEN_TRUNCATE;
                }
                long descriptor = rose_open(redirection->path, flags);
                if (descriptor < 0) {
                        print_error("sh: unable to open redirection: ");
                        print_error(redirection->path);
                        print_error("\n");
                        return false;
                }
                bool duplicated =
                    rose_dup2((int)descriptor, redirection->destination) ==
                    redirection->destination;
                (void)rose_close((int)descriptor);
                if (!duplicated) {
                        print_error("sh: unable to redirect descriptor\n");
                        return false;
                }
        }
        return true;
}

static int shell_run_pipeline(struct shell_pipeline *pipeline) {
        struct shell_saved_descriptors saved;
        if (!shell_save_standard_descriptors(&saved)) {
                print_error("sh: unable to save standard descriptors\n");
                return 1;
        }

        long children[SHELL_PIPELINE_LIMIT];
        size_t child_count = 0U;
        uint64_t process_group = 0U;
        int previous_read = -1;
        int result_status = 0;

        for (size_t index = 0U; index < pipeline->command_count; index++) {
                struct shell_command *command = &pipeline->commands[index];
                bool has_next = index + 1U < pipeline->command_count;
                int next_read = -1;

                if ((previous_read >= 0 && shell_command_redirects_descriptor(
                                               command, USER_STDIN_FILENO)) ||
                    (has_next && shell_command_redirects_descriptor(
                                     command, USER_STDOUT_FILENO))) {
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
                                print_error(
                                    "sh: unable to configure pipeline\n");
                                result_status = 1;
                                break;
                        }
                        next_read = descriptors[0];
                        (void)rose_close(descriptors[1]);
                }

                /* A pipeline connects descriptors first. Command redirections
                 * then run left-to-right, so `2>&1 | cat` sends both streams
                 * through the pipe while `2>&1 > file` does not. */
                if (!shell_apply_redirections(command)) {
                        result_status = 1;
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        break;
                }

                long child = spawn_command(command->arguments);
                if (child < 0) {
                        print_error("sh: command not found: ");
                        print_error(command->arguments[0]);
                        print_error("\n");
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        if (!shell_restore_standard_descriptors(&saved)) {
                                result_status = 1;
                        } else {
                                result_status = 127;
                        }
                        break;
                }
                if (!shell_restore_standard_descriptors(&saved)) {
                        print_error(
                            "sh: unable to restore standard descriptors\n");
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        result_status = 1;
                        break;
                }

                if (process_group == 0U) {
                        process_group = (uint64_t)child;
                }
                if (rose_setpgid(child, (int64_t)process_group) < 0) {
                        (void)rose_kill(child, USER_SIGNAL_KILL);
                        (void)rose_waitpid(child, NULL, 0U);
                        print_error("sh: unable to create process group\n");
                        if (next_read >= 0) {
                                (void)rose_close(next_read);
                        }
                        result_status = 1;
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

        if (child_count == 0U) {
                return result_status;
        }

        struct shell_job job;
        shell_job_initialize(&job, process_group, children, child_count);
        if (pipeline->background) {
                size_t identifier = shell_job_store(&job);
                if (identifier == 0U) {
                        print_error("sh: job table is full\n");
                        (void)rose_kill(-(int64_t)process_group,
                                        USER_SIGNAL_KILL);
                        shell_job_wait(&job);
                        return 1;
                }
                shell_print_job(identifier, "Running", process_group);
                return result_status;
        }

        if (rose_tcsetpgrp(USER_STDIN_FILENO, (int64_t)process_group) < 0) {
                print_error("sh: unable to foreground pipeline\n");
                (void)rose_kill(-(int64_t)process_group, USER_SIGNAL_KILL);
        }
        shell_job_wait(&job);
        shell_restore_foreground();

        if (shell_job_is_stopped(&job)) {
                size_t identifier = shell_job_store(&job);
                if (identifier != 0U) {
                        shell_print_job(identifier, "Stopped", process_group);
                } else {
                        print_error("sh: job table is full\n");
                        (void)rose_kill(-(int64_t)process_group,
                                        USER_SIGNAL_KILL);
                        for (size_t index = 0U; index < job.child_count;
                             index++) {
                                job.stopped[index] = false;
                        }
                        shell_job_wait(&job);
                }
                return 128 + USER_SIGNAL_TERMINAL_STOP;
        }

        if (result_status == 0) {
                int status = job.statuses[child_count - 1U];
                result_status =
                    USER_WAIT_STATUS_EXITED(status)
                        ? (int)USER_WAIT_STATUS_EXIT_CODE(status)
                        : 128 +
                              (int)USER_WAIT_STATUS_TERMINATION_SIGNAL(status);
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
                if (pipeline->background) {
                        print_error(
                            "sh: built-ins cannot run in the background\n");
                        shell_last_status = 1;
                        return true;
                }
                struct shell_saved_descriptors saved;
                if (!shell_save_standard_descriptors(&saved)) {
                        print_error(
                            "sh: unable to save standard descriptors\n");
                        shell_last_status = 1;
                        return true;
                }

                bool keep_running = true;
                if (!shell_apply_redirections(command)) {
                        (void)shell_restore_standard_descriptors(&saved);
                        print_error("sh: unable to redirect command\n");
                        shell_last_status = 1;
                } else {
                        shell_last_status = shell_execute_builtin(
                            command->argument_count, command->arguments,
                            &keep_running);
                        if (!shell_restore_standard_descriptors(&saved)) {
                                shell_last_status = 1;
                        }
                }
                shell_release_standard_descriptors(&saved);
                return keep_running;
        }

        if (pipeline->command_count > 1U) {
                for (size_t index = 0U; index < pipeline->command_count;
                     index++) {
                        const struct shell_command *item =
                            &pipeline->commands[index];
                        if ((index != 0U && shell_command_redirects_descriptor(
                                                item, USER_STDIN_FILENO)) ||
                            (index + 1U != pipeline->command_count &&
                             shell_command_redirects_descriptor(
                                 item, USER_STDOUT_FILENO))) {
                                print_error("sh: redirection conflicts with "
                                            "pipeline\n");
                                shell_last_status = 1;
                                return true;
                        }
                }
        }

        shell_last_status = shell_run_pipeline(pipeline);
        return true;
}

static void shell_initialize_job_control(void) {
        long pid = rose_getpid();
        if (pid <= 0 || rose_setpgid(0, 0) < 0) {
                print_error("sh: unable to create shell process group\n");
                return;
        }
        long process_group = rose_getpgrp();
        if (process_group <= 0) {
                print_error("sh: unable to read shell process group\n");
                return;
        }
        shell_process_group = (uint64_t)process_group;
        if (rose_tcsetpgrp(USER_STDIN_FILENO, (int64_t)shell_process_group) <
            0) {
                print_error("sh: unable to claim the console\n");
        }

        const struct user_signal_action ignored = {
            .handler = USER_SIGNAL_IGNORE,
            .flags = 0U,
        };
        (void)rose_sigaction(USER_SIGNAL_INTERRUPT, &ignored, NULL);
        (void)rose_sigaction(USER_SIGNAL_TERMINAL_STOP, &ignored, NULL);
        (void)rose_sigaction(USER_SIGNAL_BACKGROUND_READ, &ignored, NULL);
}

static void shell_terminate_jobs(void) {
        for (size_t index = 0U; index < SHELL_JOB_LIMIT; index++) {
                struct shell_job *job = &shell_jobs[index];
                if (!job->used) {
                        continue;
                }

                shell_job_poll(job);
                if (!shell_job_is_complete(job)) {
                        for (size_t child = 0U; child < job->child_count;
                             child++) {
                                job->stopped[child] = false;
                        }
                        (void)rose_kill(-(int64_t)job->process_group,
                                        USER_SIGNAL_KILL);
                        shell_job_wait(job);
                }
                job->used = false;
        }
}

int rose_shell_main(char **environment) { // NOLINT(misc-use-internal-linkage)
        environment_initialize(environment);
        shell_initialize_job_control();
        shell_last_status = 0;
        print("ROSE userspace shell\n");

        while (true) {
                char line[SHELL_LINE_SIZE];
                struct shell_pipeline pipeline;

                if (!shell_read_line(line)) {
                        print("Shutting down...\n");
                        shell_terminate_jobs();
                        return 0;
                }

                if (!shell_parse_pipeline(line, &pipeline)) {
                        print_error("sh: invalid or too long command line\n");
                        shell_last_status = 2;
                        continue;
                }
                if (!shell_execute(&pipeline)) {
                        shell_terminate_jobs();
                        return 0;
                }
        }
}
