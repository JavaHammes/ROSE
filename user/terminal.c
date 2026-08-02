#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/terminal.h"

enum terminal_parser_state {
        TERMINAL_PARSER_GROUND,
        TERMINAL_PARSER_ESCAPE,
        TERMINAL_PARSER_CSI,
        TERMINAL_PARSER_OSC,
        TERMINAL_PARSER_OSC_ESCAPE,
        TERMINAL_PARSER_STRING,
        TERMINAL_PARSER_STRING_ESCAPE,
};

enum { TERMINAL_SEQUENCE_LIMIT = 128 };

static struct rose_terminal_cell blank_cell(const struct rose_terminal *terminal) {
        return (struct rose_terminal_cell){
            .character = ' ',
            .foreground = terminal->foreground,
            .background = terminal->background,
            .attributes = terminal->attributes,
        };
}

static struct rose_terminal_cell (*active_cells(struct rose_terminal *terminal))
    [ROSE_TERMINAL_MAX_COLUMNS] {
        return terminal->alternate ? terminal->alternate_cells
                                   : terminal->cells;
}

static const struct rose_terminal_cell (*
active_cells_const(const struct rose_terminal *terminal))
    [ROSE_TERMINAL_MAX_COLUMNS] {
        return terminal->alternate ? terminal->alternate_cells
                                   : terminal->cells;
}

static void copy_cell(struct rose_terminal_cell *destination,
                      const struct rose_terminal_cell *source) {
        *destination = *source;
}

static void mark_dirty(struct rose_terminal *terminal, size_t row,
                       size_t left, size_t right) {
        if (row >= terminal->rows || left >= terminal->columns) return;
        if (right > terminal->columns) right = terminal->columns;
        if (right <= left) return;
        if (terminal->dirty_left[row] > left)
                terminal->dirty_left[row] = (uint16_t)left;
        if (terminal->dirty_right[row] < right)
                terminal->dirty_right[row] = (uint16_t)right;
}

static void mark_all_dirty(struct rose_terminal *terminal) {
        for (size_t row = 0U; row < terminal->rows; row++) {
                terminal->dirty_left[row] = 0U;
                terminal->dirty_right[row] = terminal->columns;
        }
}

static void clear_row_range(struct rose_terminal *terminal, size_t row,
                            size_t left, size_t right) {
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        struct rose_terminal_cell blank = blank_cell(terminal);
        if (right > terminal->columns) right = terminal->columns;
        for (size_t column = left; column < right; column++) {
                cells[row][column] = blank;
        }
        mark_dirty(terminal, row, left, right);
}

static void mark_cursor_dirty(struct rose_terminal *terminal) {
        if (terminal->view_offset == 0U) {
                mark_dirty(terminal, terminal->cursor_row,
                           terminal->cursor_column,
                           (size_t)terminal->cursor_column + 1U);
        }
}

static void set_cursor(struct rose_terminal *terminal, size_t row,
                       size_t column) {
        mark_cursor_dirty(terminal);
        if (row >= terminal->rows) row = terminal->rows - 1U;
        if (column >= terminal->columns) column = terminal->columns - 1U;
        terminal->cursor_row = (uint16_t)row;
        terminal->cursor_column = (uint16_t)column;
        terminal->wrap_pending = false;
        mark_cursor_dirty(terminal);
}

static void save_cursor(struct rose_terminal *terminal) {
        terminal->saved_row = terminal->cursor_row;
        terminal->saved_column = terminal->cursor_column;
        terminal->saved_foreground = terminal->foreground;
        terminal->saved_background = terminal->background;
        terminal->saved_attributes = terminal->attributes;
}

static void restore_cursor(struct rose_terminal *terminal) {
        terminal->foreground = terminal->saved_foreground;
        terminal->background = terminal->saved_background;
        terminal->attributes = terminal->saved_attributes;
        set_cursor(terminal, terminal->saved_row, terminal->saved_column);
}

