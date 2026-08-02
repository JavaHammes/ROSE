#ifndef ROSE_USER_TERMINAL_H
#define ROSE_USER_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
        ROSE_TERMINAL_MAX_COLUMNS = 170,
        ROSE_TERMINAL_MAX_ROWS = 80,
        ROSE_TERMINAL_SCROLLBACK_LINES = 1024,
        ROSE_TERMINAL_CSI_PARAMETERS = 16,
        ROSE_TERMINAL_ATTRIBUTE_BOLD = (1U << 0),
        ROSE_TERMINAL_ATTRIBUTE_INVERSE = (1U << 1),
};

struct rose_terminal_cell {
        uint8_t character;
        uint8_t foreground;
        uint8_t background;
        uint8_t attributes;
};

struct rose_terminal_line {
        struct rose_terminal_cell cells[ROSE_TERMINAL_MAX_COLUMNS];
        uint16_t width;
};

struct rose_terminal {
        struct rose_terminal_cell cells[ROSE_TERMINAL_MAX_ROWS]
                                        [ROSE_TERMINAL_MAX_COLUMNS];
        struct rose_terminal_cell alternate_cells[ROSE_TERMINAL_MAX_ROWS]
                                                  [ROSE_TERMINAL_MAX_COLUMNS];
        struct rose_terminal_line *scrollback;
        uint16_t dirty_left[ROSE_TERMINAL_MAX_ROWS];
        uint16_t dirty_right[ROSE_TERMINAL_MAX_ROWS];
        uint16_t columns;
        uint16_t rows;
        uint16_t cursor_row;
        uint16_t cursor_column;
        uint16_t saved_row;
        uint16_t saved_column;
        uint16_t scroll_top;
        uint16_t scroll_bottom;
        uint16_t scrollback_start;
        uint16_t scrollback_count;
        uint16_t scrollback_capacity;
        uint16_t view_offset;
        uint16_t parameters[ROSE_TERMINAL_CSI_PARAMETERS];
        uint8_t foreground;
        uint8_t background;
        uint8_t attributes;
        uint8_t saved_foreground;
        uint8_t saved_background;
        uint8_t saved_attributes;
        uint8_t parser_state;
        uint8_t parameter_count;
        uint16_t parameters_set;
        uint16_t parser_length;
        bool parser_private;
        bool parser_discard;
        bool wrap_pending;
        bool cursor_visible;
        bool alternate;
        bool application_cursor;
};

void rose_terminal_initialize(struct rose_terminal *terminal, size_t columns,
                              size_t rows);
void rose_terminal_set_scrollback(struct rose_terminal *terminal,
                                  struct rose_terminal_line *lines,
                                  size_t capacity);
void rose_terminal_resize(struct rose_terminal *terminal, size_t columns,
                          size_t rows);
void rose_terminal_feed(struct rose_terminal *terminal, const char *bytes,
                        size_t count);
void rose_terminal_scroll_page(struct rose_terminal *terminal, bool up);
void rose_terminal_jump_to_latest(struct rose_terminal *terminal);
void rose_terminal_mark_cursor_dirty(struct rose_terminal *terminal);
bool rose_terminal_take_dirty(struct rose_terminal *terminal, size_t row,
                              size_t *left, size_t *right);
const struct rose_terminal_cell *
rose_terminal_visible_cell(const struct rose_terminal *terminal, size_t row,
                           size_t column);
bool rose_terminal_visible_cursor(const struct rose_terminal *terminal,
                                  size_t *row, size_t *column);
size_t rose_terminal_columns(const struct rose_terminal *terminal);
size_t rose_terminal_rows(const struct rose_terminal *terminal);
size_t rose_terminal_scrollback_count(const struct rose_terminal *terminal);
size_t rose_terminal_view_offset(const struct rose_terminal *terminal);
bool rose_terminal_application_cursor(const struct rose_terminal *terminal);

#endif
