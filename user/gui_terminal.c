#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "rose/terminal.h"
#include "user_abi.h"

enum {
        TERMINAL_CELL_WIDTH = ROSE_GUI_FONT_WIDTH + 1,
        TERMINAL_CELL_HEIGHT = ROSE_GUI_FONT_HEIGHT + 2,
        KEY_HOME = 102,
        KEY_UP = 103,
        KEY_PAGE_UP = 104,
        KEY_LEFT = 105,
        KEY_RIGHT = 106,
        KEY_END = 107,
        KEY_DOWN = 108,
        KEY_PAGE_DOWN = 109,
        KEY_INSERT = 110,
        KEY_DELETE = 111,
};

/* ANSI's logical palette is mapped onto legible colors for the ROSE theme. */
static uint32_t terminal_palette[16];
static uint32_t terminal_cursor_active;
static uint32_t terminal_cursor_inactive;

/* Screen and scrollback storage live in BSS, leaving the small userspace stack
 * available for syscall and renderer frames. Each terminal process owns one
 * independent emulator instance. */
static struct rose_terminal terminal;
static bool terminal_output_carriage_return;

static void terminal_apply_theme(const struct rose_gui_theme *theme) {
        terminal_palette[0] = theme->background;
        terminal_palette[1] = theme->error;
        terminal_palette[2] = theme->success;
        terminal_palette[3] = theme->warning;
        terminal_palette[4] = theme->accent;
        terminal_palette[5] = UINT32_C(0x00aa55aa);
        terminal_palette[6] = UINT32_C(0x0033aaaa);
        terminal_palette[7] = theme->text;
        terminal_palette[8] = theme->muted;
        terminal_palette[9] = UINT32_C(0x00ff777f);
        terminal_palette[10] = UINT32_C(0x0066dd88);
        terminal_palette[11] = UINT32_C(0x00ffcc66);
        terminal_palette[12] = theme->accent_hover;
        terminal_palette[13] = UINT32_C(0x00ee77dd);
        terminal_palette[14] = UINT32_C(0x0066dddd);
        terminal_palette[15] = theme->on_accent;
        terminal_cursor_active = theme->focus;
        terminal_cursor_inactive = theme->muted;
}

static size_t terminal_columns_for_width(uint32_t width) {
        size_t columns = width / TERMINAL_CELL_WIDTH;
        if (columns == 0U) columns = 1U;
        if (columns > ROSE_TERMINAL_MAX_COLUMNS)
                columns = ROSE_TERMINAL_MAX_COLUMNS;
        return columns;
}

static size_t terminal_rows_for_height(uint32_t height) {
        size_t rows = height / TERMINAL_CELL_HEIGHT;
        if (rows == 0U) rows = 1U;
        if (rows > ROSE_TERMINAL_MAX_ROWS) rows = ROSE_TERMINAL_MAX_ROWS;
        return rows;
}

static void render_glyph(struct rose_gui_context *gui, size_t row,
                         size_t column,
                         const struct rose_terminal_cell *cell,
                         bool cursor, bool focused) {
        uint8_t foreground = cell->foreground & 15U;
        uint8_t background = cell->background & 15U;
        if ((cell->attributes & ROSE_TERMINAL_ATTRIBUTE_BOLD) != 0U &&
            foreground < 8U) {
                foreground += 8U;
        }
        if ((cell->attributes & ROSE_TERMINAL_ATTRIBUTE_INVERSE) != 0U) {
                uint8_t swap = foreground;
                foreground = background;
                background = swap;
        }

        int32_t x = (int32_t)(column * TERMINAL_CELL_WIDTH);
        int32_t y = (int32_t)(row * TERMINAL_CELL_HEIGHT);
        uint32_t foreground_color = terminal_palette[foreground];
        uint32_t background_color = terminal_palette[background];
        char text[2] = {(char)cell->character, '\0'};

        if (cursor && focused) {
                rose_gui_fill(gui, x, y, TERMINAL_CELL_WIDTH,
                              TERMINAL_CELL_HEIGHT, terminal_cursor_active);
                rose_gui_text(gui, x, y + 1, text, background_color, 1U);
                return;
        }

        rose_gui_fill(gui, x, y, TERMINAL_CELL_WIDTH, TERMINAL_CELL_HEIGHT,
                      background_color);
        rose_gui_text(gui, x, y + 1, text, foreground_color, 1U);
        if (cursor) {
                rose_gui_fill(gui, x, y, TERMINAL_CELL_WIDTH, 1,
                              terminal_cursor_inactive);
                rose_gui_fill(gui, x, y + TERMINAL_CELL_HEIGHT - 1,
                              TERMINAL_CELL_WIDTH, 1,
                              terminal_cursor_inactive);
                rose_gui_fill(gui, x, y, 1, TERMINAL_CELL_HEIGHT,
                              terminal_cursor_inactive);
                rose_gui_fill(gui, x + TERMINAL_CELL_WIDTH - 1, y, 1,
                              TERMINAL_CELL_HEIGHT,
                              terminal_cursor_inactive);
        }
}

