#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        EDITOR_CAPACITY = 16384,
        EDITOR_CLIPBOARD_CAPACITY = 4096,
        EDITOR_PATH_CAPACITY = 64,
        EDITOR_STATUS_CAPACITY = 96,
        EDITOR_CELL_WIDTH = 6,
        EDITOR_LINE_HEIGHT = 10,
        KEY_BACKSPACE = 14,
        KEY_ENTER = 28,
        KEY_A = 30,
        KEY_S = 31,
        KEY_Z = 44,
        KEY_X = 45,
        KEY_C = 46,
        KEY_V = 47,
        KEY_CAPS_LOCK = 58,
        KEY_DELETE = 111,
        KEY_HOME = 102,
        KEY_UP = 103,
        KEY_LEFT = 105,
        KEY_RIGHT = 106,
        KEY_END = 107,
        KEY_DOWN = 108,
};

struct editor_state {
        char text[EDITOR_CAPACITY];
        char undo_text[EDITOR_CAPACITY];
        char clipboard[EDITOR_CLIPBOARD_CAPACITY];
        char path[EDITOR_PATH_CAPACITY];
        char status_text[EDITOR_STATUS_CAPACITY];
        size_t length;
        size_t cursor;
        size_t anchor;
        size_t undo_length;
        size_t clipboard_length;
        size_t scroll_line;
        bool undo_available;
        bool dirty;
        bool pointer_selecting;
        bool running;
        struct rose_gui_application app;
        struct rose_gui_widget root;
        struct rose_gui_widget header;
        struct rose_gui_widget body;
        struct rose_gui_widget file_row;
        struct rose_gui_widget path_field;
        struct rose_gui_widget open_button;
        struct rose_gui_widget save_button;
        struct rose_gui_widget edit_row;
        struct rose_gui_widget undo_button;
        struct rose_gui_widget cut_button;
        struct rose_gui_widget copy_button;
        struct rose_gui_widget paste_button;
        struct rose_gui_widget editor;
        struct rose_gui_widget status;
        struct rose_gui_widget close_dialog;
        struct rose_gui_widget close_save;
        struct rose_gui_widget close_discard;
        struct rose_gui_widget close_cancel;
};

static size_t string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') length++;
        return length;
}

static void string_copy(char *destination, size_t capacity,
                        const char *source) {
        size_t index = 0U;
        while (index + 1U < capacity && source[index] != '\0') {
                destination[index] = source[index];
                index++;
        }
        destination[index] = '\0';
}

static void string_append(char *destination, size_t capacity,
                          const char *source) {
        size_t output = string_length(destination);
        size_t input = 0U;
        while (output + 1U < capacity && source[input] != '\0')
                destination[output++] = source[input++];
        destination[output] = '\0';
}

static void update_status(struct editor_state *state, const char *message) {
        char number[24];
        rose_gui_unsigned(number, sizeof(number), state->length);
        string_copy(state->status_text, sizeof(state->status_text), number);
        string_append(state->status_text, sizeof(state->status_text),
                      state->dirty ? " BYTES | MODIFIED" : " BYTES | SAVED");
        if (message != NULL) {
                string_append(state->status_text, sizeof(state->status_text),
                              " | ");
                string_append(state->status_text, sizeof(state->status_text),
                              message);
        }
        rose_gui_ui_invalidate(&state->app.ui);
}

static size_t selection_start(const struct editor_state *state) {
        return state->cursor < state->anchor ? state->cursor : state->anchor;
}

static size_t selection_end(const struct editor_state *state) {
        return state->cursor > state->anchor ? state->cursor : state->anchor;
}

static void remember_undo(struct editor_state *state) {
        for (size_t index = 0U; index <= state->length; index++)
                state->undo_text[index] = state->text[index];
        state->undo_length = state->length;
        state->undo_available = true;
}

static void erase_selection(struct editor_state *state) {
        size_t start = selection_start(state);
        size_t end = selection_end(state);
        if (start == end) return;
        for (size_t index = end; index <= state->length; index++)
                state->text[start + index - end] = state->text[index];
        state->length -= end - start;
        state->cursor = state->anchor = start;
}

