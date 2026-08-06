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
        SHELL_HISTORY_LIMIT = 16,
        SHELL_ALIAS_LIMIT = 8,
        SHELL_ALIAS_NAME_SIZE = 24,
        SHELL_ALIAS_VALUE_SIZE = 96,
};

enum shell_job_state {
        SHELL_JOB_RUNNING,
        SHELL_JOB_STOPPED,
        SHELL_JOB_DONE,
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
        const char *parse_error;
};

struct shell_job {
        bool used;
        uint64_t process_group;
        long children[SHELL_PIPELINE_LIMIT];
        bool finished[SHELL_PIPELINE_LIMIT];
        bool stopped[SHELL_PIPELINE_LIMIT];
        int statuses[SHELL_PIPELINE_LIMIT];
        size_t child_count;
        uint64_t generation;
        enum shell_job_state reported_state;
};

struct shell_alias {
        bool used;
        char name[SHELL_ALIAS_NAME_SIZE];
        char value[SHELL_ALIAS_VALUE_SIZE];
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
static char shell_history[SHELL_HISTORY_LIMIT][SHELL_LINE_SIZE];
static size_t shell_history_count;
static char shell_history_draft[SHELL_LINE_SIZE];
static struct shell_alias shell_aliases[SHELL_ALIAS_LIMIT];
static uint64_t shell_job_generation;

static bool shell_is_operator(char character);
static bool shell_parameter_name_start(char character);
static bool shell_parameter_name_character(char character);

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

static bool alias_name_is_valid(const char *name, size_t length) {
        if (length == 0U || !shell_parameter_name_start(name[0])) {
                return false;
        }
        for (size_t index = 1U; index < length; index++) {
                if (!shell_parameter_name_character(name[index])) {
                        return false;
                }
        }
        return length < SHELL_ALIAS_NAME_SIZE;
}

static struct shell_alias *alias_find_length(const char *name, size_t length) {
        for (size_t index = 0U; index < SHELL_ALIAS_LIMIT; index++) {
                struct shell_alias *alias = &shell_aliases[index];
                if (!alias->used || alias->name[length] != '\0') {
                        continue;
                }
                bool equal = true;
                for (size_t character = 0U; character < length; character++) {
                        if (alias->name[character] != name[character]) {
                                equal = false;
                                break;
                        }
                }
                if (equal) {
                        return alias;
                }
        }
        return NULL;
}

static struct shell_alias *alias_find(const char *name) {
        return alias_find_length(name, string_length(name));
}

static bool alias_set(const char *assignment) {
        const char *separator = assignment;
        while (*separator != '\0' && *separator != '=') {
                separator++;
        }
        if (*separator != '=') {
                return false;
        }
        size_t name_length = (size_t)(separator - assignment);
        const char *value = separator + 1;
        if (!alias_name_is_valid(assignment, name_length) ||
            string_length(value) >= SHELL_ALIAS_VALUE_SIZE) {
                return false;
        }

        struct shell_alias *alias = alias_find_length(assignment, name_length);
        if (alias == NULL) {
                for (size_t index = 0U; index < SHELL_ALIAS_LIMIT; index++) {
                        if (!shell_aliases[index].used) {
                                alias = &shell_aliases[index];
                                break;
                        }
                }
        }
        if (alias == NULL) {
                return false;
        }
        alias->used = true;
        for (size_t index = 0U; index < name_length; index++) {
                alias->name[index] = assignment[index];
        }
        alias->name[name_length] = '\0';
        copy_string(alias->value, value);
        return true;
}

static void alias_print(const struct shell_alias *alias) {
        print("alias ");
        print(alias->name);
        print("='");
        print(alias->value);
        print("'\n");
}

/* Alias substitution is deliberately limited to the command word. Repeating
 * the lookup supports short alias chains while the bound rejects cycles. */
static bool shell_expand_aliases(const char *line,
                                 char expanded[SHELL_LINE_SIZE]) {
        copy_string(expanded, line);
        for (size_t depth = 0U; depth < SHELL_ALIAS_LIMIT; depth++) {
                size_t start = 0U;
                while (expanded[start] == ' ' || expanded[start] == '\t') {
                        start++;
                }
                size_t end = start;
                while (expanded[end] != '\0' && expanded[end] != ' ' &&
                       expanded[end] != '\t' &&
                       !shell_is_operator(expanded[end])) {
                        if (expanded[end] == '\'' || expanded[end] == '"' ||
                            expanded[end] == '\\') {
                                return true;
                        }
                        end++;
                }
                struct shell_alias *alias =
                    alias_find_length(&expanded[start], end - start);
                if (alias == NULL) {
                        return true;
                }

                size_t old_length = string_length(expanded);
                size_t value_length = string_length(alias->value);
                size_t new_length = start + value_length + old_length - end;
                if (new_length >= SHELL_LINE_SIZE) {
                        return false;
                }
                char replacement[SHELL_LINE_SIZE];
                size_t output = 0U;
                for (size_t index = 0U; index < start; index++) {
                        replacement[output++] = expanded[index];
                }
                for (size_t index = 0U; index < value_length; index++) {
                        replacement[output++] = alias->value[index];
                }
                for (size_t index = end; index <= old_length; index++) {
                        replacement[output++] = expanded[index];
                }
                copy_string(expanded, replacement);
        }
        return false;
}

static void shell_redraw_line(const char line[SHELL_LINE_SIZE], size_t length,
                              size_t cursor) {
        print("\r\x1b[2Krose> ");
        for (size_t index = 0U; index < length; index++) {
                print_character(line[index]);
        }
        for (size_t index = cursor; index < length; index++) {
                print_character('\b');
        }
}

static void shell_set_edit_line(char line[SHELL_LINE_SIZE], const char *source,
                                size_t *length, size_t *cursor) {
        *length = string_length(source);
        if (*length >= SHELL_LINE_SIZE)
                *length = SHELL_LINE_SIZE - 1U;
        for (size_t index = 0U; index < *length; index++) {
                line[index] = source[index];
        }
        line[*length] = '\0';
        *cursor = *length;
}

static void shell_history_add(const char line[SHELL_LINE_SIZE], size_t length) {
        if (length == 0U) {
                return;
        }
        if (shell_history_count != 0U) {
                const char *previous = shell_history[shell_history_count - 1U];
                bool duplicate = previous[length] == '\0';

                for (size_t index = 0U; duplicate && index < length; index++) {
                        duplicate = previous[index] == line[index];
                }
                if (duplicate) {
                        return;
                }
        }
        if (shell_history_count == SHELL_HISTORY_LIMIT) {
                for (size_t index = 1U; index < SHELL_HISTORY_LIMIT; index++) {
                        copy_string(shell_history[index - 1U],
                                    shell_history[index]);
                }
                shell_history_count--;
        }
        for (size_t index = 0U; index < length; index++) {
                shell_history[shell_history_count][index] = line[index];
        }
        shell_history[shell_history_count][length] = '\0';
        shell_history_count++;
}

static bool shell_navigation(char final, size_t parameter,
                             char line[SHELL_LINE_SIZE], size_t *length,
                             size_t *cursor, size_t *history_index) {
        if (final == 'A') {
                if (shell_history_count == 0U || *history_index == 0U)
                        return false;
                if (*history_index == shell_history_count) {
                        line[*length] = '\0';
                        copy_string(shell_history_draft, line);
                }
                (*history_index)--;
                shell_set_edit_line(line, shell_history[*history_index], length,
                                    cursor);
                return true;
        }
        if (final == 'B') {
                if (*history_index >= shell_history_count)
                        return false;
                (*history_index)++;
                shell_set_edit_line(line,
                                    *history_index == shell_history_count
                                        ? shell_history_draft
                                        : shell_history[*history_index],
                                    length, cursor);
                return true;
        }
        if (final == 'C') {
                if (*cursor == *length)
                        return false;
                (*cursor)++;
                return true;
        }
        if (final == 'D') {
                if (*cursor == 0U)
                        return false;
                (*cursor)--;
                return true;
        }
        if (final == 'H' ||
            (final == '~' && (parameter == 1U || parameter == 7U))) {
                if (*cursor == 0U)
                        return false;
                *cursor = 0U;
                return true;
        }
        if (final == 'F' ||
            (final == '~' && (parameter == 4U || parameter == 8U))) {
                if (*cursor == *length)
                        return false;
                *cursor = *length;
                return true;
        }
        if (final == '~' && parameter == 3U) {
                if (*cursor == *length)
                        return false;
                for (size_t index = *cursor; index < *length; index++) {
                        line[index] = line[index + 1U];
                }
                (*length)--;
                return true;
        }
        return false;
}

/* Complete one unquoted filesystem path component. The VFS already provides
 * directory types, so a unique directory match receives '/' and a regular
 * file receives a separating space. */
static bool shell_complete_path(char line[SHELL_LINE_SIZE], size_t *length,
                                size_t *cursor) {
        size_t word_start = *cursor;
        while (word_start != 0U) {
                char previous = line[word_start - 1U];
                if (previous == ' ' || previous == '\t' ||
                    shell_is_operator(previous)) {
                        break;
                }
                if (previous == '\'' || previous == '"' || previous == '\\') {
                        return false;
                }
                word_start--;
        }

        size_t component_start = word_start;
        for (size_t index = word_start; index < *cursor; index++) {
                if (line[index] == '/') {
                        component_start = index + 1U;
                }
        }

        char directory[SHELL_PATH_SIZE];
        if (component_start == word_start) {
                directory[0] = '.';
                directory[1] = '\0';
        } else {
                size_t directory_length = component_start - word_start - 1U;
                if (directory_length == 0U) {
                        directory[0] = '/';
                        directory[1] = '\0';
                } else {
                        if (directory_length + 1U > sizeof(directory)) {
                                return false;
                        }
                        for (size_t index = 0U; index < directory_length;
                             index++) {
                                directory[index] = line[word_start + index];
                        }
                        directory[directory_length] = '\0';
                }
        }

        size_t prefix_length = *cursor - component_start;
        long descriptor =
            rose_open(directory, USER_OPEN_READ | USER_OPEN_DIRECTORY);
        if (descriptor < 0) {
                return false;
        }

        char match[USER_DIRECTORY_NAME_MAX];
        uint32_t match_type = USER_FILE_REGULAR;
        size_t match_count = 0U;
        struct user_directory_entry entry;
        while (rose_read_directory((int)descriptor, &entry) > 0) {
                if (strings_equal(entry.name, ".") ||
                    strings_equal(entry.name, "..")) {
                        continue;
                }
                bool matches = true;
                for (size_t index = 0U; index < prefix_length; index++) {
                        if (entry.name[index] == '\0' ||
                            entry.name[index] !=
                                line[component_start + index]) {
                                matches = false;
                                break;
                        }
                }
                if (matches) {
                        if (match_count == 0U) {
                                copy_string(match, entry.name);
                                match_type = entry.type;
                        }
                        match_count++;
                }
        }
        (void)rose_close((int)descriptor);
        if (match_count != 1U) {
                return false;
        }

        size_t match_length = string_length(match);
        size_t suffix_length = match_length - prefix_length;
        bool add_separator = *cursor == *length;
        size_t addition = suffix_length + (add_separator ? 1U : 0U);
        if (*length + addition >= SHELL_LINE_SIZE) {
                return false;
        }
        for (size_t index = *length + 1U; index > *cursor; index--) {
                line[index + addition - 1U] = line[index - 1U];
        }
        for (size_t index = 0U; index < suffix_length; index++) {
                line[*cursor + index] = match[prefix_length + index];
        }
        if (add_separator) {
                line[*cursor + suffix_length] =
                    match_type == USER_FILE_DIRECTORY ? '/' : ' ';
        }
        *cursor += addition;
        *length += addition;
        return true;
}

/* Read and echo one editable command line from the console. Both the serial
 * keyboard and graphical frontend deliver conventional CSI/SS3 navigation
 * sequences, so consume them here instead of inserting their printable tail. */
static bool shell_read_line(char line[SHELL_LINE_SIZE]) {
        size_t length = 0U;
        size_t cursor = 0U;
        size_t history_index = shell_history_count;
        enum {
                SHELL_INPUT_NORMAL,
                SHELL_INPUT_ESCAPE,
                SHELL_INPUT_CSI,
                SHELL_INPUT_SS3
        } input_state = SHELL_INPUT_NORMAL;
        size_t escape_parameter = 0U;
        bool escape_parameter_complete = false;

        print("rose> ");
        line[0] = '\0';
        shell_history_draft[0] = '\0';

        while (true) {
                char character;
                if (rose_read(USER_STDIN_FILENO, &character, 1U) != 1) {
                        return false;
                }

                if (input_state == SHELL_INPUT_ESCAPE) {
                        if (character == '[') {
                                input_state = SHELL_INPUT_CSI;
                                escape_parameter = 0U;
                                escape_parameter_complete = false;
                                continue;
                        }
                        if (character == 'O') {
                                input_state = SHELL_INPUT_SS3;
                                continue;
                        }
                        input_state = SHELL_INPUT_NORMAL;
                } else if (input_state == SHELL_INPUT_CSI) {
                        if (character >= '0' && character <= '9') {
                                if (!escape_parameter_complete &&
                                    escape_parameter < 100U) {
                                        escape_parameter =
                                            escape_parameter * 10U +
                                            (size_t)(character - '0');
                                }
                                continue;
                        }
                        if (character == ';') {
                                escape_parameter_complete = true;
                                continue;
                        }
                        bool changed =
                            shell_navigation(character, escape_parameter, line,
                                             &length, &cursor, &history_index);
                        input_state = SHELL_INPUT_NORMAL;
                        if (changed)
                                shell_redraw_line(line, length, cursor);
                        continue;
                } else if (input_state == SHELL_INPUT_SS3) {
                        bool changed =
                            shell_navigation(character, 0U, line, &length,
                                             &cursor, &history_index);
                        input_state = SHELL_INPUT_NORMAL;
                        if (changed)
                                shell_redraw_line(line, length, cursor);
                        continue;
                }

                if (character == '\x1b') {
                        input_state = SHELL_INPUT_ESCAPE;
                        continue;
                }

                if (character == '\r' || character == '\n') {
                        line[length] = '\0';
                        print("\n");
                        shell_history_add(line, length);
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
                        if (cursor != 0U) {
                                for (size_t index = cursor - 1U; index < length;
                                     index++) {
                                        line[index] = line[index + 1U];
                                }
                                length--;
                                cursor--;
                                shell_redraw_line(line, length, cursor);
                        }
                        continue;
                }
                if (character == '\t') {
                        if (shell_complete_path(line, &length, &cursor)) {
                                shell_redraw_line(line, length, cursor);
                        }
                        continue;
                }
                if (character < ' ' || character > '~' ||
                    length >= SHELL_LINE_SIZE - 1U) {
                        continue;
                }

                for (size_t index = length; index > cursor; index--) {
                        line[index] = line[index - 1U];
                }
                line[cursor++] = character;
                length++;
                line[length] = '\0';
                if (cursor == length) {
                        print_character(character);
                } else {
                        shell_redraw_line(line, length, cursor);
                }
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
static bool shell_parse_fail(struct shell_pipeline *pipeline,
                             const char *message) {
        pipeline->parse_error = message;
        return false;
}

static bool shell_parse_pipeline(const char *line,
                                 struct shell_pipeline *pipeline) {
        for (size_t index = 0U; index < SHELL_PIPELINE_LIMIT; index++) {
                pipeline->commands[index].argument_count = 0;
                pipeline->commands[index].arguments[0] = NULL;
                pipeline->commands[index].redirection_count = 0U;
        }
        pipeline->command_count = 0U;
        pipeline->background = false;
        pipeline->parse_error = NULL;

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
                                        return shell_parse_fail(
                                            pipeline,
                                            "unsupported file descriptor");
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
                                        return shell_parse_fail(
                                            pipeline, "unexpected '&'");
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
                                        return shell_parse_fail(
                                            pipeline, "unexpected '|'");
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
                                return shell_parse_fail(
                                    pipeline, "unexpected redirection");
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
                                        return shell_parse_fail(
                                            pipeline,
                                            "expected descriptor after '&'");
                                }
                                redirection->operation =
                                    SHELL_REDIRECTION_DUPLICATE;
                                redirection->source = *source++ - '0';
                                if ((*source >= '0' && *source <= '9') ||
                                    (*source != '\0' && *source != ' ' &&
                                     *source != '\t' &&
                                     !shell_is_operator(*source))) {
                                        return shell_parse_fail(
                                            pipeline,
                                            "invalid descriptor duplication");
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
                        return shell_parse_fail(pipeline, "too many arguments");
                }
                if (destination == storage_end) {
                        return shell_parse_fail(pipeline,
                                                "command line is too long");
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
                                        return shell_parse_fail(
                                            pipeline,
                                            *source == '\0'
                                                ? "trailing escape"
                                                : "command line is too long");
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
                                        return shell_parse_fail(
                                            pipeline,
                                            "invalid parameter expansion");
                                }
                                source = parameter;
                                continue;
                        }
                        if (destination == storage_end) {
                                return shell_parse_fail(
                                    pipeline, "command line is too long");
                        }
                        *destination++ = character;
                }