static void add_scrollback(struct rose_terminal *terminal,
                           const struct rose_terminal_cell *cells) {
        if (terminal->scrollback == NULL ||
            terminal->scrollback_capacity == 0U) {
                return;
        }
        size_t slot;
        bool full = terminal->scrollback_count ==
                    terminal->scrollback_capacity;
        if (full) {
                slot = terminal->scrollback_start;
                terminal->scrollback_start =
                    (uint16_t)((terminal->scrollback_start + 1U) %
                               terminal->scrollback_capacity);
        } else {
                slot = (terminal->scrollback_start +
                       terminal->scrollback_count) %
                       terminal->scrollback_capacity;
                terminal->scrollback_count++;
        }
        for (size_t column = 0U; column < terminal->columns; column++) {
                terminal->scrollback[slot].cells[column] = cells[column];
        }
        terminal->scrollback[slot].width = terminal->columns;
        if (terminal->view_offset != 0U &&
            terminal->view_offset < terminal->scrollback_count) {
                terminal->view_offset++;
        } else if (terminal->view_offset != 0U && !full) {
                terminal->view_offset = terminal->scrollback_count;
        }
}

static void scroll_up(struct rose_terminal *terminal, size_t top,
                      size_t bottom, size_t count) {
        if (top > bottom || bottom >= terminal->rows) return;
        size_t height = bottom - top + 1U;
        if (count == 0U) count = 1U;
        if (count > height) count = height;
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        if (!terminal->alternate && top == 0U &&
            bottom + 1U == terminal->rows) {
                for (size_t row = 0U; row < count; row++) {
                        add_scrollback(terminal, cells[row]);
                }
        }
        for (size_t row = top; row + count <= bottom; row++) {
                for (size_t column = 0U; column < terminal->columns;
                     column++) {
                        copy_cell(&cells[row][column],
                                  &cells[row + count][column]);
                }
        }
        struct rose_terminal_cell blank = blank_cell(terminal);
        for (size_t row = bottom + 1U - count; row <= bottom; row++) {
                for (size_t column = 0U; column < terminal->columns;
                     column++) {
                        cells[row][column] = blank;
                }
        }
        for (size_t row = top; row <= bottom; row++) {
                mark_dirty(terminal, row, 0U, terminal->columns);
        }
}

static void scroll_down(struct rose_terminal *terminal, size_t top,
                        size_t bottom, size_t count) {
        if (top > bottom || bottom >= terminal->rows) return;
        size_t height = bottom - top + 1U;
        if (count == 0U) count = 1U;
        if (count > height) count = height;
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        for (size_t row = bottom + 1U; row > top + count; row--) {
                size_t destination = row - 1U;
                size_t source = destination - count;
                for (size_t column = 0U; column < terminal->columns;
                     column++) {
                        copy_cell(&cells[destination][column],
                                  &cells[source][column]);
                }
        }
        struct rose_terminal_cell blank = blank_cell(terminal);
        for (size_t row = top; row < top + count; row++) {
                for (size_t column = 0U; column < terminal->columns;
                     column++) {
                        cells[row][column] = blank;
                }
        }
        for (size_t row = top; row <= bottom; row++) {
                mark_dirty(terminal, row, 0U, terminal->columns);
        }
}

static void line_feed(struct rose_terminal *terminal) {
        mark_cursor_dirty(terminal);
        if (terminal->cursor_row == terminal->scroll_bottom) {
                scroll_up(terminal, terminal->scroll_top,
                          terminal->scroll_bottom, 1U);
        } else if (terminal->cursor_row + 1U < terminal->rows) {
                terminal->cursor_row++;
        }
        terminal->wrap_pending = false;
        mark_cursor_dirty(terminal);
}

static void reverse_index(struct rose_terminal *terminal) {
        mark_cursor_dirty(terminal);
        if (terminal->cursor_row == terminal->scroll_top) {
                scroll_down(terminal, terminal->scroll_top,
                            terminal->scroll_bottom, 1U);
        } else if (terminal->cursor_row != 0U) {
                terminal->cursor_row--;
        }
        terminal->wrap_pending = false;
        mark_cursor_dirty(terminal);
}

