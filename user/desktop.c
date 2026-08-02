/* Userspace window server for shared-memory client surfaces. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        FONT_WIDTH = 5,
        FONT_HEIGHT = 7,
        WINDOW_LIMIT = 3,
        PANEL_HEIGHT = 54,
        WINDOW_BORDER = 2,
        WINDOW_TITLE_HEIGHT = 30,
        WINDOW_SHADOW_X = 9,
        WINDOW_SHADOW_Y = 11,
        POINTER_SIZE = 22,
        KEY_ESCAPE = 1,
};

#define COLOR_PANEL UINT32_C(0x0010192a)
#define COLOR_TITLE UINT32_C(0x001c2940)
#define COLOR_TITLE_FOCUSED UINT32_C(0x00253652)
#define COLOR_BORDER UINT32_C(0x00425874)
#define COLOR_TEXT UINT32_C(0x00dce8f7)
#define COLOR_MUTED UINT32_C(0x008da0bb)
#define COLOR_ACCENT UINT32_C(0x005f7cff)
#define COLOR_GREEN UINT32_C(0x0048d5a2)
#define COLOR_YELLOW UINT32_C(0x00f2bb4b)
#define COLOR_RED UINT32_C(0x00ef6575)
#define COLOR_SHADOW UINT32_C(0x00101828)
#define COLOR_WHITE UINT32_C(0x00ffffff)

static const uint8_t font[59][FONT_WIDTH] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7f, 0x14, 0x7f, 0x14},
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1c, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1c, 0x00},
    {0x14, 0x08, 0x3e, 0x08, 0x14}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3e}, {0x7e, 0x11, 0x11, 0x11, 0x7e},
    {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a},
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43},
};

struct rectangle {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
};

struct desktop_window {
        const char *title;
        const char *program;
        int32_t x;
        int32_t y;
        uint32_t width;
        uint32_t height;
        long pid;
        struct user_shared_memory_info mapping;
        struct rose_gui_surface *surface;
        uint32_t *pixels;
        uint32_t last_damage_sequence;
};

struct desktop {
        struct user_graphics_info graphics;
        uint32_t *pixels;
        uint32_t pixel_stride;
        struct desktop_window windows[WINDOW_LIMIT];
        size_t window_count;
        int32_t pointer_x;
        int32_t pointer_y;
        uint32_t pointer_buttons;
        bool dragging;
        int32_t drag_offset_x;
        int32_t drag_offset_y;
        struct rectangle dirty;
        bool has_dirty;
        bool exiting;
};

static size_t string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') length++;
        return length;
}

static bool strings_equal(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }
        return *left == *right;
}

static void print(const char *text) {
        (void)rose_write(USER_STDOUT_FILENO, text, string_length(text));
}

static void zero_bytes(void *destination, size_t size) {
        uint8_t *bytes = destination;
        for (size_t index = 0U; index < size; index++) bytes[index] = 0U;
}

static struct rectangle window_rectangle(const struct desktop_window *window) {
        return (struct rectangle){
            .x = window->x,
            .y = window->y,
            .width = (int32_t)window->width + WINDOW_BORDER * 2,
            .height = (int32_t)window->height + WINDOW_TITLE_HEIGHT +
                      WINDOW_BORDER,
        };
}

/* A window's visual footprint includes the drop shadow. Movement and removal
 * must invalidate this union, while hit-testing still uses the window itself. */
static struct rectangle
window_damage_rectangle(const struct desktop_window *window) {
        struct rectangle rectangle = window_rectangle(window);
        rectangle.width += WINDOW_SHADOW_X;
        rectangle.height += WINDOW_SHADOW_Y;
        return rectangle;
}

