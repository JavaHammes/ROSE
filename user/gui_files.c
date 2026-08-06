#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        FILE_LIMIT = 24,
        PATH_LIMIT = 64,
        STATUS_LIMIT = 96,
        COPY_BUFFER_SIZE = 256,
        PROPERTY_COUNT = 4,
};

enum files_operation {
        FILES_OPERATION_NONE,
        FILES_OPERATION_NEW_FOLDER,
        FILES_OPERATION_RENAME,
        FILES_OPERATION_DELETE,
        FILES_OPERATION_COPY,
        FILES_OPERATION_MOVE,
        FILES_OPERATION_PROPERTIES,
};

struct files_state {
        char path[PATH_LIMIT];
        struct user_directory_entry entries[FILE_LIMIT];
        const char *items[FILE_LIMIT];
        size_t count;
        char status[STATUS_LIMIT];
        char operation_text[PATH_LIMIT];
        char property_text[PROPERTY_COUNT][STATUS_LIMIT];
        enum files_operation operation;
        struct rose_gui_application *app;
        struct rose_gui_widget root;
        struct rose_gui_widget header;
        struct rose_gui_widget body;
        struct rose_gui_widget path_bar;
        struct rose_gui_widget toolbar_one;
        struct rose_gui_widget toolbar_two;
        struct rose_gui_widget new_folder;
        struct rose_gui_widget rename;
        struct rose_gui_widget delete;
        struct rose_gui_widget copy;
        struct rose_gui_widget move;
        struct rose_gui_widget properties;
        struct rose_gui_widget list;
        struct rose_gui_widget status_bar;
        struct rose_gui_widget dialog;
        struct rose_gui_widget dialog_input;
        struct rose_gui_widget property_labels[PROPERTY_COUNT];
        struct rose_gui_widget dialog_confirm;
        struct rose_gui_widget dialog_cancel;
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

static void string_append(char *destination, size_t limit,
                          const char *source) {
        size_t output = string_length(destination);
        size_t input = 0U;
        while (output + 1U < limit && source[input] != '\0') {
                destination[output++] = source[input++];
        }
        destination[output] = '\0';
}

static void set_status(struct files_state *state, const char *text) {
        string_copy(state->status, text, sizeof(state->status));
        if (state->app != NULL) rose_gui_ui_invalidate(&state->app->ui);
}

static void update_status(struct files_state *state) {
        char number[16];
        rose_gui_unsigned(number, sizeof(number), state->count);
        string_copy(state->status, number, sizeof(state->status));
        string_append(state->status, sizeof(state->status),
                      " ITEMS  |  ENTER OPENS");
}

static bool path_append(const char *directory, const char *name,
                        char result[PATH_LIMIT]) {
        size_t directory_length = string_length(directory);
        size_t name_length = string_length(name);
        size_t separator = directory_length == 1U ? 0U : 1U;
        if (directory_length + separator + name_length + 1U > PATH_LIMIT)
                return false;
        string_copy(result, directory, PATH_LIMIT);
        size_t output = directory_length;
        if (separator != 0U) result[output++] = '/';
        for (size_t index = 0U; index < name_length; index++)
                result[output++] = name[index];
        result[output] = '\0';
        return true;
}

static bool requested_path(struct files_state *state, const char *requested,
                           char result[PATH_LIMIT]) {
        if (requested[0] == '/') {
                if (string_length(requested) >= PATH_LIMIT) return false;
                string_copy(result, requested, PATH_LIMIT);
                return true;
        }
        return requested[0] != '\0' && path_append(state->path, requested,
                                                    result);
}

static bool selected_path(struct files_state *state, char result[PATH_LIMIT]) {
        if (state->list.selected_index >= state->count) return false;
        const char *name =
            state->entries[state->list.selected_index].name;
        if ((name[0] == '.' && name[1] == '\0') ||
            (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
                return false;
        }
        return path_append(state->path, name, result);
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
        if (state->app != NULL) rose_gui_ui_invalidate(&state->app->ui);
        return true;
}

static void hide_dialog(struct files_state *state) {
        state->dialog.state |= ROSE_GUI_STATE_HIDDEN;
        state->operation = FILES_OPERATION_NONE;
        rose_gui_ui_focus(&state->app->ui, &state->list);
        rose_gui_ui_invalidate(&state->app->ui);
}

static void set_property(struct files_state *state, size_t index,
                         const char *label, uint64_t value) {
        char number[24];
        rose_gui_unsigned(number, sizeof(number), value);
        string_copy(state->property_text[index], label, STATUS_LIMIT);
        string_append(state->property_text[index], STATUS_LIMIT, number);
}

static void show_dialog(struct files_state *state,
                        enum files_operation operation) {
        char source[PATH_LIMIT];
        const char *selected_name = NULL;
        if (operation != FILES_OPERATION_NEW_FOLDER) {
                if (!selected_path(state, source)) {
                        set_status(state, "SELECT A FILE OR FOLDER FIRST");
                        return;
                }
                selected_name =
                    state->entries[state->list.selected_index].name;
        }
        state->operation = operation;
        state->dialog_input.state &= ~ROSE_GUI_STATE_HIDDEN;
        state->dialog_cancel.state &= ~ROSE_GUI_STATE_HIDDEN;
        for (size_t index = 0U; index < PROPERTY_COUNT; index++)
                state->property_labels[index].state |= ROSE_GUI_STATE_HIDDEN;
        state->operation_text[0] = '\0';
        state->dialog_confirm.text = "OK";

        switch (operation) {
        case FILES_OPERATION_NEW_FOLDER:
                state->dialog.text = "CREATE FOLDER";
                state->dialog_confirm.text = "CREATE";
                break;
        case FILES_OPERATION_RENAME:
                state->dialog.text = "RENAME TO";
                state->dialog_confirm.text = "RENAME";
                string_copy(state->operation_text, selected_name,
                            sizeof(state->operation_text));
                break;
        case FILES_OPERATION_DELETE:
                state->dialog.text = "DELETE SELECTED ITEM?";
                state->dialog_confirm.text = "DELETE";
                state->dialog_input.state |= ROSE_GUI_STATE_HIDDEN;
                break;
        case FILES_OPERATION_COPY:
                state->dialog.text = "COPY TO PATH OR NAME";
                state->dialog_confirm.text = "COPY";
                break;
        case FILES_OPERATION_MOVE:
                state->dialog.text = "MOVE TO PATH OR NAME";
                state->dialog_confirm.text = "MOVE";
                break;
        case FILES_OPERATION_PROPERTIES: {
                struct user_file_status status;
                if (rose_stat(source, &status) != 0) {
                        set_status(state, "UNABLE TO READ PROPERTIES");
                        state->operation = FILES_OPERATION_NONE;
                        return;
                }
                state->dialog.text = "PROPERTIES";
                state->dialog_input.state |= ROSE_GUI_STATE_HIDDEN;
                state->dialog_cancel.state |= ROSE_GUI_STATE_HIDDEN;
                state->dialog_confirm.text = "CLOSE";
                string_copy(state->property_text[0], "NAME: ", STATUS_LIMIT);
                string_append(state->property_text[0], STATUS_LIMIT,
                              selected_name);
                string_copy(state->property_text[1], "TYPE: ", STATUS_LIMIT);
                string_append(state->property_text[1], STATUS_LIMIT,
                              status.type == USER_FILE_DIRECTORY ? "FOLDER"
                                                                 : "FILE");
                set_property(state, 2U, "SIZE: ", status.size);
                set_property(state, 3U, "MODE / INODE: ", status.mode);
                char inode[16];
                rose_gui_unsigned(inode, sizeof(inode), status.inode);
                string_append(state->property_text[3], STATUS_LIMIT, " / ");
                string_append(state->property_text[3], STATUS_LIMIT, inode);
                for (size_t index = 0U; index < PROPERTY_COUNT; index++)
                        state->property_labels[index].state &=
                            ~ROSE_GUI_STATE_HIDDEN;
                break;
        }
        case FILES_OPERATION_NONE: return;
        }
        state->dialog_input.text_length = string_length(state->operation_text);
        state->dialog.state &= ~ROSE_GUI_STATE_HIDDEN;
        rose_gui_ui_layout(
            &state->app->ui,
            (struct rose_gui_rectangle){0, 0,
                                        (int32_t)state->app->context.width,
                                        (int32_t)state->app->context.height});
        rose_gui_ui_focus(
            &state->app->ui,
            (state->dialog_input.state & ROSE_GUI_STATE_HIDDEN) == 0U
                ? &state->dialog_input
                : &state->dialog_confirm);
}

static bool copy_file(const char *source_path, const char *destination_path) {
        struct user_file_status status;
        if (rose_stat(source_path, &status) != 0 ||
            status.type != USER_FILE_REGULAR ||
            rose_stat(destination_path, &status) == 0) {
                return false;
        }
        long source = rose_open(source_path, USER_OPEN_READ);
        if (source < 0) return false;
        long destination = rose_open(destination_path, USER_OPEN_WRITE |
                                                           USER_OPEN_CREATE);
        if (destination < 0) {
                (void)rose_close((int)source);
                return false;
        }
        char buffer[COPY_BUFFER_SIZE];
        bool success = true;
        while (true) {
                long count = rose_read((int)source, buffer, sizeof(buffer));
                if (count < 0) {
                        success = false;
                        break;
                }
                if (count == 0) break;
                size_t written = 0U;
                while (written < (size_t)count) {
                        long result = rose_write((int)destination,
                                                 &buffer[written],
                                                 (size_t)count - written);
                        if (result <= 0) {
                                success = false;
                                break;
                        }
                        written += (size_t)result;
                }
                if (!success) break;
        }
        if (rose_close((int)source) != 0 ||
            rose_close((int)destination) != 0) {
                success = false;
        }
        if (!success) (void)rose_unlink(destination_path);
        return success;
}

static bool copy_path(const char *source_path, const char *destination_path,
                      size_t depth) {
        struct user_file_status source_status;
        struct user_file_status destination_status;
        if (depth > 8U || rose_stat(source_path, &source_status) != 0 ||
            rose_stat(destination_path, &destination_status) == 0) {
                return false;
        }
        if (source_status.type == USER_FILE_REGULAR)
                return copy_file(source_path, destination_path);
        if (source_status.type != USER_FILE_DIRECTORY ||
            rose_mkdir(destination_path) != 0) {
                return false;
        }
        long descriptor = rose_open(source_path,
                                    USER_OPEN_READ | USER_OPEN_DIRECTORY);
        if (descriptor < 0) return false;
        bool success = true;
        struct user_directory_entry entry;
        long read_result;
        while ((read_result =
                    rose_read_directory((int)descriptor, &entry)) > 0) {
                if ((entry.name[0] == '.' && entry.name[1] == '\0') ||
                    (entry.name[0] == '.' && entry.name[1] == '.' &&
                     entry.name[2] == '\0')) {
                        continue;
                }
                char child_source[PATH_LIMIT];
                char child_destination[PATH_LIMIT];
                if (!path_append(source_path, entry.name, child_source) ||
                    !path_append(destination_path, entry.name,
                                 child_destination) ||
                    !copy_path(child_source, child_destination, depth + 1U)) {
                        success = false;
                        break;
                }
        }
        if (read_result < 0) success = false;
        if (rose_close((int)descriptor) != 0) success = false;
        return success;
}

static void dialog_action(struct rose_gui_widget *widget,
                          enum rose_gui_widget_action action,
                          void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct files_state *state = user_data;
        if (widget == &state->dialog_cancel ||
            state->operation == FILES_OPERATION_PROPERTIES) {
                hide_dialog(state);
                return;
        }
        char source[PATH_LIMIT];
        char destination[PATH_LIMIT];
        bool success = false;
        switch (state->operation) {
        case FILES_OPERATION_NEW_FOLDER:
                success = requested_path(state, state->operation_text,
                                         destination) &&
                          rose_mkdir(destination) == 0;
                break;
        case FILES_OPERATION_RENAME:
        case FILES_OPERATION_MOVE:
                success = selected_path(state, source) &&
                          requested_path(state, state->operation_text,
                                         destination) &&
                          rose_rename(source, destination) == 0;
                break;
        case FILES_OPERATION_DELETE:
                success = selected_path(state, source) &&
                          rose_unlink(source) == 0;
                break;
        case FILES_OPERATION_COPY:
                success = selected_path(state, source) &&
                          requested_path(state, state->operation_text,
                                         destination) &&
                          copy_path(source, destination, 0U);
                break;
        case FILES_OPERATION_NONE:
        case FILES_OPERATION_PROPERTIES: break;
        }
        hide_dialog(state);
        if (success) {
                (void)files_load(state);
                set_status(state, "OPERATION COMPLETED");
        } else {
                set_status(state, "OPERATION FAILED");
        }
}

static void toolbar_action(struct rose_gui_widget *widget,
                           enum rose_gui_widget_action action,
                           void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct files_state *state = user_data;
        enum files_operation operation = FILES_OPERATION_NONE;
        if (widget == &state->new_folder)
                operation = FILES_OPERATION_NEW_FOLDER;
        else if (widget == &state->rename)
                operation = FILES_OPERATION_RENAME;
        else if (widget == &state->delete)
                operation = FILES_OPERATION_DELETE;
        else if (widget == &state->copy)
                operation = FILES_OPERATION_COPY;
        else if (widget == &state->move)
                operation = FILES_OPERATION_MOVE;
        else if (widget == &state->properties)
                operation = FILES_OPERATION_PROPERTIES;
        show_dialog(state, operation);
}

static void files_open_row(struct files_state *state, size_t row) {
        if (row >= state->count) return;
        const char *name = state->entries[row].name;
        if (state->entries[row].type == USER_FILE_DIRECTORY) {
                if (name[0] == '.' && name[1] == '\0') return;
                if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
                        size_t length = string_length(state->path);
                        while (length > 1U && state->path[length - 1U] != '/')
                                length--;
                        if (length > 1U) length--;
                        state->path[length] = '\0';
                        if (length == 0U)
                                string_copy(state->path, "/", PATH_LIMIT);
                } else {
                        char next[PATH_LIMIT];
                        if (!path_append(state->path, name, next)) return;
                        string_copy(state->path, next, PATH_LIMIT);
                }
                (void)files_load(state);
                return;
        }
        char path[PATH_LIMIT];
        if (!path_append(state->path, name, path) ||
            !rose_gui_open_path(&state->app->context, path)) {
                set_status(state, "NO APPLICATION FOR SELECTED FILE");
        }
}

static void list_action(struct rose_gui_widget *widget,
                        enum rose_gui_widget_action action, void *user_data) {
        if (action == ROSE_GUI_ACTION_SELECT)
                files_open_row(user_data, widget->selected_index);
}

static void files_build_ui(struct files_state *state) {
        rose_gui_widget_initialize(&state->root, ROSE_GUI_WIDGET_ROOT, NULL);
        state->root.padding = 0U;
        state->root.gap = 0U;
        rose_gui_widget_initialize(&state->header, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "FILES");
        rose_gui_widget_set_minimum(&state->header, 0, 36);
        rose_gui_widget_initialize(&state->body, ROSE_GUI_WIDGET_COLUMN, NULL);
        state->body.padding = 10U;
        state->body.gap = 7U;
        rose_gui_widget_set_flex(&state->body, 1U);
        rose_gui_widget_initialize(&state->path_bar,
                                   ROSE_GUI_WIDGET_STATUS_BAR, state->path);
        rose_gui_widget_set_minimum(&state->path_bar, 0, 24);
        rose_gui_widget_initialize(&state->toolbar_one, ROSE_GUI_WIDGET_ROW,
                                   NULL);
        rose_gui_widget_initialize(&state->toolbar_two, ROSE_GUI_WIDGET_ROW,
                                   NULL);
        state->toolbar_one.padding = state->toolbar_two.padding = 0U;
        state->toolbar_one.gap = state->toolbar_two.gap = 5U;
        rose_gui_widget_set_minimum(&state->toolbar_one, 0, 28);
        rose_gui_widget_set_minimum(&state->toolbar_two, 0, 28);
        struct rose_gui_widget *buttons[] = {
            &state->new_folder, &state->rename,     &state->delete,
            &state->copy,       &state->move,       &state->properties,
        };
        const char *labels[] = {"NEW FOLDER", "RENAME", "DELETE",
                                "COPY",       "MOVE",   "PROPERTIES"};
        for (size_t index = 0U; index < 6U; index++) {
                rose_gui_widget_initialize(buttons[index],
                                           ROSE_GUI_WIDGET_BUTTON,
                                           labels[index]);
                rose_gui_widget_set_minimum(buttons[index], 0, 28);
                rose_gui_widget_set_flex(buttons[index], 1U);
                buttons[index]->callback = toolbar_action;
                buttons[index]->user_data = state;
                rose_gui_widget_add(index < 3U ? &state->toolbar_one
                                               : &state->toolbar_two,
                                    buttons[index]);
        }
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
        rose_gui_widget_add(&state->body, &state->toolbar_one);
        rose_gui_widget_add(&state->body, &state->toolbar_two);
        rose_gui_widget_add(&state->body, &state->list);
        rose_gui_widget_add(&state->body, &state->status_bar);

        rose_gui_widget_initialize(&state->dialog, ROSE_GUI_WIDGET_DIALOG,
                                   "FILE OPERATION");
        state->dialog.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        state->dialog.bounds = (struct rose_gui_rectangle){20, 96, 300, 270};
        state->dialog.padding = 30U;
        state->dialog.gap = 8U;
        state->dialog.state |= ROSE_GUI_STATE_HIDDEN;
        rose_gui_text_field_initialize(&state->dialog_input,
                                       state->operation_text,
                                       sizeof(state->operation_text));
        rose_gui_widget_add(&state->dialog, &state->dialog_input);
        for (size_t index = 0U; index < PROPERTY_COUNT; index++) {
                rose_gui_widget_initialize(&state->property_labels[index],
                                           ROSE_GUI_WIDGET_LABEL,
                                           state->property_text[index]);
                state->property_labels[index].state |= ROSE_GUI_STATE_HIDDEN;
                rose_gui_widget_set_minimum(&state->property_labels[index],
                                            0, 18);
                rose_gui_widget_add(&state->dialog,
                                    &state->property_labels[index]);
        }
        rose_gui_widget_initialize(&state->dialog_confirm,
                                   ROSE_GUI_WIDGET_BUTTON, "OK");
        rose_gui_widget_initialize(&state->dialog_cancel,
                                   ROSE_GUI_WIDGET_BUTTON, "CANCEL");
        state->dialog_confirm.callback = dialog_action;
        state->dialog_cancel.callback = dialog_action;
        state->dialog_confirm.user_data = state;
        state->dialog_cancel.user_data = state;
        rose_gui_widget_add(&state->dialog, &state->dialog_confirm);
        rose_gui_widget_add(&state->dialog, &state->dialog_cancel);
        rose_gui_widget_add(&state->root, &state->dialog);
}

int rose_gui_files_main(int argc, char **argv) {
        static struct files_state state;
        static struct rose_gui_application app;
        if (argc != 2) return 1;
        string_copy(state.path, "/", sizeof(state.path));
        update_status(&state);
        files_build_ui(&state);
        if (!rose_gui_application_initialize(&app, argv[1], &state.root))
                return 1;
        state.app = &app;
        if (!files_load(&state)) {
                rose_gui_disconnect(&app.context);
                return 1;
        }
        rose_gui_ui_focus(&app.ui, &state.list);
        return rose_gui_application_run(&app);
}