static void terminal_render(struct rose_gui_context *gui,
                            struct rose_terminal *state, bool focused) {
        size_t cursor_row = 0U;
        size_t cursor_column = 0U;
        bool cursor = rose_terminal_visible_cursor(
            state, &cursor_row, &cursor_column);

        for (size_t row = 0U; row < rose_terminal_rows(state); row++) {
                size_t left;
                size_t right;
                if (!rose_terminal_take_dirty(state, row, &left, &right))
                        continue;
                for (size_t column = left; column < right; column++) {
                        const struct rose_terminal_cell *cell =
                            rose_terminal_visible_cell(state, row, column);
                        render_glyph(gui, row, column, cell,
                                     cursor && row == cursor_row &&
                                         column == cursor_column,
                                     focused);
                }
                rose_gui_present(gui,
                                 (int32_t)(left * TERMINAL_CELL_WIDTH),
                                 (int32_t)(row * TERMINAL_CELL_HEIGHT),
                                 (int32_t)((right - left) *
                                           TERMINAL_CELL_WIDTH),
                                 TERMINAL_CELL_HEIGHT);
        }
}

/* ROSE's compact terminal ABI does not yet expose POSIX OPOST/ONLCR. Programs
 * nevertheless write conventional newline-only text, so perform that output
 * postprocessing at the PTY frontend boundary instead of weakening VT line-
 * feed semantics inside the emulator. */
static void terminal_feed_output(struct rose_terminal *state,
                                 const char *bytes, size_t count) {
        for (size_t index = 0U; index < count; index++) {
                if (bytes[index] == '\n' &&
                    !terminal_output_carriage_return) {
                        rose_terminal_feed(state, "\r", 1U);
                }
                rose_terminal_feed(state, &bytes[index], 1U);
                terminal_output_carriage_return = bytes[index] == '\r';
        }
}

static bool terminal_set_window_size(int descriptor,
                                     const struct rose_gui_context *gui) {
        struct user_terminal_window_size window_size = {
            .rows = (uint16_t)rose_terminal_rows(&terminal),
            .columns = (uint16_t)rose_terminal_columns(&terminal),
            .pixel_width = (uint16_t)gui->width,
            .pixel_height = (uint16_t)gui->height,
        };
        return rose_tcsetwinsize(descriptor, &window_size) == 0;
}

static _Noreturn void terminal_child(int master, int slave,
                                     const char *program) {
        if (rose_setsid() < 0 || rose_tcsetctty(slave) < 0) {
                rose_exit(126U);
        }
        (void)rose_dup2(slave, USER_STDIN_FILENO);
        (void)rose_dup2(slave, USER_STDOUT_FILENO);
        (void)rose_dup2(slave, USER_STDERR_FILENO);
        (void)rose_close(master);
        if (slave > USER_STDERR_FILENO) {
                (void)rose_close(slave);
        }
        if (program == NULL || program[0] == '\0') program = "/bin/sh";
        char *arguments[] = {(char *)program, NULL};
        char *environment[] = {"HOME=/", "PATH=/bin:/sbin", "TERM=xterm",
                               NULL};
        (void)rose_execve(program, arguments, environment);
        rose_exit(127U);
}

static size_t sequence_length(const char *sequence) {
        size_t length = 0U;
        while (sequence[length] != '\0') length++;
        return length;
}

static void set_wait_item(struct user_wait_item *item, uint32_t type,
                          uint32_t events, int64_t identifier,
                          uint64_t value) {
        item->type = type;
        item->events = events;
        item->identifier = identifier;
        item->value = value;
        item->returned_events = 0U;
        item->reserved = 0U;
}