static void put_character(struct rose_terminal *terminal, uint8_t character) {
        if (terminal->wrap_pending) {
                mark_cursor_dirty(terminal);
                terminal->cursor_column = 0U;
                line_feed(terminal);
        }
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        mark_cursor_dirty(terminal);
        cells[terminal->cursor_row][terminal->cursor_column] =
            (struct rose_terminal_cell){
                .character = character,
                .foreground = terminal->foreground,
                .background = terminal->background,
                .attributes = terminal->attributes,
            };
        mark_dirty(terminal, terminal->cursor_row, terminal->cursor_column,
                   (size_t)terminal->cursor_column + 1U);
        if (terminal->cursor_column + 1U == terminal->columns) {
                terminal->wrap_pending = true;
        } else {
                terminal->cursor_column++;
                mark_cursor_dirty(terminal);
        }
}

static size_t parameter(const struct rose_terminal *terminal, size_t index,
                        size_t default_value, bool zero_is_default) {
        if (index >= terminal->parameter_count ||
            (terminal->parameters_set & (1U << index)) == 0U) {
                return default_value;
        }
        size_t value = terminal->parameters[index];
        return zero_is_default && value == 0U ? default_value : value;
}

static void erase_display(struct rose_terminal *terminal, size_t mode) {
        if (mode == 0U) {
                clear_row_range(terminal, terminal->cursor_row,
                                terminal->cursor_column, terminal->columns);
                for (size_t row = terminal->cursor_row + 1U;
                     row < terminal->rows; row++) {
                        clear_row_range(terminal, row, 0U, terminal->columns);
                }
        } else if (mode == 1U) {
                for (size_t row = 0U; row < terminal->cursor_row; row++) {
                        clear_row_range(terminal, row, 0U, terminal->columns);
                }
                clear_row_range(terminal, terminal->cursor_row, 0U,
                                (size_t)terminal->cursor_column + 1U);
        } else if (mode == 2U || mode == 3U) {
                for (size_t row = 0U; row < terminal->rows; row++) {
                        clear_row_range(terminal, row, 0U, terminal->columns);
                }
                if (mode == 3U) {
                        terminal->scrollback_start = 0U;
                        terminal->scrollback_count = 0U;
                        terminal->view_offset = 0U;
                }
        }
}

static void erase_line(struct rose_terminal *terminal, size_t mode) {
        if (mode == 0U) {
                clear_row_range(terminal, terminal->cursor_row,
                                terminal->cursor_column, terminal->columns);
        } else if (mode == 1U) {
                clear_row_range(terminal, terminal->cursor_row, 0U,
                                (size_t)terminal->cursor_column + 1U);
        } else if (mode == 2U) {
                clear_row_range(terminal, terminal->cursor_row, 0U,
                                terminal->columns);
        }
}

static void select_graphic_rendition(struct rose_terminal *terminal) {
        for (size_t index = 0U; index < terminal->parameter_count; index++) {
                size_t value = parameter(terminal, index, 0U, false);
                if (value == 0U) {
                        terminal->foreground = 7U;
                        terminal->background = 0U;
                        terminal->attributes = 0U;
                } else if (value == 1U) {
                        terminal->attributes |= ROSE_TERMINAL_ATTRIBUTE_BOLD;
                } else if (value == 7U) {
                        terminal->attributes |= ROSE_TERMINAL_ATTRIBUTE_INVERSE;
                } else if (value == 22U) {
                        terminal->attributes &=
                            (uint8_t)~ROSE_TERMINAL_ATTRIBUTE_BOLD;
                } else if (value == 27U) {
                        terminal->attributes &=
                            (uint8_t)~ROSE_TERMINAL_ATTRIBUTE_INVERSE;
                } else if (value >= 30U && value <= 37U) {
                        terminal->foreground = (uint8_t)(value - 30U);
                } else if (value == 39U) {
                        terminal->foreground = 7U;
                } else if (value >= 40U && value <= 47U) {
                        terminal->background = (uint8_t)(value - 40U);
                } else if (value == 49U) {
                        terminal->background = 0U;
                } else if (value >= 90U && value <= 97U) {
                        terminal->foreground = (uint8_t)(value - 90U + 8U);
                } else if (value >= 100U && value <= 107U) {
                        terminal->background = (uint8_t)(value - 100U + 8U);
                }
        }
}