static void dirty_add(struct desktop *desktop, struct rectangle rectangle) {
        if (rectangle.x < 0) {
                rectangle.width += rectangle.x;
                rectangle.x = 0;
        }
        if (rectangle.y < 0) {
                rectangle.height += rectangle.y;
                rectangle.y = 0;
        }
        if (rectangle.x + rectangle.width > (int32_t)desktop->graphics.width) {
                rectangle.width =
                    (int32_t)desktop->graphics.width - rectangle.x;
        }
        if (rectangle.y + rectangle.height >
            (int32_t)desktop->graphics.height) {
                rectangle.height =
                    (int32_t)desktop->graphics.height - rectangle.y;
        }
        if (rectangle.width <= 0 || rectangle.height <= 0) return;
        if (!desktop->has_dirty) {
                desktop->dirty = rectangle;
                desktop->has_dirty = true;
                return;
        }
        int32_t right = desktop->dirty.x + desktop->dirty.width;
        int32_t bottom = desktop->dirty.y + desktop->dirty.height;
        int32_t new_right = rectangle.x + rectangle.width;
        int32_t new_bottom = rectangle.y + rectangle.height;
        if (rectangle.x < desktop->dirty.x) desktop->dirty.x = rectangle.x;
        if (rectangle.y < desktop->dirty.y) desktop->dirty.y = rectangle.y;
        if (new_right > right) right = new_right;
        if (new_bottom > bottom) bottom = new_bottom;
        desktop->dirty.width = right - desktop->dirty.x;
        desktop->dirty.height = bottom - desktop->dirty.y;
}

static bool point_in_rectangle(int32_t x, int32_t y,
                               struct rectangle rectangle) {
        return x >= rectangle.x && y >= rectangle.y &&
               x < rectangle.x + rectangle.width &&
               y < rectangle.y + rectangle.height;
}

static void put_pixel(struct desktop *desktop, int32_t x, int32_t y,
                      uint32_t color) {
        if (!desktop->has_dirty || x < desktop->dirty.x ||
            y < desktop->dirty.y ||
            x >= desktop->dirty.x + desktop->dirty.width ||
            y >= desktop->dirty.y + desktop->dirty.height || x < 0 || y < 0 ||
            x >= (int32_t)desktop->graphics.width ||
            y >= (int32_t)desktop->graphics.height) {
                return;
        }
        desktop->pixels[(size_t)y * desktop->pixel_stride + (size_t)x] = color;
}

static void fill_rectangle(struct desktop *desktop, int32_t x, int32_t y,
                           int32_t width, int32_t height, uint32_t color) {
        if (!desktop->has_dirty) return;
        int32_t left = x > desktop->dirty.x ? x : desktop->dirty.x;
        int32_t top = y > desktop->dirty.y ? y : desktop->dirty.y;
        int32_t right = x + width;
        int32_t bottom = y + height;
        int32_t dirty_right = desktop->dirty.x + desktop->dirty.width;
        int32_t dirty_bottom = desktop->dirty.y + desktop->dirty.height;
        if (right > dirty_right) right = dirty_right;
        if (bottom > dirty_bottom) bottom = dirty_bottom;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > (int32_t)desktop->graphics.width)
                right = (int32_t)desktop->graphics.width;
        if (bottom > (int32_t)desktop->graphics.height)
                bottom = (int32_t)desktop->graphics.height;
        for (int32_t row = top; row < bottom; row++) {
                for (int32_t column = left; column < right; column++) {
                        desktop->pixels[(size_t)row * desktop->pixel_stride +
                                        (size_t)column] = color;
                }
        }
}

static void draw_text(struct desktop *desktop, int32_t x, int32_t y,
                      const char *text, uint32_t color, uint32_t scale) {
        while (*text != '\0') {
                char character = *text++;
                if (character >= 'a' && character <= 'z')
                        character = (char)(character - 'a' + 'A');
                if (character < ' ' || character > 'Z') character = '?';
                const uint8_t *glyph = font[(uint8_t)character - ' '];
                for (int32_t glyph_x = 0; glyph_x < FONT_WIDTH; glyph_x++) {
                        for (int32_t glyph_y = 0; glyph_y < FONT_HEIGHT;
                             glyph_y++) {
                                if ((glyph[glyph_x] & (1U << glyph_y)) != 0U) {
                                        fill_rectangle(
                                            desktop,
                                            x + glyph_x * (int32_t)scale,
                                            y + glyph_y * (int32_t)scale,
                                            (int32_t)scale, (int32_t)scale,
                                            color);
                                }
                        }
                }
                x += (FONT_WIDTH + 1) * (int32_t)scale;
        }
}