                if (quote != '\0') {
                        return shell_parse_fail(pipeline, "unterminated quote");
                }
                if (destination == storage_end) {
                        return shell_parse_fail(pipeline,
                                                "command line is too long");
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
                return shell_parse_fail(
                    pipeline, pending_redirection != SHELL_REDIRECTION_LIMIT
                                  ? "expected path after redirection"
                                  : "expected command");
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
        job->generation = ++shell_job_generation;
        job->reported_state = SHELL_JOB_RUNNING;
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

static enum shell_job_state shell_job_state_of(const struct shell_job *job) {
        if (shell_job_is_complete(job)) {
                return SHELL_JOB_DONE;
        }
        return shell_job_is_stopped(job) ? SHELL_JOB_STOPPED
                                         : SHELL_JOB_RUNNING;
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
                        destination->generation = job->generation;
                        destination->reported_state = shell_job_state_of(job);
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
        print("Built-ins: cd pwd echo export unset alias unalias type history "
              "jobs fg bg kill clear help exit\n");
        print("Commands: ls cat echo pwd env mkdir rm cp mv touch head wc find "
              "ps kill sleep ifconfig ping nslookup curl\n");
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
                        job->reported_state = SHELL_JOB_STOPPED;
                } else {
                        shell_print_job(index + 1U, "Running",
                                        job->process_group);
                        job->reported_state = SHELL_JOB_RUNNING;
                }
        }
        if (!found) {
                print("No jobs\n");
        }
}