static void switch_alternate_screen(struct rose_terminal *terminal,
                                    bool enabled) {
        if (enabled == terminal->alternate) return;
        if (enabled) {
                save_cursor(terminal);
                terminal->alternate = true;
                terminal->cursor_row = 0U;
                terminal->cursor_column = 0U;
                terminal->foreground = 7U;
                terminal->background = 0U;
                terminal->attributes = 0U;
                for (size_t row = 0U; row < terminal->rows; row++) {
                        clear_row_range(terminal, row, 0U,
                                        terminal->columns);
                }
        } else {
                terminal->alternate = false;
                restore_cursor(terminal);
        }
        terminal->view_offset = 0U;
        terminal->scroll_top = 0U;
        terminal->scroll_bottom = terminal->rows - 1U;
        terminal->wrap_pending = false;
        mark_all_dirty(terminal);
}

static void set_private_mode(struct rose_terminal *terminal, bool enabled) {
        for (size_t index = 0U; index < terminal->parameter_count; index++) {
                size_t mode = parameter(terminal, index, 0U, false);
                if (mode == 1U) {
                        terminal->application_cursor = enabled;
                } else if (mode == 25U) {
                        mark_cursor_dirty(terminal);
                        terminal->cursor_visible = enabled;
                        mark_cursor_dirty(terminal);
                } else if (mode == 47U || mode == 1047U || mode == 1049U) {
                        switch_alternate_screen(terminal, enabled);
                }
        }
}

static void insert_characters(struct rose_terminal *terminal, size_t count) {
        if (count == 0U) count = 1U;
        size_t available = terminal->columns - terminal->cursor_column;
        if (count > available) count = available;
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        size_t row = terminal->cursor_row;
        for (size_t column = terminal->columns; column >
                                               terminal->cursor_column + count;
             column--) {
                cells[row][column - 1U] = cells[row][column - 1U - count];
        }
        struct rose_terminal_cell blank = blank_cell(terminal);
        for (size_t column = terminal->cursor_column;
             column < terminal->cursor_column + count; column++) {
                cells[row][column] = blank;
        }
        mark_dirty(terminal, row, terminal->cursor_column, terminal->columns);
}

static void delete_characters(struct rose_terminal *terminal, size_t count) {
        if (count == 0U) count = 1U;
        size_t available = terminal->columns - terminal->cursor_column;
        if (count > available) count = available;
        struct rose_terminal_cell (*cells)[ROSE_TERMINAL_MAX_COLUMNS] =
            active_cells(terminal);
        size_t row = terminal->cursor_row;
        for (size_t column = terminal->cursor_column;
             column + count < terminal->columns; column++) {
                cells[row][column] = cells[row][column + count];
        }
        struct rose_terminal_cell blank = blank_cell(terminal);
        for (size_t column = terminal->columns - count;
             column < terminal->columns; column++) {
                cells[row][column] = blank;
        }
        mark_dirty(terminal, row, terminal->cursor_column, terminal->columns);
}