static void draw_background(struct desktop *desktop) {
        int32_t start = desktop->dirty.y;
        int32_t end = start + desktop->dirty.height;
        for (int32_t y = start; y < end; y++) {
                uint32_t blue = 0x38U + ((uint32_t)y >> 5U);
                uint32_t green = 0x26U + ((uint32_t)y >> 6U);
                uint32_t color = UINT32_C(0x00170000) | (green << 8U) | blue;
                fill_rectangle(desktop, desktop->dirty.x, y,
                               desktop->dirty.width, 1, color);
        }
        fill_rectangle(desktop, 0, 0, (int32_t)desktop->graphics.width,
                       PANEL_HEIGHT, COLOR_PANEL);
        fill_rectangle(desktop, 18, 14, 26, 26, COLOR_ACCENT);
        fill_rectangle(desktop, 23, 19, 16, 16, COLOR_GREEN);
        draw_text(desktop, 58, 17, "ROSE", COLOR_WHITE, 2U);
        draw_text(desktop, 142, 21, "MULTI-PROCESS DESKTOP", COLOR_MUTED, 1U);
        draw_text(desktop, (int32_t)desktop->graphics.width - 156, 21,
                  "ESC TO EXIT", COLOR_MUTED, 1U);
}

static void draw_window(struct desktop *desktop,
                        const struct desktop_window *window, bool focused) {
        struct rectangle bounds = window_rectangle(window);
        fill_rectangle(desktop, bounds.x + WINDOW_SHADOW_X,
                       bounds.y + WINDOW_SHADOW_Y, bounds.width, bounds.height,
                       COLOR_SHADOW);
        fill_rectangle(desktop, bounds.x, bounds.y, bounds.width, bounds.height,
                       COLOR_BORDER);
        fill_rectangle(desktop, bounds.x + WINDOW_BORDER,
                       bounds.y + WINDOW_BORDER,
                       (int32_t)window->width, WINDOW_TITLE_HEIGHT - 2,
                       focused ? COLOR_TITLE_FOCUSED : COLOR_TITLE);
        fill_rectangle(desktop, bounds.x + 12, bounds.y + 11, 10, 10,
                       COLOR_RED);
        fill_rectangle(desktop, bounds.x + 30, bounds.y + 11, 10, 10,
                       COLOR_YELLOW);
        fill_rectangle(desktop, bounds.x + 48, bounds.y + 11, 10, 10,
                       COLOR_GREEN);
        draw_text(desktop, bounds.x + 72, bounds.y + 11, window->title,
                  focused ? COLOR_WHITE : COLOR_MUTED, 1U);
        if (focused) {
                fill_rectangle(desktop, bounds.x + WINDOW_BORDER,
                               bounds.y + WINDOW_TITLE_HEIGHT - 2,
                               (int32_t)window->width, 2, COLOR_ACCENT);
        }

        int32_t content_x = bounds.x + WINDOW_BORDER;
        int32_t content_y = bounds.y + WINDOW_TITLE_HEIGHT;
        int32_t left = content_x > desktop->dirty.x ? content_x
                                                    : desktop->dirty.x;
        int32_t top = content_y > desktop->dirty.y ? content_y
                                                   : desktop->dirty.y;
        int32_t right = content_x + (int32_t)window->width;
        int32_t bottom = content_y + (int32_t)window->height;
        int32_t dirty_right = desktop->dirty.x + desktop->dirty.width;
        int32_t dirty_bottom = desktop->dirty.y + desktop->dirty.height;
        if (right > dirty_right) right = dirty_right;
        if (bottom > dirty_bottom) bottom = dirty_bottom;
        for (int32_t y = top; y < bottom; y++) {
                for (int32_t x = left; x < right; x++) {
                        size_t source_y = (size_t)(y - content_y);
                        size_t source_x = (size_t)(x - content_x);
                        put_pixel(desktop, x, y,
                                  window->pixels[source_y * window->width +
                                                 source_x]);
                }
        }
}

