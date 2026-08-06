#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum { FILE_LIMIT = 24, PATH_LIMIT = 64 };

struct files_state {
        char path[PATH_LIMIT];
        struct user_directory_entry entries[FILE_LIMIT];
        const char *items[FILE_LIMIT];
        size_t count;
        char status[32];
        struct rose_gui_widget root;
        struct rose_gui_widget header;
        struct rose_gui_widget body;
        struct rose_gui_widget path_bar;
        struct rose_gui_widget list;
        struct rose_gui_widget status_bar;
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

static void update_status(struct files_state *state) {
        char number[16];
        rose_gui_unsigned(number, sizeof(number), state->count);
        size_t output = 0U;
        while (number[output] != '\0' && output + 1U < sizeof(state->status)) {
                state->status[output] = number[output];
                output++;
        }
        const char suffix[] = " ITEMS  |  ARROWS SELECT, ENTER OPEN";
        for (size_t index = 0U; suffix[index] != '\0' &&
                                output + 1U < sizeof(state->status);
             index++) {
                state->status[output++] = suffix[index];
        }
        state->status[output] = '\0';
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
                state->items[state->count] = state->entries[state->count].name;
                state->count++;
        }
        (void)rose_close((int)descriptor);
        state->list.items = state->items;
        state->list.item_count = state->count;
        if (state->list.selected_index >= state->count)
                state->list.selected_index = 0U;
        update_status(state);
        return true;
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

static void list_action(struct rose_gui_widget *widget,
                        enum rose_gui_widget_action action, void *user_data) {
        struct files_state *state = user_data;
        if (action == ROSE_GUI_ACTION_SELECT)
                files_open_row(state, widget->selected_index);
}

static void files_build_ui(struct files_state *state) {
        rose_gui_widget_initialize(&state->root, ROSE_GUI_WIDGET_ROOT, NULL);
        state->root.padding = 0U;
        state->root.gap = 0U;

        rose_gui_widget_initialize(&state->header, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "FILES");
        rose_gui_widget_set_minimum(&state->header, 0, 42);

        rose_gui_widget_initialize(&state->body, ROSE_GUI_WIDGET_COLUMN, NULL);
        state->body.padding = 14U;
        state->body.gap = 10U;
        rose_gui_widget_set_flex(&state->body, 1U);

        rose_gui_widget_initialize(&state->path_bar,
                                   ROSE_GUI_WIDGET_STATUS_BAR, state->path);
        rose_gui_widget_set_minimum(&state->path_bar, 0, 28);

        rose_gui_items_initialize(&state->list, ROSE_GUI_WIDGET_LIST,
                                  state->items, state->count);
        rose_gui_widget_set_flex(&state->list, 1U);
        state->list.callback = list_action;
        state->list.user_data = state;

        rose_gui_widget_initialize(&state->status_bar,
                                   ROSE_GUI_WIDGET_STATUS_BAR, state->status);
        rose_gui_widget_set_minimum(&state->status_bar, 0, 24);

        rose_gui_widget_add(&state->root, &state->header);
        rose_gui_widget_add(&state->root, &state->body);
        rose_gui_widget_add(&state->body, &state->path_bar);
        rose_gui_widget_add(&state->body, &state->list);
        rose_gui_widget_add(&state->body, &state->status_bar);
}

int rose_gui_files_main(int argc, char **argv) {
        static struct files_state state;
        static struct rose_gui_application app;
        if (argc != 2) return 1;
        string_copy(state.path, "/", sizeof(state.path));
        state.count = 0U;
        update_status(&state);
        files_build_ui(&state);
        if (!files_load(&state) ||
            !rose_gui_application_initialize(&app, argv[1], &state.root)) {
                return 1;
        }
        rose_gui_ui_focus(&app.ui, &state.list);
        return rose_gui_application_run(&app);
}