static void execute_csi(struct rose_terminal *terminal, uint8_t final) {
        if (terminal->parser_discard) return;
        size_t first = parameter(terminal, 0U, 1U, true);
        size_t row = terminal->cursor_row;
        size_t column = terminal->cursor_column;
        switch (final) {
        case 'A':
                row = first > row ? 0U : row - first;
                set_cursor(terminal, row, column);
                break;
        case 'B':
        case 'e':
                set_cursor(terminal, row + first, column);
                break;
        case 'C':
        case 'a':
                set_cursor(terminal, row, column + first);
                break;
        case 'D':
                column = first > column ? 0U : column - first;
                set_cursor(terminal, row, column);
                break;
        case 'E':
                set_cursor(terminal, row + first, 0U);
                break;
        case 'F':
                row = first > row ? 0U : row - first;
                set_cursor(terminal, row, 0U);
                break;
        case 'G':
        case '`':
                set_cursor(terminal, row, first - 1U);
                break;
        case 'H':
        case 'f':
                set_cursor(terminal,
                           parameter(terminal, 0U, 1U, true) - 1U,
                           parameter(terminal, 1U, 1U, true) - 1U);
                break;
        case 'd':
                set_cursor(terminal, first - 1U, column);
                break;
        case 'J': erase_display(terminal, parameter(terminal, 0U, 0U, false)); break;
        case 'K': erase_line(terminal, parameter(terminal, 0U, 0U, false)); break;
        case 'm': select_graphic_rendition(terminal); break;
        case 's': save_cursor(terminal); break;
        case 'u': restore_cursor(terminal); break;
        case 'r': {
                size_t top = parameter(terminal, 0U, 1U, true);
                size_t bottom = parameter(terminal, 1U, terminal->rows, true);
                if (top < bottom && bottom <= terminal->rows) {
                        terminal->scroll_top = (uint16_t)(top - 1U);
                        terminal->scroll_bottom = (uint16_t)(bottom - 1U);
                        set_cursor(terminal, 0U, 0U);
                }
                break;
        }
        case 'S': scroll_up(terminal, terminal->scroll_top,
                            terminal->scroll_bottom, first); break;
        case 'T': scroll_down(terminal, terminal->scroll_top,
                              terminal->scroll_bottom, first); break;
        case 'L': scroll_down(terminal, terminal->cursor_row,
                              terminal->scroll_bottom, first); break;
        case 'M': scroll_up(terminal, terminal->cursor_row,
                            terminal->scroll_bottom, first); break;
        case '@': insert_characters(terminal, first); break;
        case 'P': delete_characters(terminal, first); break;
        case 'X': clear_row_range(terminal, terminal->cursor_row,
                                  terminal->cursor_column,
                                  terminal->cursor_column + first); break;
        case 'h': if (terminal->parser_private) set_private_mode(terminal, true); break;
        case 'l': if (terminal->parser_private) set_private_mode(terminal, false); break;
        default: break;
        }
}

static void begin_csi(struct rose_terminal *terminal) {
        terminal->parser_state = TERMINAL_PARSER_CSI;
        terminal->parameter_count = 1U;
        terminal->parameters_set = 0U;
        terminal->parameters[0] = 0U;
        terminal->parser_length = 0U;
        terminal->parser_private = false;
        terminal->parser_discard = false;
}

static void reset_parser(struct rose_terminal *terminal) {
        terminal->parser_state = TERMINAL_PARSER_GROUND;
        terminal->parser_length = 0U;
        terminal->parser_private = false;
        terminal->parser_discard = false;
}

static void feed_csi(struct rose_terminal *terminal, uint8_t character) {
        terminal->parser_length++;
        if (terminal->parser_length > TERMINAL_SEQUENCE_LIMIT)
                terminal->parser_discard = true;
        if (character >= '0' && character <= '9') {
                size_t index = terminal->parameter_count - 1U;
                if (terminal->parser_discard) return;
                uint16_t value = terminal->parameters[index];
                uint16_t digit = (uint16_t)(character - '0');
                terminal->parameters[index] =
                    value > 999U ? 9999U : (uint16_t)(value * 10U + digit);
                terminal->parameters_set |= (uint16_t)(1U << index);
        } else if (character == ';' || character == ':') {
                if (terminal->parameter_count <
                    ROSE_TERMINAL_CSI_PARAMETERS) {
                        terminal->parameters[terminal->parameter_count] = 0U;
                        terminal->parameter_count++;
                } else {
                        terminal->parser_discard = true;
                }
        } else if (character == '?' && terminal->parser_length == 1U) {
                terminal->parser_private = true;
        } else if (character >= 0x40U && character <= 0x7eU) {
                execute_csi(terminal, character);
                reset_parser(terminal);
        } else if (character < 0x20U) {
                return;
        }
}