static void draw_pointer(struct desktop *desktop) {
        for (int32_t offset = 0; offset < 18; offset++) {
                fill_rectangle(desktop, desktop->pointer_x + offset / 2,
                               desktop->pointer_y + offset, 3, 3, COLOR_WHITE);
        }
        fill_rectangle(desktop, desktop->pointer_x + 8,
                       desktop->pointer_y + 15, 7, 7, COLOR_PANEL);
}

static bool render(struct desktop *desktop) {
        if (!desktop->has_dirty) return true;
        struct rectangle dirty = desktop->dirty;
        draw_background(desktop);
        for (size_t index = 0U; index < desktop->window_count; index++) {
                draw_window(desktop, &desktop->windows[index],
                            index + 1U == desktop->window_count);
        }
        draw_pointer(desktop);
        desktop->has_dirty = false;
        return rose_graphics_flush((uint32_t)dirty.x, (uint32_t)dirty.y,
                                   (uint32_t)dirty.width,
                                   (uint32_t)dirty.height) == 0;
}

static void format_identifier(uint32_t identifier, char text[11]) {
        char reversed[10];
        size_t count = 0U;
        do {
                reversed[count++] = (char)('0' + identifier % 10U);
                identifier /= 10U;
        } while (identifier != 0U);
        size_t output = 0U;
        while (count != 0U) text[output++] = reversed[--count];
        text[output] = '\0';
}

static bool create_window(struct desktop *desktop, const char *title,
                          const char *program, int32_t x, int32_t y,
                          uint32_t width, uint32_t height) {
        if (desktop->window_count == WINDOW_LIMIT ||
            width > UINT32_MAX / sizeof(uint32_t)) {
                return false;
        }
        size_t size = ROSE_GUI_SURFACE_PIXEL_OFFSET +
                      (size_t)width * height * sizeof(uint32_t);
        struct desktop_window *window =
            &desktop->windows[desktop->window_count];
        zero_bytes(window, sizeof(*window));
        if (rose_shared_memory_create(size, &window->mapping) != 0) {
                return false;
        }
        window->title = title;
        window->program = program;
        window->x = x;
        window->y = y;
        window->width = width;
        window->height = height;
        window->surface = (struct rose_gui_surface *)window->mapping.address;
        window->pixels = (uint32_t *)(window->mapping.address +
                                      ROSE_GUI_SURFACE_PIXEL_OFFSET);
        window->surface->magic = ROSE_GUI_SURFACE_MAGIC;
        window->surface->version = ROSE_GUI_SURFACE_VERSION;
        window->surface->width = width;
        window->surface->height = height;
        window->surface->stride = width * sizeof(uint32_t);
        for (size_t pixel = 0U; pixel < (size_t)width * height; pixel++) {
                window->pixels[pixel] = UINT32_C(0x00101826);
        }

        char identifier[11];
        format_identifier(window->mapping.identifier, identifier);
        char *arguments[] = {(char *)program, identifier, NULL};
        char *environment[] = {"HOME=/", "PATH=/bin:/sbin",
                               "TERM=rose-gui", NULL};
        window->pid = rose_spawn(program, arguments, environment);
        if (window->pid < 0) {
                (void)rose_shared_memory_unmap(window->mapping.identifier);
                zero_bytes(window, sizeof(*window));
                return false;
        }
        desktop->window_count++;
        window->surface->focused = 1U;
        if (desktop->window_count > 1U) {
                desktop->windows[desktop->window_count - 2U]
                    .surface->focused = 0U;
        }
        dirty_add(desktop, window_damage_rectangle(window));
        return true;
}