static void insert_bytes(struct editor_state *state, const char *bytes,
                         size_t count) {
        size_t selected = selection_end(state) - selection_start(state);
        if (count > EDITOR_CAPACITY - 1U - (state->length - selected)) return;
        remember_undo(state);
        erase_selection(state);
        for (size_t index = state->length + 1U; index > state->cursor; index--)
                state->text[index + count - 1U] = state->text[index - 1U];
        for (size_t index = 0U; index < count; index++)
                state->text[state->cursor + index] = bytes[index];
        state->cursor += count;
        state->anchor = state->cursor;
        state->length += count;
        state->dirty = true;
        update_status(state, NULL);
}

static void delete_selection_or_character(struct editor_state *state,
                                          bool backward) {
        if (state->cursor == state->anchor &&
            ((backward && state->cursor == 0U) ||
             (!backward && state->cursor == state->length))) {
                return;
        }
        remember_undo(state);
        if (state->cursor != state->anchor) {
                erase_selection(state);
        } else {
                size_t remove = backward ? state->cursor - 1U : state->cursor;
                for (size_t index = remove + 1U; index <= state->length;
                     index++)
                        state->text[index - 1U] = state->text[index];
                state->length--;
                state->cursor = remove;
                state->anchor = remove;
        }
        state->dirty = true;
        update_status(state, NULL);
}

static void copy_selection(struct editor_state *state) {
        size_t start = selection_start(state);
        size_t count = selection_end(state) - start;
        if (count >= sizeof(state->clipboard))
                count = sizeof(state->clipboard) - 1U;
        for (size_t index = 0U; index < count; index++)
                state->clipboard[index] = state->text[start + index];
        state->clipboard[count] = '\0';
        state->clipboard_length = count;
        update_status(state, count == 0U ? "NOTHING SELECTED" : "COPIED");
}

static void undo(struct editor_state *state) {
        if (!state->undo_available) return;
        for (size_t index = 0U; index <= state->undo_length; index++)
                state->text[index] = state->undo_text[index];
        state->length = state->undo_length;
        state->cursor = state->anchor = state->length;
        state->undo_available = false;
        state->dirty = true;
        update_status(state, "UNDONE");
}

static bool editor_load(struct editor_state *state) {
        long descriptor = rose_open(state->path, USER_OPEN_READ);
        if (descriptor < 0) {
                update_status(state, "OPEN FAILED");
                return false;
        }
        size_t length = 0U;
        bool success = true;
        while (length + 1U < sizeof(state->text)) {
                long count = rose_read((int)descriptor, &state->text[length],
                                       sizeof(state->text) - length - 1U);
                if (count < 0) {
                        success = false;
                        break;
                }
                if (count == 0) break;
                length += (size_t)count;
        }
        if (rose_close((int)descriptor) != 0) success = false;
        if (!success) {
                update_status(state, "OPEN FAILED");
                return false;
        }
        state->text[length] = '\0';
        state->length = length;
        state->cursor = state->anchor = 0U;
        state->scroll_line = 0U;
        state->dirty = false;
        state->undo_available = false;
        state->path_field.text_length = string_length(state->path);
        update_status(state, "OPENED");
        return true;
}

static bool editor_save(struct editor_state *state) {
        if (state->path[0] == '\0') {
                state->path_field.state |= ROSE_GUI_STATE_ERROR;
                update_status(state, "ENTER A PATH");
                return false;
        }
        long descriptor = rose_open(state->path, USER_OPEN_WRITE |
                                                     USER_OPEN_CREATE |
                                                     USER_OPEN_TRUNCATE);
        if (descriptor < 0) {
                update_status(state, "SAVE FAILED");
                return false;
        }
        size_t written = 0U;
        while (written < state->length) {
                long count = rose_write((int)descriptor,
                                        &state->text[written],
                                        state->length - written);
                if (count <= 0) break;
                written += (size_t)count;
        }
        bool success = written == state->length &&
                       rose_close((int)descriptor) == 0;
        if (success) state->dirty = false;
        update_status(state, success ? "SAVED" : "SAVE FAILED");
        return success;
}