static void reset_terminal(struct rose_terminal *terminal) {
        terminal->foreground = 7U;
        terminal->background = 0U;
        terminal->attributes = 0U;
        terminal->cursor_visible = true;
        terminal->application_cursor = false;
        terminal->scroll_top = 0U;
        terminal->scroll_bottom = terminal->rows - 1U;
        terminal->cursor_row = 0U;
        terminal->cursor_column = 0U;
        terminal->wrap_pending = false;
        for (size_t row = 0U; row < terminal->rows; row++) {
                clear_row_range(terminal, row, 0U, terminal->columns);
        }
}

static void feed_byte(struct rose_terminal *terminal, uint8_t character) {
        switch (terminal->parser_state) {
        case TERMINAL_PARSER_GROUND:
                if (character == 0x1bU) {
                        terminal->parser_state = TERMINAL_PARSER_ESCAPE;
                } else if (character == '\r') {
                        set_cursor(terminal, terminal->cursor_row, 0U);
                } else if (character == '\n' || character == '\v' ||
                           character == '\f') {
                        line_feed(terminal);
                } else if (character == '\b') {
                        if (terminal->cursor_column != 0U)
                                set_cursor(terminal, terminal->cursor_row,
                                           terminal->cursor_column - 1U);
                } else if (character == '\t') {
                        size_t next =
                            ((size_t)terminal->cursor_column + 8U) & ~7U;
                        set_cursor(terminal, terminal->cursor_row, next);
                } else if (character >= ' ' && character <= '~') {
                        put_character(terminal, character);
                }
                break;
        case TERMINAL_PARSER_ESCAPE:
                if (character == '[') {
                        begin_csi(terminal);
                } else if (character == ']') {
                        terminal->parser_state = TERMINAL_PARSER_OSC;
                } else if (character == 'P' || character == '^' ||
                           character == '_') {
                        terminal->parser_state = TERMINAL_PARSER_STRING;
                } else {
                        if (character == '7') save_cursor(terminal);
                        if (character == '8') restore_cursor(terminal);
                        if (character == 'D') line_feed(terminal);
                        if (character == 'E') {
                                set_cursor(terminal, terminal->cursor_row, 0U);
                                line_feed(terminal);
                        }
                        if (character == 'M') reverse_index(terminal);
                        if (character == 'c') reset_terminal(terminal);
                        reset_parser(terminal);
                }
                break;
        case TERMINAL_PARSER_CSI:
                feed_csi(terminal, character);
                break;
        case TERMINAL_PARSER_OSC:
                if (character == 0x07U) reset_parser(terminal);
                else if (character == 0x1bU)
                        terminal->parser_state = TERMINAL_PARSER_OSC_ESCAPE;
                break;
        case TERMINAL_PARSER_OSC_ESCAPE:
                if (character == '\\') reset_parser(terminal);
                else terminal->parser_state = TERMINAL_PARSER_OSC;
                break;
        case TERMINAL_PARSER_STRING:
                if (character == 0x1bU)
                        terminal->parser_state = TERMINAL_PARSER_STRING_ESCAPE;
                break;
        case TERMINAL_PARSER_STRING_ESCAPE:
                if (character == '\\') reset_parser(terminal);
                else terminal->parser_state = TERMINAL_PARSER_STRING;
                break;
        default: reset_parser(terminal); break;
        }
}

