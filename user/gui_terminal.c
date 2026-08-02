#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        TERMINAL_COLUMNS = 92,
        TERMINAL_ROWS = 40,
        TERMINAL_LEFT = 12,
        TERMINAL_TOP = 22,
        TERMINAL_LINE_HEIGHT = 9,
};

#define TERMINAL_BACKGROUND UINT32_C(0x00101624)
#define TERMINAL_FOREGROUND UINT32_C(0x00d9e7f2)
#define TERMINAL_ACCENT UINT32_C(0x004de2b4)
#define TERMINAL_MUTED UINT32_C(0x005f7189)

struct terminal_state {
        char cells[TERMINAL_ROWS][TERMINAL_COLUMNS];
        size_t row;
        size_t column;
        bool escape;
};

/* The terminal grid deliberately lives in BSS: userspace stacks are one page
 * and should remain available for syscall and rendering call frames. */
static struct terminal_state terminal;

static void terminal_clear_row(struct terminal_state *terminal, size_t row) {
        for (size_t column = 0U; column < TERMINAL_COLUMNS; column++) {
                terminal->cells[row][column] = ' ';
        }
}

static void terminal_initialize(struct terminal_state *terminal) {
        terminal->row = 0U;
        terminal->column = 0U;
        terminal->escape = false;
        for (size_t row = 0U; row < TERMINAL_ROWS; row++) {
                terminal_clear_row(terminal, row);
        }
}

static void terminal_newline(struct terminal_state *terminal) {
        terminal->column = 0U;
        if (terminal->row + 1U < TERMINAL_ROWS) {
                terminal->row++;
                return;
        }
        for (size_t row = 1U; row < TERMINAL_ROWS; row++) {
                for (size_t column = 0U; column < TERMINAL_COLUMNS; column++) {
                        terminal->cells[row - 1U][column] =
                            terminal->cells[row][column];
                }
        }
        terminal_clear_row(terminal, TERMINAL_ROWS - 1U);
}