static void shell_notify_jobs(void) {
        for (size_t index = 0U; index < SHELL_JOB_LIMIT; index++) {
                struct shell_job *job = &shell_jobs[index];
                if (!job->used) {
                        continue;
                }
                shell_job_poll(job);
                enum shell_job_state state = shell_job_state_of(job);
                if (state == job->reported_state) {
                        continue;
                }
                shell_print_job(
                    index + 1U,
                    state == SHELL_JOB_DONE
                        ? "Done"
                        : (state == SHELL_JOB_STOPPED ? "Stopped" : "Running"),
                    job->process_group);
                job->reported_state = state;
                if (state == SHELL_JOB_DONE) {
                        job->used = false;
                }
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

static size_t shell_relative_job_identifier(bool previous) {
        uint64_t newest = 0U;
        uint64_t second = 0U;
        size_t newest_identifier = 0U;
        size_t second_identifier = 0U;
        for (size_t index = 0U; index < SHELL_JOB_LIMIT; index++) {
                if (!shell_jobs[index].used) {
                        continue;
                }
                uint64_t generation = shell_jobs[index].generation;
                if (generation > newest) {
                        second = newest;
                        second_identifier = newest_identifier;
                        newest = generation;
                        newest_identifier = index + 1U;
                } else if (generation > second) {
                        second = generation;
                        second_identifier = index + 1U;
                }
        }
        return previous ? second_identifier : newest_identifier;
}

static size_t shell_select_job(const char *text) {
        if (text == NULL || strings_equal(text, "%") ||
            strings_equal(text, "%%") || strings_equal(text, "%+")) {
                return shell_relative_job_identifier(false);
        }
        if (strings_equal(text, "%-")) {
                return shell_relative_job_identifier(true);
        }
        return shell_parse_job_identifier(text);
}

static int shell_foreground_job(int count, char **arguments) {
        if (count > 2) {
                print_error("Usage: fg [JOB]\n");
                return 1;
        }

        size_t identifier = shell_select_job(count == 2 ? arguments[1] : NULL);
        if (identifier == 0U || identifier > SHELL_JOB_LIMIT ||
            !shell_jobs[identifier - 1U].used) {
                print_error("fg: no such job\n");
                return 1;
        }

        struct shell_job *job = &shell_jobs[identifier - 1U];
        job->generation = ++shell_job_generation;
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
                job->reported_state = SHELL_JOB_STOPPED;
                return 128 + USER_SIGNAL_TERMINAL_STOP;
        }
        return 1;
}

static int shell_background_job(int count, char **arguments) {
        if (count > 2) {
                print_error("Usage: bg [JOB]\n");
                return 1;
        }
        size_t identifier = shell_select_job(count == 2 ? arguments[1] : NULL);
        if (identifier == 0U || identifier > SHELL_JOB_LIMIT ||
            !shell_jobs[identifier - 1U].used) {
                print_error("bg: no such job\n");
                return 1;
        }

        struct shell_job *job = &shell_jobs[identifier - 1U];
        shell_job_poll(job);
        if (shell_job_is_complete(job)) {
                shell_print_job(identifier, "Done", job->process_group);
                job->used = false;
                return 1;
        }
        for (size_t index = 0U; index < job->child_count; index++) {
                if (!job->finished[index]) {
                        job->stopped[index] = false;
                }
        }
        if (rose_kill(-(int64_t)job->process_group, USER_SIGNAL_CONTINUE) < 0) {
                print_error("bg: unable to continue job\n");
                return 1;
        }
        job->generation = ++shell_job_generation;
        job->reported_state = SHELL_JOB_RUNNING;
        shell_print_job(identifier, "Running", job->process_group);
        return 0;
}

static bool shell_set_assignment(const char *assignment) {
        const char *separator = assignment;
        while (*separator != '\0' && *separator != '=') {
                separator++;
        }
        if (*separator != '=') {
                return false;
        }
        size_t length = (size_t)(separator - assignment);
        if (length == 0U || length >= SHELL_ENVIRONMENT_ENTRY_SIZE) {
                return false;
        }
        char name[SHELL_ENVIRONMENT_ENTRY_SIZE];
        for (size_t index = 0U; index < length; index++) {
                name[index] = assignment[index];
        }
        name[length] = '\0';
        return environment_set(name, separator + 1);
}

static bool shell_parse_positive_number(const char *text, uint64_t *value) {
        if (*text < '0' || *text > '9') {
                return false;
        }
        uint64_t result = 0U;
        while (*text >= '0' && *text <= '9') {
                uint64_t digit = (uint64_t)(*text++ - '0');
                if (result > (UINT64_MAX - digit) / 10U) {
                        return false;
                }
                result = result * 10U + digit;
        }
        if (*text != '\0' || result == 0U) {
                return false;
        }
        *value = result;
        return true;
}

static int shell_kill_builtin(int count, char **arguments) {
        int signal = USER_SIGNAL_TERMINATE;
        int first_target = 1;
        if (count >= 2 && arguments[1][0] == '-') {
                uint64_t parsed;
                if (!shell_parse_positive_number(&arguments[1][1], &parsed) ||
                    parsed > USER_SIGNAL_MAX) {
                        print_error("kill: invalid signal\n");
                        return 1;
                }
                signal = (int)parsed;
                first_target = 2;
        }
        if (first_target == count) {
                print_error("Usage: kill [-SIGNAL] PID|%JOB...\n");
                return 1;
        }

        int status = 0;
        for (int index = first_target; index < count; index++) {
                int64_t target;
                if (arguments[index][0] == '%') {
                        size_t identifier = shell_select_job(arguments[index]);
                        if (identifier == 0U || identifier > SHELL_JOB_LIMIT ||
                            !shell_jobs[identifier - 1U].used) {
                                print_error("kill: no such job\n");
                                status = 1;
                                continue;
                        }
                        target =
                            -(int64_t)shell_jobs[identifier - 1U].process_group;
                } else {
                        uint64_t pid;
                        if (!shell_parse_positive_number(arguments[index],
                                                         &pid) ||
                            pid > INT64_MAX) {
                                print_error("kill: invalid process ID\n");
                                status = 1;
                                continue;
                        }
                        target = (int64_t)pid;
                }
                if (rose_kill(target, signal) != 0) {
                        print_error("kill: unable to signal target\n");
                        status = 1;
                }
        }
        return status;
}

static bool shell_resolve_command(const char *name,
                                  char result[SHELL_PATH_SIZE]) {
        if (string_contains(name, '/')) {
                struct user_file_status status;
                if (string_length(name) >= SHELL_PATH_SIZE ||
                    rose_stat(name, &status) != 0 ||
                    status.type != USER_FILE_REGULAR) {
                        return false;
                }
                copy_string(result, name);
                return true;
        }

        const char *path = environment_value("PATH");
        if (path == NULL) {
                return false;
        }
        while (true) {
                size_t length = 0U;
                while (*path != '\0' && *path != ':') {
                        if (length + 1U >= SHELL_PATH_SIZE) {
                                length = SHELL_PATH_SIZE;
                        } else {
                                result[length++] = *path;
                        }
                        path++;
                }
                if (length != 0U && length < SHELL_PATH_SIZE) {
                        if (result[length - 1U] != '/') {
                                result[length++] = '/';
                        }
                        size_t name_length = string_length(name);
                        if (length + name_length + 1U <= SHELL_PATH_SIZE) {
                                copy_string(&result[length], name);
                                struct user_file_status status;
                                if (rose_stat(result, &status) == 0 &&
                                    status.type == USER_FILE_REGULAR) {
                                        return true;
                                }
                        }
                }
                if (*path == '\0') {
                        return false;
                }
                path++;
        }
}

static bool shell_is_builtin(const char *name) {
        return strings_equal(name, "exit") || strings_equal(name, "help") ||
               strings_equal(name, "echo") || strings_equal(name, "clear") ||
               strings_equal(name, "pwd") || strings_equal(name, "cd") ||
               strings_equal(name, "env") || strings_equal(name, "setenv") ||
               strings_equal(name, "unsetenv") ||
               strings_equal(name, "export") || strings_equal(name, "unset") ||
               strings_equal(name, "alias") || strings_equal(name, "unalias") ||
               strings_equal(name, "type") || strings_equal(name, "history") ||
               strings_equal(name, "jobs") || strings_equal(name, "fg") ||
               strings_equal(name, "bg") || strings_equal(name, "kill") ||
               strings_equal(name, "run");
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
        if (strings_equal(arguments[0], "export")) {
                if (count == 1) {
                        for (size_t index = 0U; index < shell_environment_count;
                             index++) {
                                print("export ");
                                print(shell_environment[index]);
                                print("\n");
                        }
                        return 0;
                }
                if (count == 3 && string_contains(arguments[1], '=') == false) {
                        if (!environment_set(arguments[1], arguments[2])) {
                                print_error("export: invalid assignment\n");
                                return 1;
                        }
                        return 0;
                }
                for (int index = 1; index < count; index++) {
                        if (!shell_set_assignment(arguments[index])) {
                                print_error("Usage: export NAME=VALUE...\n");
                                return 1;
                        }
                }
                return 0;
        }
        if (strings_equal(arguments[0], "unset")) {
                if (count < 2) {
                        print_error("Usage: unset NAME...\n");
                        return 1;
                }
                for (int index = 1; index < count; index++) {
                        if (!environment_name_is_valid(arguments[index])) {
                                print_error("unset: invalid name\n");
                                return 1;
                        }
                        environment_unset(arguments[index]);
                }
                return 0;
        }
        if (strings_equal(arguments[0], "alias")) {
                if (count == 1) {
                        for (size_t index = 0U; index < SHELL_ALIAS_LIMIT;
                             index++) {
                                if (shell_aliases[index].used) {
                                        alias_print(&shell_aliases[index]);
                                }
                        }
                        return 0;
                }
                int status = 0;
                for (int index = 1; index < count; index++) {
                        struct shell_alias *existing =
                            alias_find(arguments[index]);
                        if (existing != NULL) {
                                alias_print(existing);
                        } else if (!alias_set(arguments[index])) {
                                print_error("alias: expected NAME=VALUE\n");
                                status = 1;
                        }
                }
                return status;
        }
        if (strings_equal(arguments[0], "unalias")) {
                if (count < 2) {
                        print_error("Usage: unalias NAME...\n");
                        return 1;
                }
                int status = 0;
                for (int index = 1; index < count; index++) {
                        struct shell_alias *alias =
                            alias_find(arguments[index]);
                        if (alias == NULL) {
                                print_error("unalias: no such alias: ");
                                print_error(arguments[index]);
                                print_error("\n");
                                status = 1;
                        } else {
                                alias->used = false;
                        }
                }
                return status;
        }
        if (strings_equal(arguments[0], "type")) {
                if (count < 2) {
                        print_error("Usage: type NAME...\n");
                        return 1;
                }
                int status = 0;
                for (int index = 1; index < count; index++) {
                        struct shell_alias *alias =
                            alias_find(arguments[index]);
                        char resolved[SHELL_PATH_SIZE];
                        print(arguments[index]);
                        if (alias != NULL) {
                                print(" is an alias for '");
                                print(alias->value);
                                print("'\n");
                        } else if (shell_is_builtin(arguments[index])) {
                                print(" is a shell builtin\n");
                        } else if (shell_resolve_command(arguments[index],
                                                         resolved)) {
                                print(" is ");
                                print(resolved);
                                print("\n");
                        } else {
                                print(" not found\n");
                                status = 1;
                        }
                }
                return status;
        }
        if (strings_equal(arguments[0], "history")) {
                if (count != 1) {
                        print_error("Usage: history\n");
                        return 1;
                }
                for (size_t index = 0U; index < shell_history_count; index++) {
                        print_unsigned(index + 1U);
                        print("  ");
                        print(shell_history[index]);
                        print("\n");
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
        if (strings_equal(arguments[0], "bg")) {
                return shell_background_job(count, arguments);
        }
        if (strings_equal(arguments[0], "kill")) {
                return shell_kill_builtin(count, arguments);
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

static bool shell_execute_line(const char *line) {
        while (*line == ' ' || *line == '\t') {
                line++;
        }
        if (*line == '\0' || *line == '#') {
                return true;
        }

        char expanded[SHELL_LINE_SIZE];
        struct shell_pipeline pipeline;
        if (!shell_expand_aliases(line, expanded)) {
                print_error("sh: alias expansion is too long or recursive\n");
                shell_last_status = 2;
                return true;
        }
        if (!shell_parse_pipeline(expanded, &pipeline)) {
                print_error("sh: syntax error: ");
                print_error(pipeline.parse_error == NULL
                                ? "invalid command"
                                : pipeline.parse_error);
                print_error("\n");
                shell_last_status = 2;
                return true;
        }
        return shell_execute(&pipeline);
}

static bool shell_run_startup_file(const char *path) {
        long descriptor = rose_open(path, USER_OPEN_READ);
        if (descriptor < 0) {
                return true;
        }

        char line[SHELL_LINE_SIZE];
        size_t length = 0U;
        bool overflow = false;
        bool keep_running = true;
        char character;
        while (keep_running &&
               rose_read((int)descriptor, &character, 1U) == 1) {
                if (character == '\n') {
                        if (overflow) {
                                print_error("sh: startup line is too long: ");
                                print_error(path);
                                print_error("\n");
                                shell_last_status = 2;
                        } else {
                                line[length] = '\0';
                                keep_running = shell_execute_line(line);
                        }
                        length = 0U;
                        overflow = false;
                } else if (character != '\r') {
                        if (length + 1U < sizeof(line)) {
                                line[length++] = character;
                        } else {
                                overflow = true;
                        }
                }
        }
        if (keep_running && (length != 0U || overflow)) {
                if (overflow) {
                        print_error("sh: startup line is too long: ");
                        print_error(path);
                        print_error("\n");
                        shell_last_status = 2;
                } else {
                        line[length] = '\0';
                        keep_running = shell_execute_line(line);
                }
        }
        (void)rose_close((int)descriptor);
        return keep_running;
}

static bool shell_run_startup_files(void) {
        if (!shell_run_startup_file("/etc/roserc")) {
                return false;
        }
        const char *home = environment_value("HOME");
        if (home == NULL) {
                return true;
        }
        char path[SHELL_PATH_SIZE];
        size_t length = string_length(home);
        static const char suffix[] = "/.roserc";
        size_t suffix_start = length;
        if (length == 1U && home[0] == '/') {
                suffix_start = 0U;
        }
        if (suffix_start + sizeof(suffix) > sizeof(path)) {
                print_error("sh: HOME is too long for ~/.roserc\n");
                return true;
        }
        for (size_t index = 0U; index < length; index++) {
                path[index] = home[index];
        }
        for (size_t index = 0U; index < sizeof(suffix); index++) {
                path[suffix_start + index] = suffix[index];
        }
        return shell_run_startup_file(path);
}

static void shell_initialize_job_control(void) {
        long pid = rose_getpid();
        long process_group = rose_getpgrp();
        if (pid <= 0 || process_group <= 0 ||
            (process_group != pid && rose_setpgid(0, 0) < 0)) {
                print_error("sh: unable to create shell process group\n");
                return;
        }
        process_group = rose_getpgrp();
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
        if (!shell_run_startup_files()) {
                shell_terminate_jobs();
                return 0;
        }
        print("ROSE userspace shell\n");

        while (true) {
                char line[SHELL_LINE_SIZE];

                shell_notify_jobs();

                if (!shell_read_line(line)) {
                        print("Shutting down...\n");
                        shell_terminate_jobs();
                        return 0;
                }
                if (!shell_execute_line(line)) {
                        shell_terminate_jobs();
                        return 0;
                }
        }
}

#ifdef ROSE_SHELL_PARSE_TEST
/* Narrow host-test adapter. Section garbage collection drops the interactive
 * shell and leaves only the allocation-free parser plus a getpid stub. */
int rose_shell_parse_test(const char *line, size_t *command_count,
                          bool *background, const char **error) {
        char *environment[] = {"HOME=/", "PATH=/bin:/sbin", NULL};
        struct shell_pipeline pipeline;
        environment_initialize(environment);
        shell_last_status = 7;
        bool parsed = shell_parse_pipeline(line, &pipeline);
        *command_count = pipeline.command_count;
        *background = pipeline.background;
        *error = pipeline.parse_error;
        return parsed ? 0 : 1;
}
#endif