void rose_terminal_initialize(struct rose_terminal *terminal, size_t columns,
                              size_t rows) {
        if (columns == 0U) columns = 1U;
        if (rows == 0U) rows = 1U;
        if (columns > ROSE_TERMINAL_MAX_COLUMNS)
                columns = ROSE_TERMINAL_MAX_COLUMNS;
        if (rows > ROSE_TERMINAL_MAX_ROWS) rows = ROSE_TERMINAL_MAX_ROWS;

        terminal->columns = (uint16_t)columns;
        terminal->rows = (uint16_t)rows;
        terminal->foreground = 7U;
        terminal->background = 0U;
        terminal->attributes = 0U;
        terminal->saved_foreground = 7U;
        terminal->saved_background = 0U;
        terminal->saved_attributes = 0U;
        terminal->cursor_row = 0U;
        terminal->cursor_column = 0U;
        terminal->saved_row = 0U;
        terminal->saved_column = 0U;
        terminal->scroll_top = 0U;
        terminal->scroll_bottom = (uint16_t)(rows - 1U);
        terminal->scrollback_start = 0U;
        terminal->scrollback_count = 0U;
        terminal->scrollback_capacity = 0U;
        terminal->scrollback = NULL;
        terminal->view_offset = 0U;
        terminal->cursor_visible = true;
        terminal->alternate = false;
        terminal->application_cursor = false;
        terminal->wrap_pending = false;
        reset_parser(terminal);

        struct rose_terminal_cell blank = blank_cell(terminal);
        for (size_t row = 0U; row < ROSE_TERMINAL_MAX_ROWS; row++) {
                terminal->dirty_left[row] = ROSE_TERMINAL_MAX_COLUMNS;
                terminal->dirty_right[row] = 0U;
                for (size_t column = 0U;
                     column < ROSE_TERMINAL_MAX_COLUMNS; column++) {
                        terminal->cells[row][column] = blank;
                        terminal->alternate_cells[row][column] = blank;
                }
        }
        mark_all_dirty(terminal);
}

void rose_terminal_set_scrollback(struct rose_terminal *terminal,
                                  struct rose_terminal_line *lines,
                                  size_t capacity) {
        if (capacity > ROSE_TERMINAL_SCROLLBACK_LINES)
                capacity = ROSE_TERMINAL_SCROLLBACK_LINES;
        terminal->scrollback = lines;
        terminal->scrollback_capacity =
            lines == NULL ? 0U : (uint16_t)capacity;
        terminal->scrollback_start = 0U;
        terminal->scrollback_count = 0U;
        terminal->view_offset = 0U;
}

void rose_terminal_resize(struct rose_terminal *terminal, size_t columns,
                          size_t rows) {
        if (columns == 0U) columns = 1U;
        if (rows == 0U) rows = 1U;
        if (columns > ROSE_TERMINAL_MAX_COLUMNS)
                columns = ROSE_TERMINAL_MAX_COLUMNS;
        if (rows > ROSE_TERMINAL_MAX_ROWS) rows = ROSE_TERMINAL_MAX_ROWS;
        size_t old_columns = terminal->columns;
        size_t old_rows = terminal->rows;
        if (old_columns > ROSE_TERMINAL_MAX_COLUMNS)
                old_columns = ROSE_TERMINAL_MAX_COLUMNS;
        if (old_rows > ROSE_TERMINAL_MAX_ROWS)
                old_rows = ROSE_TERMINAL_MAX_ROWS;
        if (columns == old_columns && rows == old_rows) return;

        if (rows < old_rows && terminal->cursor_row >= rows) {
                size_t remove = terminal->cursor_row - rows + 1U;
                scroll_up(terminal, 0U, old_rows - 1U, remove);
                terminal->cursor_row = (uint16_t)(terminal->cursor_row - remove);
        }
        struct rose_terminal_cell blank = blank_cell(terminal);
        if (columns != old_columns) {
                size_t left = columns < old_columns ? columns : old_columns;
                size_t right = columns > old_columns ? columns : old_columns;
                for (size_t row = 0U; row < ROSE_TERMINAL_MAX_ROWS; row++) {
                        for (size_t column = left; column < right; column++) {
                                terminal->cells[row][column] = blank;
                                terminal->alternate_cells[row][column] = blank;
                        }
                }
        }
        if (rows != old_rows) {
                size_t top = rows < old_rows ? rows : old_rows;
                size_t bottom = rows > old_rows ? rows : old_rows;
                for (size_t row = top; row < bottom; row++) {
                        for (size_t column = 0U; column < columns; column++) {
                                terminal->cells[row][column] = blank;
                                terminal->alternate_cells[row][column] = blank;
                        }
                }
        }
        terminal->columns = (uint16_t)columns;
        terminal->rows = (uint16_t)rows;
        if (terminal->cursor_column >= columns)
                terminal->cursor_column = (uint16_t)(columns - 1U);
        if (terminal->cursor_row >= rows)
                terminal->cursor_row = (uint16_t)(rows - 1U);
        if (terminal->saved_column >= columns)
                terminal->saved_column = (uint16_t)(columns - 1U);
        if (terminal->saved_row >= rows)
                terminal->saved_row = (uint16_t)(rows - 1U);
        terminal->scroll_top = 0U;
        terminal->scroll_bottom = (uint16_t)(rows - 1U);
        terminal->wrap_pending = false;
        mark_all_dirty(terminal);
}

