#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum { FILE_LIMIT = 24, PATH_LIMIT = 64, FILE_ROW_HEIGHT = 24 };

#define FILE_BACKGROUND UINT32_C(0x00f4f7fb)
#define FILE_TEXT UINT32_C(0x001b2940)
#define FILE_MUTED UINT32_C(0x0065798f)
#define FILE_ACCENT UINT32_C(0x005a6ff0)
#define FILE_DIRECTORY UINT32_C(0x00f0b84f)
#define FILE_ROW UINT32_C(0x00e8edf5)

struct files_state {
        char path[PATH_LIMIT];
        struct user_directory_entry entries[FILE_LIMIT];
        size_t count;
};

static size_t string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') length++;
        return length;
}

static void string_copy(char *destination, const char *source, size_t limit) {
        size_t index = 0U;
        while (index + 1U < limit && source[index] != '\0') {
                destination[index] = source[index];
                index++;
        }
        destination[index] = '\0';
}

static bool files_load(struct files_state *state) {
        long descriptor = rose_open(state->path,
                                    USER_OPEN_READ | USER_OPEN_DIRECTORY);
        if (descriptor < 0) return false;
        state->count = 0U;
        while (state->count < FILE_LIMIT) {
                long result = rose_read_directory(
                    (int)descriptor, &state->entries[state->count]);
                if (result <= 0) break;
                state->count++;
        }
        (void)rose_close((int)descriptor);
        return true;
}

static void files_render(struct rose_gui_context *gui,
                         const struct files_state *state) {
        rose_gui_fill(gui, 0, 0, (int32_t)gui->width, (int32_t)gui->height,
                      FILE_BACKGROUND);
        rose_gui_fill(gui, 0, 0, (int32_t)gui->width, 46, FILE_ACCENT);
        rose_gui_text(gui, 16, 12, "FILES", UINT32_C(0x00ffffff), 2U);
        rose_gui_fill(gui, 14, 58, (int32_t)gui->width - 28, 30, FILE_ROW);
        rose_gui_text(gui, 24, 68, state->path, FILE_TEXT, 1U);
        for (size_t index = 0U; index < state->count; index++) {
                int32_t y = 102 + (int32_t)index * FILE_ROW_HEIGHT;
                if (y + FILE_ROW_HEIGHT > (int32_t)gui->height) break;
                if ((index & 1U) != 0U) {
                        rose_gui_fill(gui, 14, y - 4,
                                      (int32_t)gui->width - 28,
                                      FILE_ROW_HEIGHT, FILE_ROW);
                }
                uint32_t color = state->entries[index].type ==
                                         USER_FILE_DIRECTORY
                                     ? FILE_DIRECTORY
                                     : FILE_MUTED;
                rose_gui_fill(gui, 24, y, 12, 12, color);
                rose_gui_text(gui, 48, y + 2, state->entries[index].name,
                              FILE_TEXT, 1U);
        }
        rose_gui_present(gui, 0, 0, (int32_t)gui->width,
                         (int32_t)gui->height);
}

static void files_open_row(struct files_state *state, size_t row) {
        if (row >= state->count ||
            state->entries[row].type != USER_FILE_DIRECTORY) {
                return;
        }
        const char *name = state->entries[row].name;
        if (name[0] == '.' && name[1] == '\0') return;
        if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
                size_t length = string_length(state->path);
                while (length > 1U && state->path[length - 1U] != '/') length--;
                if (length > 1U) length--;
                state->path[length] = '\0';
                if (length == 0U) string_copy(state->path, "/", PATH_LIMIT);
        } else {
                size_t length = string_length(state->path);
                size_t name_length = string_length(name);
                size_t separator = length == 1U ? 0U : 1U;
                if (length + separator + name_length + 1U > PATH_LIMIT) return;
                if (separator != 0U) state->path[length++] = '/';
                for (size_t index = 0U; index < name_length; index++) {
                        state->path[length++] = name[index];
                }
                state->path[length] = '\0';
        }
        (void)files_load(state);
}

int rose_gui_files_main(int argc, char **argv) {
        struct rose_gui_context gui;
        if (argc != 2 || !rose_gui_connect(argv[1], &gui)) return 1;
        struct files_state state;
        string_copy(state.path, "/", sizeof(state.path));
        state.count = 0U;
        (void)files_load(&state);
        files_render(&gui, &state);

        bool pressed = false;
        while (gui.surface->close_requested == 0U) {
                struct user_input_event event;
                while (rose_gui_poll_event(&gui, &event)) {
                        if (event.type != USER_INPUT_EVENT_POINTER) continue;
                        bool now =
                            (event.buttons & USER_POINTER_BUTTON_LEFT) != 0U;
                        if (!pressed && now && event.y >= 98) {
                                size_t row =
                                    (size_t)(event.y - 98) / FILE_ROW_HEIGHT;
                                files_open_row(&state, row);
                                files_render(&gui, &state);
                        }
                        pressed = now;
                }
                if (rose_gui_wait(&gui, -1) < 0) {
                        rose_gui_disconnect(&gui);
                        return 2;
                }
        }
        rose_gui_disconnect(&gui);
        return 0;
}