static void cursor_line_column(const struct editor_state *state, size_t index,
                               size_t *line, size_t *column) {
        *line = 0U;
        *column = 0U;
        for (size_t position = 0U; position < index; position++) {
                if (state->text[position] == '\n') {
                        (*line)++;
                        *column = 0U;
                } else {
                        (*column)++;
                }
        }
}

static size_t line_column_index(const struct editor_state *state,
                                size_t wanted_line, size_t wanted_column) {
        size_t line = 0U;
        size_t index = 0U;
        while (index < state->length && line < wanted_line) {
                if (state->text[index++] == '\n') line++;
        }
        size_t column = 0U;
        while (index < state->length && state->text[index] != '\n' &&
               column < wanted_column) {
                index++;
                column++;
        }
        return index;
}

static void ensure_cursor_visible(struct editor_state *state) {
        size_t line;
        size_t column;
        cursor_line_column(state, state->cursor, &line, &column);
        (void)column;
        size_t visible = state->editor.bounds.height > 8
                             ? (size_t)(state->editor.bounds.height - 8) /
                                   EDITOR_LINE_HEIGHT
                             : 1U;
        if (line < state->scroll_line) state->scroll_line = line;
        if (line >= state->scroll_line + visible)
                state->scroll_line = line - visible + 1U;
}

static void editor_draw(struct rose_gui_widget *widget,
                        struct rose_gui_canvas *canvas,
                        const struct rose_gui_theme *theme, void *user_data) {
        struct editor_state *state = user_data;
        rose_gui_canvas_fill(canvas, widget->bounds.x, widget->bounds.y,
                             widget->bounds.width, widget->bounds.height,
                             theme->surface);
        rose_gui_canvas_border(canvas, widget->bounds, 1, theme->border);
        size_t selection_left = selection_start(state);
        size_t selection_right = selection_end(state);
        size_t line = 0U;
        size_t column = 0U;
        for (size_t index = 0U; index <= state->length; index++) {
                if (line >= state->scroll_line) {
                        int32_t x = widget->bounds.x + 5 +
                                    (int32_t)column * EDITOR_CELL_WIDTH;
                        int32_t y = widget->bounds.y + 4 +
                                    (int32_t)(line - state->scroll_line) *
                                        EDITOR_LINE_HEIGHT;
                        if (y + EDITOR_LINE_HEIGHT >
                            widget->bounds.y + widget->bounds.height)
                                break;
                        if (index < state->length &&
                            index >= selection_left &&
                            index < selection_right) {
                                rose_gui_canvas_fill(
                                    canvas, x, y, EDITOR_CELL_WIDTH,
                                    EDITOR_LINE_HEIGHT, theme->selected);
                        }
                        if (index == state->cursor &&
                            (widget->state & ROSE_GUI_STATE_FOCUSED) != 0U) {
                                rose_gui_canvas_line(
                                    canvas, x, y, x,
                                    y + EDITOR_LINE_HEIGHT - 1, theme->focus);
                        }
                        if (index < state->length &&
                            state->text[index] != '\n') {
                                char glyph[2] = {state->text[index], '\0'};
                                rose_gui_canvas_text(canvas, x, y + 1, glyph,
                                                     theme->text, 1U);
                        }
                }
                if (index == state->length) break;
                if (state->text[index] == '\n') {
                        line++;
                        column = 0U;
                } else {
                        column++;
                }
        }
        if ((widget->state & ROSE_GUI_STATE_FOCUSED) != 0U)
                rose_gui_canvas_border(canvas, widget->bounds, 2, theme->focus);
}

static size_t pointer_index(struct editor_state *state, int32_t x, int32_t y) {
        int32_t relative_x = x - state->editor.bounds.x - 5;
        int32_t relative_y = y - state->editor.bounds.y - 4;
        size_t column = relative_x > 0
                            ? (size_t)relative_x / EDITOR_CELL_WIDTH
                            : 0U;
        size_t line = state->scroll_line +
                      (relative_y > 0
                           ? (size_t)relative_y / EDITOR_LINE_HEIGHT
                           : 0U);
        return line_column_index(state, line, column);
}