static void focus_window(struct desktop *desktop, size_t index) {
        if (index >= desktop->window_count) return;
        for (size_t item = 0U; item < desktop->window_count; item++) {
                desktop->windows[item].surface->focused = 0U;
                (void)rose_event_notify(
                    &desktop->windows[item].surface->focused);
        }
        if (index + 1U != desktop->window_count) {
                struct desktop_window selected = desktop->windows[index];
                for (size_t item = index; item + 1U < desktop->window_count;
                     item++) {
                        desktop->windows[item] = desktop->windows[item + 1U];
                }
                desktop->windows[desktop->window_count - 1U] = selected;
                dirty_add(desktop, (struct rectangle){
                                       .x = 0,
                                       .y = PANEL_HEIGHT,
                                       .width = (int32_t)desktop->graphics.width,
                                       .height = (int32_t)desktop->graphics.height -
                                                 PANEL_HEIGHT,
                                   });
        }
        desktop->windows[desktop->window_count - 1U].surface->focused = 1U;
        (void)rose_event_notify(
            &desktop->windows[desktop->window_count - 1U].surface->focused);
        dirty_add(
            desktop,
            window_damage_rectangle(
                &desktop->windows[desktop->window_count - 1U]));
}

static void send_event(struct desktop_window *window,
                       const struct user_input_event *source, bool pointer) {
        if (window->surface->client_ready == 0U ||
            window->surface->client_closed != 0U) {
                return;
        }
        uint32_t write = window->surface->input_write;
        uint32_t next = (write + 1U) % ROSE_GUI_EVENT_CAPACITY;
        if (next == window->surface->input_read) {
                window->surface->input_read =
                    (window->surface->input_read + 1U) %
                    ROSE_GUI_EVENT_CAPACITY;
        }
        struct user_input_event event = *source;
        if (pointer) {
                event.x -= window->x + WINDOW_BORDER;
                event.y -= window->y + WINDOW_TITLE_HEIGHT;
        }
        window->surface->input_events[write] = event;
        __sync_synchronize();
        window->surface->input_write = next;
        (void)rose_event_notify(&window->surface->input_write);
}