static const char *navigation_sequence(uint16_t code, bool application) {
        switch (code) {
        case KEY_HOME: return application ? "\033OH" : "\033[H";
        case KEY_UP: return application ? "\033OA" : "\033[A";
        case KEY_LEFT: return application ? "\033OD" : "\033[D";
        case KEY_RIGHT: return application ? "\033OC" : "\033[C";
        case KEY_END: return application ? "\033OF" : "\033[F";
        case KEY_DOWN: return application ? "\033OB" : "\033[B";
        case KEY_INSERT: return "\033[2~";
        case KEY_DELETE: return "\033[3~";
        default: return NULL;
        }
}

static void handle_key(struct rose_gui_context *gui, int master,
                       const struct user_input_event *event) {
        char character = rose_gui_key_character(gui, event);
        if (event->value == 0U) return;

        if (event->code == KEY_PAGE_UP || event->code == KEY_PAGE_DOWN) {
                rose_terminal_scroll_page(&terminal,
                                          event->code == KEY_PAGE_UP);
                return;
        }
        if (character != 0) {
                rose_terminal_jump_to_latest(&terminal);
                (void)rose_write(master, &character, 1U);
                return;
        }
        const char *sequence = navigation_sequence(
            event->code, rose_terminal_application_cursor(&terminal));
        if (sequence != NULL) {
                rose_terminal_jump_to_latest(&terminal);
                (void)rose_write(master, sequence, sequence_length(sequence));
        }
}

struct terminal_input {
        int master;
};

static void terminal_event(struct rose_gui_context *gui,
                           const struct user_input_event *event,
                           void *user_data) {
        struct terminal_input *input = user_data;
        if (event->type == USER_INPUT_EVENT_KEY)
                handle_key(gui, input->master, event);
}

