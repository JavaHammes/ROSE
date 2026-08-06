#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum { RESOURCE_BUFFER_SIZE = 4096 };
enum { RESOURCE_READ_SIZE = 512 };
static char resource_buffer[RESOURCE_BUFFER_SIZE];

static size_t text_length(const char *text) {
        size_t length = 0U;
        while (text != NULL && text[length] != '\0') length++;
        return length;
}

static bool text_equal_range(const char *text, const char *start, size_t size) {
        size_t length = text_length(text);
        if (length != size) return false;
        for (size_t index = 0U; index < size; index++) {
                if (text[index] != start[index]) return false;
        }
        return true;
}

static size_t copy_range(char *destination, size_t capacity,
                         const char *start, size_t size) {
        if (capacity == 0U) return 0U;
        if (size >= capacity) size = capacity - 1U;
        for (size_t index = 0U; index < size; index++) {
                destination[index] = start[index];
        }
        destination[size] = '\0';
        return size;
}

static long read_resource(const char *path) {
        long descriptor = rose_open(path, USER_OPEN_READ);
        if (descriptor < 0) return descriptor;
        size_t used = 0U;
        while (used + 1U < RESOURCE_BUFFER_SIZE) {
                size_t available = RESOURCE_BUFFER_SIZE - used - 1U;
                if (available > RESOURCE_READ_SIZE)
                        available = RESOURCE_READ_SIZE;
                long count = rose_read((int)descriptor, resource_buffer + used,
                                       available);
                if (count < 0) {
                        (void)rose_close((int)descriptor);
                        return count;
                }
                if (count == 0) break;
                used += (size_t)count;
        }
        (void)rose_close((int)descriptor);
        resource_buffer[used] = '\0';
        return (long)used;
}

static int hex_digit(char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
        return -1;
}

static bool parse_color(const char *start, const char *end, uint32_t *value) {
        if (start < end && *start == '#') start++;
        if ((size_t)(end - start) != 6U) return false;
        uint32_t color = 0U;
        for (size_t index = 0U; index < 6U; index++) {
                int digit = hex_digit(start[index]);
                if (digit < 0) return false;
                color = (color << 4U) | (uint32_t)digit;
        }
        *value = color;
        return true;
}

static bool parse_unsigned(const char *start, const char *end,
                           uint32_t *value) {
        if (start == end) return false;
        uint32_t result = 0U;
        while (start < end) {
                if (*start < '0' || *start > '9') return false;
                uint32_t digit = (uint32_t)(*start++ - '0');
                if (result > (UINT32_MAX - digit) / 10U) return false;
                result = result * 10U + digit;
        }
        *value = result;
        return true;
}

void rose_gui_theme_defaults(struct rose_gui_theme *theme) {
        if (theme == NULL) return;
        theme->background = UINT32_C(0x00131c2b);
        theme->surface = UINT32_C(0x001d2a3f);
        theme->surface_alternate = UINT32_C(0x0026344b);
        theme->text = UINT32_C(0x00edf4ff);
        theme->muted = UINT32_C(0x0095a7bf);
        theme->accent = UINT32_C(0x005f7cff);
        theme->accent_hover = UINT32_C(0x00768cff);
        theme->accent_pressed = UINT32_C(0x004b63d8);
        theme->on_accent = UINT32_C(0x00ffffff);
        theme->border = UINT32_C(0x00425874);
        theme->focus = UINT32_C(0x004de2b4);
        theme->error = UINT32_C(0x00ef6575);
        theme->selected = UINT32_C(0x0034476b);
        theme->disabled = UINT32_C(0x005e6d82);
        theme->shadow = UINT32_C(0x000b111c);
        theme->success = UINT32_C(0x0048d5a2);
        theme->warning = UINT32_C(0x00f2bb4b);
        theme->padding = 12U;
        theme->gap = 8U;
        theme->control_height = 28U;
        theme->corner_radius = 4U;
}