static void process_pointer(struct desktop *desktop,
                            const struct user_input_event *event) {
        bool was_pressed =
            (desktop->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        bool is_pressed =
            (event->buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        dirty_add(desktop, (struct rectangle){desktop->pointer_x,
                                              desktop->pointer_y, POINTER_SIZE,
                                              POINTER_SIZE});
        desktop->pointer_x = event->x;
        desktop->pointer_y = event->y;
        dirty_add(desktop, (struct rectangle){desktop->pointer_x,
                                              desktop->pointer_y, POINTER_SIZE,
                                              POINTER_SIZE});

        if (!was_pressed && is_pressed) {
                for (size_t reverse = desktop->window_count; reverse != 0U;
                     reverse--) {
                        size_t index = reverse - 1U;
                        struct desktop_window *window =
                            &desktop->windows[index];
                        if (!point_in_rectangle(event->x, event->y,
                                                window_rectangle(window))) {
                                continue;
                        }
                        focus_window(desktop, index);
                        window = &desktop->windows[desktop->window_count - 1U];
                        if (event->y < window->y + WINDOW_TITLE_HEIGHT) {
                                if (event->x >= window->x + 7 &&
                                    event->x < window->x + 27) {
                                        window->surface->close_requested = 1U;
                                        (void)rose_event_notify(
                                            &window->surface->close_requested);
                                } else {
                                        desktop->dragging = true;
                                        desktop->drag_offset_x =
                                            event->x - window->x;
                                        desktop->drag_offset_y =
                                            event->y - window->y;
                                }
                        }
                        break;
                }
        }

        if (desktop->dragging && desktop->window_count != 0U) {
                struct desktop_window *window =
                    &desktop->windows[desktop->window_count - 1U];
                dirty_add(desktop, window_damage_rectangle(window));
                window->x = event->x - desktop->drag_offset_x;
                window->y = event->y - desktop->drag_offset_y;
                int32_t max_x = (int32_t)desktop->graphics.width -
                                (int32_t)window->width - WINDOW_BORDER * 2 - 10;
                int32_t max_y = (int32_t)desktop->graphics.height -
                                (int32_t)window->height - WINDOW_TITLE_HEIGHT -
                                WINDOW_BORDER - 10;
                if (window->x < 6) window->x = 6;
                if (window->y < PANEL_HEIGHT + 6)
                        window->y = PANEL_HEIGHT + 6;
                if (window->x > max_x) window->x = max_x;
                if (window->y > max_y) window->y = max_y;
                dirty_add(desktop, window_damage_rectangle(window));
        }
        if (!is_pressed) desktop->dragging = false;
        desktop->pointer_buttons = event->buttons;

        if (!desktop->dragging && desktop->window_count != 0U) {
                struct desktop_window *focused =
                    &desktop->windows[desktop->window_count - 1U];
                struct rectangle content = {
                    .x = focused->x + WINDOW_BORDER,
                    .y = focused->y + WINDOW_TITLE_HEIGHT,
                    .width = (int32_t)focused->width,
                    .height = (int32_t)focused->height,
                };
                if (point_in_rectangle(event->x, event->y, content)) {
                        send_event(focused, event, true);
                }
        }
}

static void collect_damage(struct desktop *desktop) {
        for (size_t index = 0U; index < desktop->window_count; index++) {
                struct desktop_window *window = &desktop->windows[index];
                uint32_t sequence = window->surface->damage_sequence;
                if (sequence == window->last_damage_sequence) continue;
                __sync_synchronize();
                struct rectangle damage = {
                    .x = window->x + WINDOW_BORDER + window->surface->damage_x,
                    .y = window->y + WINDOW_TITLE_HEIGHT +
                         window->surface->damage_y,
                    .width = window->surface->damage_width,
                    .height = window->surface->damage_height,
                };
                struct rectangle content = {
                    .x = window->x + WINDOW_BORDER,
                    .y = window->y + WINDOW_TITLE_HEIGHT,
                    .width = (int32_t)window->width,
                    .height = (int32_t)window->height,
                };
                if (damage.x < content.x) {
                        damage.width -= content.x - damage.x;
                        damage.x = content.x;
                }
                if (damage.y < content.y) {
                        damage.height -= content.y - damage.y;
                        damage.y = content.y;
                }
                if (damage.x + damage.width > content.x + content.width)
                        damage.width = content.x + content.width - damage.x;
                if (damage.y + damage.height > content.y + content.height)
                        damage.height = content.y + content.height - damage.y;
                dirty_add(desktop, damage);
                window->last_damage_sequence = sequence;
                window->surface->damage_consumed = sequence;
        }
}

static void remove_window(struct desktop *desktop, size_t index) {
        struct rectangle old =
            window_damage_rectangle(&desktop->windows[index]);
        (void)rose_shared_memory_unmap(
            desktop->windows[index].mapping.identifier);
        for (size_t item = index; item + 1U < desktop->window_count; item++) {
                desktop->windows[item] = desktop->windows[item + 1U];
        }
        desktop->window_count--;
        desktop->dragging = false;
        if (desktop->window_count != 0U) {
                desktop->windows[desktop->window_count - 1U].surface->focused =
                    1U;
                (void)rose_event_notify(
                    &desktop->windows[desktop->window_count - 1U]
                         .surface->focused);
        }
        dirty_add(desktop, old);
}

static void reap_windows(struct desktop *desktop) {
        size_t index = 0U;
        while (index < desktop->window_count) {
                int status;
                long result = rose_waitpid(desktop->windows[index].pid,
                                           &status, USER_WAIT_NO_HANG);
                if (result == desktop->windows[index].pid) {
                        remove_window(desktop, index);
                } else {
                        index++;
                }
        }
}

static void close_windows(struct desktop *desktop) {
        for (size_t index = 0U; index < desktop->window_count; index++) {
                desktop->windows[index].surface->close_requested = 1U;
                (void)rose_event_notify(
                    &desktop->windows[index].surface->close_requested);
        }
        for (size_t turns = 0U; desktop->window_count != 0U && turns < 100U;
             turns++) {
                reap_windows(desktop);
                if (desktop->window_count != 0U) {
                        struct user_wait_item child = {
                            .type = USER_WAIT_OBJECT_CHILD,
                            .events = USER_WAIT_EVENT_CHILD_EXITED,
                            .identifier = -1,
                        };
                        (void)rose_wait_events(&child, 1U,
                                               INT64_C(10000000));
                }
        }
        while (desktop->window_count != 0U) {
                (void)rose_kill(desktop->windows[0].pid,
                                USER_SIGNAL_TERMINATE);
                int status;
                (void)rose_waitpid(desktop->windows[0].pid, &status, 0U);
                remove_window(desktop, 0U);
        }
}

static long desktop_wait_for_activity(struct desktop *desktop) {
        struct user_wait_item items[WINDOW_LIMIT + 2U];
        size_t count = 0U;

        items[count++] = (struct user_wait_item){
            .type = USER_WAIT_OBJECT_INPUT,
            .events = USER_WAIT_EVENT_READABLE,
        };
        if (desktop->window_count != 0U) {
                items[count++] = (struct user_wait_item){
                    .type = USER_WAIT_OBJECT_CHILD,
                    .events = USER_WAIT_EVENT_CHILD_EXITED,
                    .identifier = -1,
                };
        }
        for (size_t index = 0U; index < desktop->window_count; index++) {
                struct desktop_window *window = &desktop->windows[index];
                items[count++] = (struct user_wait_item){
                    .type = USER_WAIT_OBJECT_SHARED_WORD,
                    .events = USER_WAIT_EVENT_CHANGED,
                    .identifier = (int64_t)(uintptr_t)&window->surface
                                      ->damage_sequence,
                    .value = window->last_damage_sequence,
                };
        }
        return rose_wait_events(items, count, -1);
}

int rose_desktop_main(int argc, char **argv) {
        struct desktop desktop;
        zero_bytes(&desktop, sizeof(desktop));
        desktop.pointer_x = 512;
        desktop.pointer_y = 384;
        if (rose_graphics_map(&desktop.graphics) != 0 ||
            desktop.graphics.pixel_format != USER_GRAPHICS_PIXEL_XRGB8888 ||
            desktop.graphics.width < 800U || desktop.graphics.height < 600U) {
                print("desktop: graphics unavailable\n");
                return 1;
        }
        desktop.pixels = (uint32_t *)desktop.graphics.framebuffer;
        desktop.pixel_stride = desktop.graphics.stride / sizeof(uint32_t);
        dirty_add(&desktop, (struct rectangle){
                                0, 0, (int32_t)desktop.graphics.width,
                                (int32_t)desktop.graphics.height});

        if (argc == 2 && strings_equal(argv[1], "--test")) {
                if (!render(&desktop)) return 2;
                print("Graphics userspace test passed\n");
                return 0;
        }

        bool created =
            create_window(&desktop, "FILES", "/bin/gui-files", 652, 82, 340,
                          480) &&
            create_window(&desktop, "SYSTEM MONITOR", "/bin/gui-monitor", 588,
                          350, 390, 380) &&
            create_window(&desktop, "TERMINAL", "/bin/gui-terminal", 28, 104,
                          570, 390);
        if (!created) {
                print("desktop: unable to start graphical applications\n");
                close_windows(&desktop);
                return 3;
        }
        if (!render(&desktop)) {
                close_windows(&desktop);
                return 4;
        }

        while (!desktop.exiting) {
                collect_damage(&desktop);
                reap_windows(&desktop);
                struct user_input_event event;
                long result;
                while ((result = rose_input_read(&event)) > 0) {
                        if (event.type == USER_INPUT_EVENT_KEY) {
                                if (event.value != 0 &&
                                    event.code == KEY_ESCAPE) {
                                        desktop.exiting = true;
                                } else if (desktop.window_count != 0U) {
                                        send_event(
                                            &desktop.windows
                                                 [desktop.window_count - 1U],
                                            &event, false);
                                }
                        } else if (event.type == USER_INPUT_EVENT_POINTER) {
                                process_pointer(&desktop, &event);
                        }
                }
                if (result < 0 || !render(&desktop)) {
                        close_windows(&desktop);
                        return 5;
                }
                if (!desktop.exiting && desktop_wait_for_activity(&desktop) <
                                            0) {
                        close_windows(&desktop);
                        return 6;
                }
        }
        close_windows(&desktop);
        return 0;
}
