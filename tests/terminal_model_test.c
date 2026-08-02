#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/terminal.h"

static struct rose_terminal terminal;
static struct rose_terminal_line
    scrollback[ROSE_TERMINAL_SCROLLBACK_LINES];

static const struct rose_terminal_cell *cell(size_t row, size_t column) {
        return rose_terminal_visible_cell(&terminal, row, column);
}

int main(void) {
        rose_terminal_initialize(&terminal, 10U, 4U);
        rose_terminal_set_scrollback(&terminal, scrollback,
                                     ROSE_TERMINAL_SCROLLBACK_LINES);
        rose_terminal_feed(&terminal, "abc", 3U);
        assert(cell(0U, 0U)->character == 'a');
        assert(cell(0U, 2U)->character == 'c');

        static const char addressed[] = "\033[2;3HX";
        rose_terminal_feed(&terminal, addressed, sizeof(addressed) - 1U);
        assert(cell(1U, 2U)->character == 'X');

        static const char colored[] = "\033[1;7;91;104mZ";
        rose_terminal_feed(&terminal, colored, sizeof(colored) - 1U);
        const struct rose_terminal_cell *colored_cell = cell(1U, 3U);
        assert(colored_cell->character == 'Z');
        assert(colored_cell->foreground == 9U);
        assert(colored_cell->background == 12U);
        assert((colored_cell->attributes & ROSE_TERMINAL_ATTRIBUTE_BOLD) != 0U);
        assert((colored_cell->attributes & ROSE_TERMINAL_ATTRIBUTE_INVERSE) !=
               0U);

        static const char saved[] = "\033[s\033[4;10HQ\033[uR";
        rose_terminal_feed(&terminal, saved, sizeof(saved) - 1U);
        assert(cell(3U, 9U)->character == 'Q');
        assert(cell(1U, 4U)->character == 'R');
        static const char erased[] = "\033[2K";
        rose_terminal_feed(&terminal, erased, sizeof(erased) - 1U);
        for (size_t column = 0U; column < rose_terminal_columns(&terminal);
             column++) {
                assert(cell(1U, column)->character == ' ');
        }

        static const char alternate[] = "\033[?1049hALT\033[?1049l";
        rose_terminal_feed(&terminal, alternate, sizeof(alternate) - 1U);
        assert(cell(0U, 0U)->character == 'a');
        static const char clear_screen[] = "\033[2J\033[H";
        rose_terminal_feed(&terminal, clear_screen,
                           sizeof(clear_screen) - 1U);
        for (size_t row = 0U; row < rose_terminal_rows(&terminal); row++) {
                for (size_t column = 0U;
                     column < rose_terminal_columns(&terminal); column++) {
                        assert(cell(row, column)->character == ' ');
                }
        }

        rose_terminal_initialize(&terminal, 5U, 3U);
        rose_terminal_set_scrollback(&terminal, scrollback,
                                     ROSE_TERMINAL_SCROLLBACK_LINES);
        for (size_t line = 0U; line < 1100U; line++) {
                rose_terminal_feed(&terminal, "x\r\n", 3U);
        }
        assert(rose_terminal_scrollback_count(&terminal) ==
               ROSE_TERMINAL_SCROLLBACK_LINES);
        rose_terminal_scroll_page(&terminal, true);
        size_t old_offset = rose_terminal_view_offset(&terminal);
        assert(old_offset != 0U);
        rose_terminal_feed(&terminal, "live\r\n", 6U);
        assert(rose_terminal_view_offset(&terminal) >= old_offset);
        rose_terminal_jump_to_latest(&terminal);
        assert(rose_terminal_view_offset(&terminal) == 0U);

        rose_terminal_resize(&terminal, 3U, 2U);
        assert(rose_terminal_columns(&terminal) == 3U);
        assert(rose_terminal_rows(&terminal) == 2U);

        rose_terminal_initialize(&terminal, 8U, 2U);
        rose_terminal_feed(&terminal, "\033[", 2U);
        for (size_t index = 0U; index < 256U; index++) {
                rose_terminal_feed(&terminal, "1", 1U);
        }
        rose_terminal_feed(&terminal, "m!", 2U);
        assert(cell(0U, 0U)->character == '!');
        return 0;
}