static void terminal_put(struct terminal_state *terminal, char character) {
        if (terminal->escape) {
                if ((character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z')) {
                        terminal->escape = false;
                }
                return;
        }
        if (character == 27) {
                terminal->escape = true;
        } else if (character == '\r') {
                terminal->column = 0U;
        } else if (character == '\n') {
                terminal_newline(terminal);
        } else if (character == '\b' || character == 127) {
                if (terminal->column != 0U) {
                        terminal->column--;
                        terminal->cells[terminal->row][terminal->column] = ' ';
                }
        } else if (character == '\t') {
                size_t next = (terminal->column + 8U) & ~7U;
                while (terminal->column < next &&
                       terminal->column < TERMINAL_COLUMNS) {
                        terminal->cells[terminal->row][terminal->column++] =
                            ' ';
                }
        } else if (character >= ' ') {
                if (terminal->column == TERMINAL_COLUMNS) {
                        terminal_newline(terminal);
                }
                terminal->cells[terminal->row][terminal->column++] = character;
        }
}

static void terminal_render(struct rose_gui_context *gui,
                            const struct terminal_state *terminal) {
        rose_gui_fill(gui, 0, 0, (int32_t)gui->width, (int32_t)gui->height,
                      TERMINAL_BACKGROUND);
        rose_gui_fill(gui, 0, 0, (int32_t)gui->width, 4, TERMINAL_ACCENT);
        rose_gui_text(gui, 12, 9, "ROSE SHELL  /  PTY", TERMINAL_MUTED, 1U);
        char line[TERMINAL_COLUMNS + 1U];
        for (size_t row = 0U; row < TERMINAL_ROWS; row++) {
                size_t last = TERMINAL_COLUMNS;
                while (last != 0U && terminal->cells[row][last - 1U] == ' ') {
                        last--;
                }
                for (size_t column = 0U; column < last; column++) {
                        line[column] = terminal->cells[row][column];
                }
                line[last] = '\0';
                rose_gui_text(gui, TERMINAL_LEFT,
                              TERMINAL_TOP +
                                  (int32_t)row * TERMINAL_LINE_HEIGHT,
                              line, TERMINAL_FOREGROUND, 1U);
        }
        if (gui->surface->focused != 0U) {
                int32_t cursor_x =
                    TERMINAL_LEFT + (int32_t)terminal->column * 6;
                int32_t cursor_y =
                    TERMINAL_TOP + (int32_t)terminal->row * TERMINAL_LINE_HEIGHT;
                rose_gui_fill(gui, cursor_x, cursor_y + 7, 5, 2,
                              TERMINAL_ACCENT);
        }
        rose_gui_present(gui, 0, 0, (int32_t)gui->width,
                         (int32_t)gui->height);
}

static _Noreturn void terminal_child(int master, int slave) {
        (void)rose_dup2(slave, USER_STDIN_FILENO);
        (void)rose_dup2(slave, USER_STDOUT_FILENO);
        (void)rose_dup2(slave, USER_STDERR_FILENO);
        (void)rose_close(master);
        if (slave > USER_STDERR_FILENO) {
                (void)rose_close(slave);
        }
        char *arguments[] = {"/bin/sh", NULL};
        char *environment[] = {"HOME=/", "PATH=/bin:/sbin",
                               "TERM=rose-gui", NULL};
        (void)rose_execve("/bin/sh", arguments, environment);
        rose_exit(127U);
}

int rose_gui_terminal_main(int argc, char **argv) {
        struct rose_gui_context gui;
        if (argc != 2 || !rose_gui_connect(argv[1], &gui)) {
                return 1;
        }
        int terminals[2];
        if (rose_openpty(terminals) != 0) {
                rose_gui_disconnect(&gui);
                return 2;
        }
        long shell_pid = rose_fork();
        if (shell_pid == 0) {
                terminal_child(terminals[0], terminals[1]);
        }
        if (shell_pid < 0) {
                (void)rose_close(terminals[0]);
                (void)rose_close(terminals[1]);
                rose_gui_disconnect(&gui);
                return 3;
        }
        (void)rose_close(terminals[1]);
        (void)rose_set_descriptor_flags(
            terminals[0], USER_DESCRIPTOR_NONBLOCK |
                              USER_DESCRIPTOR_CLOSE_ON_EXEC);

        terminal_initialize(&terminal);
        const char banner[] = "ROSE GRAPHICAL TERMINAL\n";
        for (size_t index = 0U; index < sizeof(banner) - 1U; index++) {
                terminal_put(&terminal, banner[index]);
        }
        terminal_render(&gui, &terminal);

        while (gui.surface->close_requested == 0U) {
                bool changed = false;
                char output[256];
                long count = rose_read(terminals[0], output, sizeof(output));
                if (count > 0) {
                        for (long index = 0; index < count; index++) {
                                terminal_put(&terminal, output[index]);
                        }
                        changed = true;
                }

                struct user_input_event event;
                while (rose_gui_poll_event(&gui, &event)) {
                        if (event.type != USER_INPUT_EVENT_KEY) {
                                continue;
                        }
                        char character = rose_gui_key_character(&gui, &event);
                        if (character != 0) {
                                (void)rose_write(terminals[0], &character, 1U);
                        } else if (event.value != 0 && event.code >= 103U &&
                                   event.code <= 108U) {
                                const char *sequence = NULL;
                                if (event.code == 103U) sequence = "\033[A";
                                if (event.code == 108U) sequence = "\033[B";
                                if (event.code == 106U) sequence = "\033[C";
                                if (event.code == 105U) sequence = "\033[D";
                                if (sequence != NULL) {
                                        (void)rose_write(terminals[0], sequence,
                                                         3U);
                                }
                        }
                }
                if (changed) {
                        terminal_render(&gui, &terminal);
                }
                int status;
                long waited = rose_waitpid(shell_pid, &status,
                                            USER_WAIT_NO_HANG);
                if (waited == shell_pid) {
                        break;
                }
                struct user_wait_item waits[4] = {
                    {.type = USER_WAIT_OBJECT_DESCRIPTOR,
                     .events = USER_WAIT_EVENT_READABLE,
                     .identifier = terminals[0]},
                    {.type = USER_WAIT_OBJECT_CHILD,
                     .events = USER_WAIT_EVENT_CHILD_EXITED,
                     .identifier = shell_pid},
                    {.type = USER_WAIT_OBJECT_SHARED_WORD,
                     .events = USER_WAIT_EVENT_CHANGED,
                     .identifier =
                         (int64_t)(uintptr_t)&gui.surface->input_write,
                     .value = gui.surface->input_write},
                    {.type = USER_WAIT_OBJECT_SHARED_WORD,
                     .events = USER_WAIT_EVENT_CHANGED,
                     .identifier =
                         (int64_t)(uintptr_t)&gui.surface->close_requested,
                     .value = gui.surface->close_requested},
                };
                if (rose_wait_events(waits, 4U, -1) < 0) {
                        break;
                }
        }

        (void)rose_kill(shell_pid, USER_SIGNAL_TERMINATE);
        int status;
        (void)rose_waitpid(shell_pid, &status, 0U);
        (void)rose_close(terminals[0]);
        rose_gui_disconnect(&gui);
        return 0;
}