static void move_vertical(struct editor_state *state, int direction) {
        size_t line;
        size_t column;
        cursor_line_column(state, state->cursor, &line, &column);
        if (direction < 0 && line != 0U) line--;
        if (direction > 0) line++;
        state->cursor = line_column_index(state, line, column);
}

static void editor_key(struct editor_state *state,
                       const struct user_input_event *event) {
        if (event->value == 0 || state->app.ui.focused != &state->editor ||
            (state->close_dialog.state & ROSE_GUI_STATE_HIDDEN) == 0U)
                return;
        bool shift = state->app.context.shift;
        bool control = state->app.context.control;
        if (control) {
                if (event->code == KEY_A) {
                        state->anchor = 0U;
                        state->cursor = state->length;
                } else if (event->code == KEY_C) {
                        copy_selection(state);
                } else if (event->code == KEY_X) {
                        if (state->cursor != state->anchor) {
                                copy_selection(state);
                                delete_selection_or_character(state, false);
                        }
                } else if (event->code == KEY_V) {
                        insert_bytes(state, state->clipboard,
                                     state->clipboard_length);
                } else if (event->code == KEY_Z) {
                        undo(state);
                } else if (event->code == KEY_S) {
                        (void)editor_save(state);
                }
                rose_gui_ui_invalidate(&state->app.ui);
                return;
        }
        if (!shift) state->anchor = state->cursor;
        switch (event->code) {
        case KEY_LEFT:
                if (state->cursor != 0U) state->cursor--;
                break;
        case KEY_RIGHT:
                if (state->cursor < state->length) state->cursor++;
                break;
        case KEY_UP: move_vertical(state, -1); break;
        case KEY_DOWN: move_vertical(state, 1); break;
        case KEY_HOME: {
                size_t line;
                size_t column;
                cursor_line_column(state, state->cursor, &line, &column);
                state->cursor = line_column_index(state, line, 0U);
                break;
        }
        case KEY_END: {
                size_t line;
                size_t column;
                cursor_line_column(state, state->cursor, &line, &column);
                state->cursor = line_column_index(state, line, SIZE_MAX);
                break;
        }
        case KEY_BACKSPACE:
                delete_selection_or_character(state, true);
                return;
        case KEY_DELETE:
                delete_selection_or_character(state, false);
                return;
        case KEY_ENTER: {
                const char newline = '\n';
                insert_bytes(state, &newline, 1U);
                return;
        }
        case KEY_CAPS_LOCK: return;
        default: {
                char character = rose_gui_key_character(&state->app.context,
                                                        event);
                if (character >= 32 && character <= 126)
                        insert_bytes(state, &character, 1U);
                return;
        }
        }
        if (!shift) state->anchor = state->cursor;
        ensure_cursor_visible(state);
        rose_gui_ui_invalidate(&state->app.ui);
}