void rose_terminal_feed(struct rose_terminal *terminal, const char *bytes,
                        size_t count) {
        for (size_t index = 0U; index < count; index++) {
                feed_byte(terminal, (uint8_t)bytes[index]);
        }
}

void rose_terminal_scroll_page(struct rose_terminal *terminal, bool up) {
        size_t page = terminal->rows > 1U ? terminal->rows - 1U : 1U;
        if (up) {
                size_t offset = terminal->view_offset + page;
                if (offset > terminal->scrollback_count)
                        offset = terminal->scrollback_count;
                terminal->view_offset = (uint16_t)offset;
        } else if (page >= terminal->view_offset) {
                terminal->view_offset = 0U;
        } else {
                terminal->view_offset =
                    (uint16_t)(terminal->view_offset - page);
        }
        mark_all_dirty(terminal);
}

void rose_terminal_jump_to_latest(struct rose_terminal *terminal) {
        if (terminal->view_offset == 0U) return;
        terminal->view_offset = 0U;
        mark_all_dirty(terminal);
}

void rose_terminal_mark_cursor_dirty(struct rose_terminal *terminal) {
        mark_cursor_dirty(terminal);
}

bool rose_terminal_take_dirty(struct rose_terminal *terminal, size_t row,
                              size_t *left, size_t *right) {
        if (row >= terminal->rows ||
            terminal->dirty_left[row] >= terminal->dirty_right[row]) {
                return false;
        }
        *left = terminal->dirty_left[row];
        *right = terminal->dirty_right[row];
        terminal->dirty_left[row] = ROSE_TERMINAL_MAX_COLUMNS;
        terminal->dirty_right[row] = 0U;
        return true;
}

const struct rose_terminal_cell *
rose_terminal_visible_cell(const struct rose_terminal *terminal, size_t row,
                           size_t column) {
        static const struct rose_terminal_cell blank = {' ', 7U, 0U, 0U};
        if (row >= terminal->rows || column >= terminal->columns) return &blank;
        size_t index = (size_t)terminal->scrollback_count -
                       terminal->view_offset + row;
        if (!terminal->alternate && index < terminal->scrollback_count) {
                size_t slot = (terminal->scrollback_start + index) %
                              terminal->scrollback_capacity;
                if (column >= terminal->scrollback[slot].width) return &blank;
                return &terminal->scrollback[slot].cells[column];
        }
        size_t screen_row = terminal->alternate
                                ? row
                                : index - terminal->scrollback_count;
        if (screen_row >= terminal->rows) return &blank;
        const struct rose_terminal_cell (*cells)
            [ROSE_TERMINAL_MAX_COLUMNS] = active_cells_const(terminal);
        return &cells[screen_row][column];
}

bool rose_terminal_visible_cursor(const struct rose_terminal *terminal,
                                  size_t *row, size_t *column) {
        if (!terminal->cursor_visible || terminal->view_offset != 0U) {
                return false;
        }
        *row = terminal->cursor_row;
        *column = terminal->cursor_column;
        return true;
}

size_t rose_terminal_columns(const struct rose_terminal *terminal) {
        return terminal->columns;
}

size_t rose_terminal_rows(const struct rose_terminal *terminal) {
        return terminal->rows;
}

size_t rose_terminal_scrollback_count(const struct rose_terminal *terminal) {
        return terminal->scrollback_count;
}

size_t rose_terminal_view_offset(const struct rose_terminal *terminal) {
        return terminal->view_offset;
}

bool rose_terminal_application_cursor(const struct rose_terminal *terminal) {
        return terminal->application_cursor;
}