int rose_gui_terminal_main(int argc, char **argv) {
        struct rose_gui_context gui;
        if ((argc != 2 && argc != 3) || !rose_gui_connect(argv[1], &gui))
                return 1;
        struct rose_gui_theme theme;
        (void)rose_gui_theme_load(&theme, "/share/gui/theme.conf");
        terminal_apply_theme(&theme);

        rose_terminal_initialize(&terminal,
                                 terminal_columns_for_width(gui.width),
                                 terminal_rows_for_height(gui.height));
        size_t scrollback_size = sizeof(struct rose_terminal_line) *
                                 ROSE_TERMINAL_SCROLLBACK_LINES;
        long scrollback_address = rose_mmap(
            scrollback_size,
            USER_MEMORY_PROTECTION_READ | USER_MEMORY_PROTECTION_WRITE);
        if (scrollback_address < 0) {
                rose_gui_disconnect(&gui);
                return 2;
        }
        rose_terminal_set_scrollback(
            &terminal, (struct rose_terminal_line *)(uintptr_t)scrollback_address,
            ROSE_TERMINAL_SCROLLBACK_LINES);
        rose_gui_fill(&gui, 0, 0, (int32_t)gui.width, (int32_t)gui.height,
                      terminal_palette[0]);
        rose_gui_present(&gui, 0, 0, (int32_t)gui.width,
                         (int32_t)gui.height);

        int terminals[2];
        if (rose_openpty(terminals) != 0) {
                (void)rose_munmap((uintptr_t)scrollback_address,
                                  scrollback_size);
                rose_gui_disconnect(&gui);
                return 2;
        }
        struct user_terminal_attributes attributes;
        if (rose_tcgetattr(terminals[1], &attributes) != 0) {
                (void)rose_close(terminals[0]);
                (void)rose_close(terminals[1]);
                (void)rose_munmap((uintptr_t)scrollback_address,
                                  scrollback_size);
                rose_gui_disconnect(&gui);
                return 2;
        }
        attributes.flags = USER_TERMINAL_SIGNALS;
        if (rose_tcsetattr(terminals[1], &attributes) != 0 ||
            !terminal_set_window_size(terminals[0], &gui)) {
                (void)rose_close(terminals[0]);
                (void)rose_close(terminals[1]);
                (void)rose_munmap((uintptr_t)scrollback_address,
                                  scrollback_size);
                rose_gui_disconnect(&gui);
                return 2;
        }

        long shell_pid = rose_fork();
        if (shell_pid == 0)
                terminal_child(terminals[0], terminals[1],
                               argc == 3 ? argv[2] : NULL);
        if (shell_pid < 0) {
                (void)rose_close(terminals[0]);
                (void)rose_close(terminals[1]);
                (void)rose_munmap((uintptr_t)scrollback_address,
                                  scrollback_size);
                rose_gui_disconnect(&gui);
                return 3;
        }
        (void)rose_close(terminals[1]);
        (void)rose_set_descriptor_flags(
            terminals[0], USER_DESCRIPTOR_NONBLOCK |
                              USER_DESCRIPTOR_CLOSE_ON_EXEC);

        static const char banner[] = "ROSE GRAPHICAL TERMINAL\r\n";
        terminal_feed_output(&terminal, banner, sizeof(banner) - 1U);
        bool focused = gui.surface->focused != 0U;
        bool child_exited = false;
        bool one_shot = argc == 3;
        struct terminal_input input = {.master = terminals[0]};
        terminal_render(&gui, &terminal, focused);

        while (gui.surface->close_requested == 0U) {
                uint32_t gui_events = rose_gui_event_loop_poll(
                    &gui, NULL, terminal_event, &input);
                if ((gui_events & ROSE_GUI_LOOP_CLOSE) != 0U) break;
                if ((gui_events & ROSE_GUI_LOOP_RESIZED) != 0U) {
                        rose_terminal_resize(
                            &terminal, terminal_columns_for_width(gui.width),
                            terminal_rows_for_height(gui.height));
                        if (!terminal_set_window_size(terminals[0], &gui))
                                break;
                }

                bool now_focused = gui.surface->focused != 0U;
                if (now_focused != focused) {
                        rose_terminal_mark_cursor_dirty(&terminal);
                        focused = now_focused;
                }

                char output[512];
                long count;
                do {
                        count = rose_read(terminals[0], output, sizeof(output));
                        if (count > 0) {
                                terminal_feed_output(&terminal, output,
                                                     (size_t)count);
                        }
                } while (count > 0);

                terminal_render(&gui, &terminal, focused);

                int status;
                long waited = child_exited
                                  ? 0
                                  : rose_waitpid(shell_pid, &status,
                                                 USER_WAIT_NO_HANG);
                if (waited == shell_pid) {
                        child_exited = true;
                        if (!one_shot) break;
                        static const char finished[] =
                            "\r\nPROGRAM FINISHED - CLOSE THIS WINDOW\r\n";
                        terminal_feed_output(&terminal, finished,
                                             sizeof(finished) - 1U);
                        terminal_render(&gui, &terminal, focused);
                }
                if (child_exited) {
                        if (rose_gui_wait(&gui, -1) < 0) break;
                        continue;
                }

                struct user_wait_item waits[6];
                set_wait_item(&waits[0], USER_WAIT_OBJECT_DESCRIPTOR,
                              USER_WAIT_EVENT_READABLE, terminals[0], 0U);
                set_wait_item(&waits[1], USER_WAIT_OBJECT_CHILD,
                              USER_WAIT_EVENT_CHILD_EXITED, shell_pid, 0U);
                set_wait_item(
                    &waits[2], USER_WAIT_OBJECT_SHARED_WORD,
                    USER_WAIT_EVENT_CHANGED,
                    (int64_t)(uintptr_t)&gui.surface->input_write,
                    gui.surface->input_write);
                set_wait_item(
                    &waits[3], USER_WAIT_OBJECT_SHARED_WORD,
                    USER_WAIT_EVENT_CHANGED,
                    (int64_t)(uintptr_t)&gui.surface->close_requested,
                    gui.surface->close_requested);
                set_wait_item(
                    &waits[4], USER_WAIT_OBJECT_SHARED_WORD,
                    USER_WAIT_EVENT_CHANGED,
                    (int64_t)(uintptr_t)&gui.surface->focused,
                    gui.surface->focused);
                set_wait_item(
                    &waits[5], USER_WAIT_OBJECT_SHARED_WORD,
                    USER_WAIT_EVENT_CHANGED,
                    (int64_t)(uintptr_t)&gui.surface->resize_sequence,
                    gui.observed_resize_sequence);
                if (rose_wait_events(waits, 6U, -1) < 0) break;
        }

        if (!child_exited) {
                (void)rose_kill(shell_pid, USER_SIGNAL_TERMINATE);
                int status;
                (void)rose_waitpid(shell_pid, &status, 0U);
        }
        (void)rose_close(terminals[0]);
        (void)rose_munmap((uintptr_t)scrollback_address, scrollback_size);
        rose_gui_disconnect(&gui);
        return 0;
}