static void assign_theme_color(struct rose_gui_theme *theme, const char *key,
                               size_t key_size, uint32_t value) {
#define ASSIGN_COLOR(name)                                                     \
        if (text_equal_range(#name, key, key_size)) {                          \
                theme->name = value;                                            \
                return;                                                         \
        }
        ASSIGN_COLOR(background)
        ASSIGN_COLOR(surface)
        ASSIGN_COLOR(surface_alternate)
        ASSIGN_COLOR(text)
        ASSIGN_COLOR(muted)
        ASSIGN_COLOR(accent)
        ASSIGN_COLOR(accent_hover)
        ASSIGN_COLOR(accent_pressed)
        ASSIGN_COLOR(on_accent)
        ASSIGN_COLOR(border)
        ASSIGN_COLOR(focus)
        ASSIGN_COLOR(error)
        ASSIGN_COLOR(selected)
        ASSIGN_COLOR(disabled)
        ASSIGN_COLOR(shadow)
        ASSIGN_COLOR(success)
        ASSIGN_COLOR(warning)
#undef ASSIGN_COLOR
}

static void assign_theme_metric(struct rose_gui_theme *theme, const char *key,
                                size_t key_size, uint32_t value) {
        if (value > UINT16_MAX) return;
#define ASSIGN_METRIC(name)                                                    \
        if (text_equal_range(#name, key, key_size)) {                          \
                theme->name = (uint16_t)value;                                  \
                return;                                                         \
        }
        ASSIGN_METRIC(padding)
        ASSIGN_METRIC(gap)
        ASSIGN_METRIC(control_height)
        ASSIGN_METRIC(corner_radius)
#undef ASSIGN_METRIC
}

bool rose_gui_theme_load(struct rose_gui_theme *theme, const char *path) {
        if (theme == NULL || path == NULL) return false;
        rose_gui_theme_defaults(theme);
        long count = read_resource(path);
        if (count < 0) return false;
        const char *cursor = resource_buffer;
        const char *limit = resource_buffer + count;
        while (cursor < limit) {
                const char *line = cursor;
                while (cursor < limit && *cursor != '\n') cursor++;
                const char *end = cursor;
                if (cursor < limit) cursor++;
                if (line == end || *line == '#') continue;
                const char *equals = line;
                while (equals < end && *equals != '=') equals++;
                if (equals == end) continue;
                const char *value_start = equals + 1;
                uint32_t value;
                if (parse_color(value_start, end, &value)) {
                        assign_theme_color(theme, line,
                                           (size_t)(equals - line), value);
                } else if (parse_unsigned(value_start, end, &value)) {
                        assign_theme_metric(theme, line,
                                            (size_t)(equals - line), value);
                }
        }
        return true;
}

bool rose_gui_icon_load(struct rose_gui_icon *icon, const char *name,
                        const char *path) {
        if (icon == NULL || name == NULL || path == NULL) return false;
        long count = read_resource(path);
        if (count < 0) return false;
        const char *cursor = resource_buffer;
        const char *limit = resource_buffer + count;
        while (cursor < limit) {
                const char *line = cursor;
                while (cursor < limit && *cursor != '\n') cursor++;
                const char *end = cursor;
                if (cursor < limit) cursor++;
                const char *equals = line;
                while (equals < end && *equals != '=') equals++;
                if (equals == end ||
                    !text_equal_range(name, line, (size_t)(equals - line)) ||
                    (size_t)(end - equals - 1) !=
                        (size_t)ROSE_GUI_ICON_SIZE * 2U) {
                        continue;
                }
                for (size_t row = 0U; row < ROSE_GUI_ICON_SIZE; row++) {
                        int high = hex_digit(equals[1 + row * 2U]);
                        int low = hex_digit(equals[2 + row * 2U]);
                        if (high < 0 || low < 0) return false;
                        icon->rows[row] = (uint8_t)((high << 4) | low);
                }
                return true;
        }
        return false;
}

static const char *field_end(const char *cursor, const char *line_end) {
        while (cursor < line_end && *cursor != '|') cursor++;
        return cursor;
}

bool rose_gui_app_catalog_load(struct rose_gui_app_catalog *catalog,
                               const char *path) {
        if (catalog == NULL || path == NULL) return false;
        catalog->count = 0U;
        long count = read_resource(path);
        if (count < 0) return false;
        const char *cursor = resource_buffer;
        const char *limit = resource_buffer + count;
        while (cursor < limit && catalog->count < ROSE_GUI_APP_LIMIT) {
                const char *line = cursor;
                while (cursor < limit && *cursor != '\n') cursor++;
                const char *end = cursor;
                if (cursor < limit) cursor++;
                if (line == end || *line == '#') continue;

                struct rose_gui_app_metadata *app =
                    &catalog->apps[catalog->count];
                const char *field = line;
                const char *next = field_end(field, end);
                if (next == end) continue;
                copy_range(app->name, sizeof(app->name), field,
                           (size_t)(next - field));
                field = next + 1;
                next = field_end(field, end);
                if (next == end) continue;
                copy_range(app->title, sizeof(app->title), field,
                           (size_t)(next - field));
                field = next + 1;
                next = field_end(field, end);
                if (next == end) continue;
                copy_range(app->program, sizeof(app->program), field,
                           (size_t)(next - field));
                field = next + 1;
                next = field_end(field, end);
                if (next == end) continue;
                copy_range(app->icon, sizeof(app->icon), field,
                           (size_t)(next - field));
                field = next + 1;
                next = field_end(field, end);
                uint32_t width;
                if (next == end || !parse_unsigned(field, next, &width))
                        continue;
                field = next + 1;
                uint32_t height;
                if (!parse_unsigned(field, end, &height) || width == 0U ||
                    height == 0U) {
                        continue;
                }
                app->width = width;
                app->height = height;
                catalog->count++;
        }
        return catalog->count != 0U;
}

const struct rose_gui_app_metadata *rose_gui_app_find(
    const struct rose_gui_app_catalog *catalog, const char *name) {
        if (catalog == NULL || name == NULL) return NULL;
        for (size_t index = 0U; index < catalog->count; index++) {
                if (text_equal_range(name, catalog->apps[index].name,
                                     text_length(catalog->apps[index].name))) {
                        return &catalog->apps[index];
                }
        }
        return NULL;
}
