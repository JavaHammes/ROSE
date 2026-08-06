#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int rose_shell_parse_test(const char *line, size_t *command_count,
                          bool *background, const char **error);

long rose_getpid(void) { return 42; }

static int expect_success(const char *line, size_t expected_commands,
                          bool expected_background) {
        size_t commands = 0U;
        bool background = false;
        const char *error = NULL;
        if (rose_shell_parse_test(line, &commands, &background, &error) != 0 ||
            commands != expected_commands ||
            background != expected_background || error != NULL) {
                fprintf(stderr, "expected parse success: %s\n", line);
                return 1;
        }
        return 0;
}

static int expect_error(const char *line, const char *expected_error) {
        size_t commands = 0U;
        bool background = false;
        const char *error = NULL;
        if (rose_shell_parse_test(line, &commands, &background, &error) == 0 ||
            error == NULL || strcmp(error, expected_error) != 0) {
                fprintf(stderr, "expected '%s' for: %s; got: %s\n",
                        expected_error, line, error == NULL ? "(none)" : error);
                return 1;
        }
        return 0;
}

int main(void) {
        int status = 0;
        status |= expect_success("echo 'hello world' | cat > out &", 2U, true);
        status |= expect_success("echo ${HOME} $? $$", 1U, false);
        status |= expect_success("cat 2>&1", 1U, false);
        status |= expect_error("| cat", "unexpected '|'");
        status |= expect_error("echo hi |", "expected command");
        status |= expect_error("echo >", "expected path after redirection");
        status |= expect_error("echo 'missing", "unterminated quote");
        status |= expect_error("echo trailing\\", "trailing escape");
        status |= expect_error("echo ${BROKEN", "invalid parameter expansion");
        if (status == 0) {
                puts("Shell parser tests passed");
        }
        return status;
}