static void editor_event(struct rose_gui_context *context,
                         const struct user_input_event *event,
                         void *user_data) {
        (void)context;
        struct editor_state *state = user_data;
        if (event->type == USER_INPUT_EVENT_KEY) {
                editor_key(state, event);
                return;
        }
        if (event->type != USER_INPUT_EVENT_POINTER ||
            (state->close_dialog.state & ROSE_GUI_STATE_HIDDEN) == 0U)
                return;
        bool inside = event->x >= state->editor.bounds.x &&
                      event->y >= state->editor.bounds.y &&
                      event->x < state->editor.bounds.x +
                                     state->editor.bounds.width &&
                      event->y < state->editor.bounds.y +
                                     state->editor.bounds.height;
        bool pressed =
            (event->buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        if (pressed && inside) {
                size_t index = pointer_index(state, event->x, event->y);
                if (!state->pointer_selecting) {
                        state->anchor = index;
                        state->pointer_selecting = true;
                        rose_gui_ui_focus(&state->app.ui, &state->editor);
                }
                state->cursor = index;
                rose_gui_ui_invalidate(&state->app.ui);
        } else if (!pressed) {
                state->pointer_selecting = false;
        }
}

static void file_action(struct rose_gui_widget *widget,
                        enum rose_gui_widget_action action, void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct editor_state *state = user_data;
        if (widget == &state->open_button)
                (void)editor_load(state);
        else
                (void)editor_save(state);
        rose_gui_ui_focus(&state->app.ui, &state->editor);
}

static void edit_action(struct rose_gui_widget *widget,
                        enum rose_gui_widget_action action, void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct editor_state *state = user_data;
        if (widget == &state->undo_button) {
                undo(state);
        } else if (widget == &state->copy_button) {
                copy_selection(state);
        } else if (widget == &state->cut_button) {
                if (state->cursor != state->anchor) {
                        copy_selection(state);
                        delete_selection_or_character(state, false);
                }
        } else if (widget == &state->paste_button) {
                insert_bytes(state, state->clipboard,
                             state->clipboard_length);
        }
        rose_gui_ui_focus(&state->app.ui, &state->editor);
}

static void close_action(struct rose_gui_widget *widget,
                         enum rose_gui_widget_action action, void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct editor_state *state = user_data;
        if (widget == &state->close_save) {
                if (editor_save(state)) state->running = false;
        } else if (widget == &state->close_discard) {
                state->running = false;
        } else {
                state->close_dialog.state |= ROSE_GUI_STATE_HIDDEN;
                rose_gui_ui_focus(&state->app.ui, &state->editor);
                rose_gui_ui_invalidate(&state->app.ui);
        }
}

static void show_close_dialog(struct editor_state *state) {
        state->app.context.surface->close_requested = 0U;
        state->close_dialog.state &= ~ROSE_GUI_STATE_HIDDEN;
        rose_gui_ui_focus(&state->app.ui, &state->close_cancel);
        rose_gui_ui_invalidate(&state->app.ui);
}

static void editor_build_ui(struct editor_state *state) {
        rose_gui_widget_initialize(&state->root, ROSE_GUI_WIDGET_ROOT, NULL);
        state->root.padding = 0U;
        state->root.gap = 0U;
        rose_gui_widget_initialize(&state->header, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "EDITOR");
        rose_gui_widget_set_minimum(&state->header, 0, 36);
        rose_gui_widget_initialize(&state->body, ROSE_GUI_WIDGET_COLUMN, NULL);
        state->body.padding = 9U;
        state->body.gap = 6U;
        rose_gui_widget_set_flex(&state->body, 1U);
        rose_gui_widget_initialize(&state->file_row, ROSE_GUI_WIDGET_ROW, NULL);
        state->file_row.padding = 0U;
        state->file_row.gap = 6U;
        rose_gui_widget_set_minimum(&state->file_row, 0, 28);
        rose_gui_text_field_initialize(&state->path_field, state->path,
                                       sizeof(state->path));
        rose_gui_widget_set_flex(&state->path_field, 1U);
        rose_gui_widget_initialize(&state->open_button,
                                   ROSE_GUI_WIDGET_BUTTON, "OPEN");
        rose_gui_widget_initialize(&state->save_button,
                                   ROSE_GUI_WIDGET_BUTTON, "SAVE");
        state->open_button.callback = state->save_button.callback = file_action;
        state->open_button.user_data = state->save_button.user_data = state;
        rose_gui_widget_add(&state->file_row, &state->path_field);
        rose_gui_widget_add(&state->file_row, &state->open_button);
        rose_gui_widget_add(&state->file_row, &state->save_button);

        rose_gui_widget_initialize(&state->edit_row, ROSE_GUI_WIDGET_ROW, NULL);
        state->edit_row.padding = 0U;
        state->edit_row.gap = 6U;
        rose_gui_widget_set_minimum(&state->edit_row, 0, 28);
        struct rose_gui_widget *buttons[] = {
            &state->undo_button, &state->cut_button, &state->copy_button,
            &state->paste_button};
        const char *labels[] = {"UNDO", "CUT", "COPY", "PASTE"};
        for (size_t index = 0U; index < 4U; index++) {
                rose_gui_widget_initialize(buttons[index],
                                           ROSE_GUI_WIDGET_BUTTON,
                                           labels[index]);
                rose_gui_widget_set_flex(buttons[index], 1U);
                buttons[index]->callback = edit_action;
                buttons[index]->user_data = state;
                rose_gui_widget_add(&state->edit_row, buttons[index]);
        }
        rose_gui_widget_initialize(&state->editor, ROSE_GUI_WIDGET_CUSTOM,
                                   NULL);
        state->editor.flags |= ROSE_GUI_WIDGET_FOCUSABLE;
        state->editor.custom_draw = editor_draw;
        state->editor.user_data = state;
        rose_gui_widget_set_minimum(&state->editor, 0, 120);
        rose_gui_widget_set_flex(&state->editor, 1U);
        rose_gui_widget_initialize(&state->status, ROSE_GUI_WIDGET_STATUS_BAR,
                                   state->status_text);
        rose_gui_widget_set_minimum(&state->status, 0, 24);
        rose_gui_widget_add(&state->root, &state->header);
        rose_gui_widget_add(&state->root, &state->body);
        rose_gui_widget_add(&state->body, &state->file_row);
        rose_gui_widget_add(&state->body, &state->edit_row);
        rose_gui_widget_add(&state->body, &state->editor);
        rose_gui_widget_add(&state->body, &state->status);

        rose_gui_widget_initialize(&state->close_dialog,
                                   ROSE_GUI_WIDGET_DIALOG,
                                   "SAVE UNSAVED CHANGES?");
        state->close_dialog.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        state->close_dialog.bounds =
            (struct rose_gui_rectangle){130, 110, 300, 190};
        state->close_dialog.padding = 34U;
        state->close_dialog.gap = 9U;
        state->close_dialog.state |= ROSE_GUI_STATE_HIDDEN;
        rose_gui_widget_initialize(&state->close_save,
                                   ROSE_GUI_WIDGET_BUTTON, "SAVE");
        rose_gui_widget_initialize(&state->close_discard,
                                   ROSE_GUI_WIDGET_BUTTON, "DISCARD");
        rose_gui_widget_initialize(&state->close_cancel,
                                   ROSE_GUI_WIDGET_BUTTON, "CANCEL");
        state->close_save.callback = state->close_discard.callback =
            state->close_cancel.callback = close_action;
        state->close_save.user_data = state->close_discard.user_data =
            state->close_cancel.user_data = state;
        rose_gui_widget_add(&state->close_dialog, &state->close_save);
        rose_gui_widget_add(&state->close_dialog, &state->close_discard);
        rose_gui_widget_add(&state->close_dialog, &state->close_cancel);
        rose_gui_widget_add(&state->root, &state->close_dialog);
}

int rose_gui_editor_main(int argc, char **argv) {
        static struct editor_state state;
        if (argc != 2 && argc != 3) return 1;
        if (argc == 3) string_copy(state.path, sizeof(state.path), argv[2]);
        editor_build_ui(&state);
        if (!rose_gui_application_initialize(&state.app, argv[1], &state.root))
                return 1;
        state.running = true;
        state.text[0] = '\0';
        state.length = 0U;
        state.dirty = false;
        if (argc == 3) (void)editor_load(&state);
        update_status(&state, argc == 3 ? NULL : "NEW DOCUMENT");
        rose_gui_ui_focus(&state.app.ui, &state.editor);
        rose_gui_application_render(&state.app);
        while (state.running) {
                uint32_t events = rose_gui_event_loop_poll(
                    &state.app.context, &state.app.ui, editor_event, &state);
                if ((events & ROSE_GUI_LOOP_CLOSE) != 0U ||
                    state.app.context.surface->close_requested != 0U) {
                        if (state.dirty) {
                                if ((state.close_dialog.state &
                                     ROSE_GUI_STATE_HIDDEN) != 0U)
                                        show_close_dialog(&state);
                        } else {
                                break;
                        }
                }
                ensure_cursor_visible(&state);
                rose_gui_application_render(&state.app);
                if (state.running &&
                    rose_gui_wait(&state.app.context, -1) < 0) {
                        rose_gui_disconnect(&state.app.context);
                        return 2;
                }
        }
        rose_gui_disconnect(&state.app.context);
        return 0;
}
