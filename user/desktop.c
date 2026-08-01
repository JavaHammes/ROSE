/* Tiny userspace compositor: directly owns the mapped scanout while running. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/syscall.h"
#include "user_abi.h"

enum {
        FONT_WIDTH = 5,
        FONT_HEIGHT = 7,
        TEXT_SCALE = 2,
        KEY_ESCAPE = 1,
        KEY_Q = 16,
};

#define COLOR_PANEL UINT32_C(0x00131b2e)
#define COLOR_WINDOW UINT32_C(0x00f4f7fb)
#define COLOR_WINDOW_DARK UINT32_C(0x00172236)
#define COLOR_TEXT UINT32_C(0x001c2942)
#define COLOR_MUTED UINT32_C(0x0068778f)
#define COLOR_ACCENT UINT32_C(0x005a6ff0)
#define COLOR_GREEN UINT32_C(0x0039c685)
#define COLOR_YELLOW UINT32_C(0x00f5b942)
#define COLOR_RED UINT32_C(0x00ef6575)
#define COLOR_WHITE UINT32_C(0x00ffffff)
#define COLOR_SHADOW UINT32_C(0x002b3653)

/* Printable ASCII through Z; lowercase labels are normalized to uppercase. */
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

struct desktop {
        struct user_graphics_info graphics;
        uint32_t *pixels;
        uint32_t pixel_stride;
        int32_t window_x;
        int32_t window_y;
        int32_t pointer_x;
        int32_t pointer_y;
        int32_t drag_offset_x;
        int32_t drag_offset_y;
        uint32_t pointer_buttons;
        bool dragging;
};

static size_t string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') {
                length++;
        }
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

static void fill_rectangle(struct desktop *desktop, int32_t x, int32_t y,
                           int32_t width, int32_t height, uint32_t color) {
        if (x < 0) {
                width += x;
                x = 0;
        }
        if (y < 0) {
                height += y;
                y = 0;
        }
        if (x + width > (int32_t)desktop->graphics.width) {
                width = (int32_t)desktop->graphics.width - x;
        }
        if (y + height > (int32_t)desktop->graphics.height) {
                height = (int32_t)desktop->graphics.height - y;
        }
        if (width <= 0 || height <= 0) {
                return;
        }

        for (int32_t row = y; row < y + height; row++) {
                for (int32_t column = x; column < x + width; column++) {
                        desktop->pixels[(size_t)row * desktop->pixel_stride +
                                        (size_t)column] = color;
                }
        }
}

static void draw_text(struct desktop *desktop, int32_t x, int32_t y,
                      const char *text, uint32_t color) {
        while (*text != '\0') {
                char character = *text++;
                if (character >= 'a' && character <= 'z') {
                        character = (char)(character - 'a' + 'A');
                }
                if (character < ' ' || character > 'Z') {
                        character = '?';
                }
                const uint8_t *glyph = font[(uint8_t)character - ' '];
                for (int32_t glyph_x = 0; glyph_x < FONT_WIDTH; glyph_x++) {
                        for (int32_t glyph_y = 0; glyph_y < FONT_HEIGHT;
                             glyph_y++) {
                                if ((glyph[glyph_x] & (1U << glyph_y)) != 0U) {
                                        fill_rectangle(
                                            desktop, x + glyph_x * TEXT_SCALE,
                                            y + glyph_y * TEXT_SCALE,
                                            TEXT_SCALE, TEXT_SCALE, color);
                                }
                        }
                }
                x += (FONT_WIDTH + 1) * TEXT_SCALE;
        }
}

static void draw_background(struct desktop *desktop) {
        for (uint32_t y = 0U; y < desktop->graphics.height; y++) {
                uint32_t blue = 0x3aU + (y >> 5U);
                uint32_t green = 0x28U + (y >> 6U);
                uint32_t color = UINT32_C(0x001a0000) | (green << 8U) | blue;
                fill_rectangle(desktop, 0, (int32_t)y,
                               (int32_t)desktop->graphics.width, 1, color);
        }

        fill_rectangle(desktop, 0, 0, (int32_t)desktop->graphics.width, 48,
                       COLOR_PANEL);
        fill_rectangle(desktop, 18, 12, 24, 24, COLOR_ACCENT);
        draw_text(desktop, 56, 16, "ROSE DESKTOP", COLOR_WHITE);
        draw_text(desktop, (int32_t)desktop->graphics.width - 220, 16,
                  "Q / ESC TO RETURN", COLOR_WHITE);
}

static void draw_window(struct desktop *desktop) {
        int32_t x = desktop->window_x;
        int32_t y = desktop->window_y;
        int32_t width = 610;
        int32_t height = 440;

        fill_rectangle(desktop, x + 12, y + 14, width, height, COLOR_SHADOW);
        fill_rectangle(desktop, x, y, width, height, COLOR_WINDOW);
        fill_rectangle(desktop, x, y, width, 42, COLOR_WINDOW_DARK);
        fill_rectangle(desktop, x + 16, y + 15, 12, 12, COLOR_RED);
        fill_rectangle(desktop, x + 36, y + 15, 12, 12, COLOR_YELLOW);
        fill_rectangle(desktop, x + 56, y + 15, 12, 12, COLOR_GREEN);
        draw_text(desktop, x + 88, y + 13, "SYSTEM OVERVIEW", COLOR_WHITE);

        draw_text(desktop, x + 28, y + 72, "WELCOME TO ROSE", COLOR_TEXT);
        draw_text(desktop, x + 28, y + 104, "A SMALL RISC-V OPERATING SYSTEM",
                  COLOR_MUTED);
        fill_rectangle(desktop, x + 28, y + 142, width - 56, 2,
                       UINT32_C(0x00d8deea));

        fill_rectangle(desktop, x + 28, y + 174, 164, 104,
                       UINT32_C(0x00e8edff));
        fill_rectangle(desktop, x + 212, y + 174, 164, 104,
                       UINT32_C(0x00e5f8f0));
        fill_rectangle(desktop, x + 396, y + 174, 164, 104,
                       UINT32_C(0x00fff3dd));
        draw_text(desktop, x + 48, y + 194, "DISPLAY", COLOR_ACCENT);
        draw_text(desktop, x + 232, y + 194, "INPUT", COLOR_GREEN);
        draw_text(desktop, x + 416, y + 194, "FILES", COLOR_YELLOW);
        draw_text(desktop, x + 48, y + 236, "1024 X 768", COLOR_TEXT);
        draw_text(desktop, x + 232, y + 236, "VIRTIO", COLOR_TEXT);
        draw_text(desktop, x + 416, y + 236, "EXT2", COLOR_TEXT);

        draw_text(desktop, x + 28, y + 318,
                  "DRAG THIS TITLE BAR WITH THE TABLET", COLOR_MUTED);
        fill_rectangle(desktop, x + 28, y + 360, 300, 48, COLOR_ACCENT);
        draw_text(desktop, x + 48, y + 377, "USERSPACE COMPOSITOR",
                  COLOR_WHITE);
}

static void draw_side_card(struct desktop *desktop) {
        int32_t width = 250;
        int32_t x = (int32_t)desktop->graphics.width - width - 42;
        int32_t y = 118;

        fill_rectangle(desktop, x + 10, y + 12, width, 310, COLOR_SHADOW);
        fill_rectangle(desktop, x, y, width, 310, COLOR_WINDOW_DARK);
        draw_text(desktop, x + 24, y + 26, "GRAPHICS STACK", COLOR_WHITE);
        fill_rectangle(desktop, x + 24, y + 66, width - 48, 2,
                       UINT32_C(0x00404c65));
        draw_text(desktop, x + 24, y + 94, "VIRTIO GPU", COLOR_GREEN);
        draw_text(desktop, x + 24, y + 130, "XRGB8888", COLOR_WHITE);
        draw_text(desktop, x + 24, y + 166, "DIRTY FLUSH", COLOR_WHITE);
        draw_text(desktop, x + 24, y + 202, "KEYBOARD", COLOR_WHITE);
        draw_text(desktop, x + 24, y + 238, "TABLET", COLOR_WHITE);
        fill_rectangle(desktop, x + 24, y + 274, width - 48, 10, COLOR_GREEN);
}

static void draw_pointer(struct desktop *desktop) {
        int32_t x = desktop->pointer_x;
        int32_t y = desktop->pointer_y;

        for (int32_t offset = 0; offset < 18; offset++) {
                fill_rectangle(desktop, x + offset / 2, y + offset, 3, 3,
                               COLOR_WHITE);
        }
        fill_rectangle(desktop, x + 8, y + 15, 7, 7, COLOR_PANEL);
}

static bool redraw(struct desktop *desktop) {
        draw_background(desktop);
        draw_window(desktop);
        draw_side_card(desktop);
        draw_pointer(desktop);
        return rose_graphics_flush(0U, 0U, desktop->graphics.width,
                                   desktop->graphics.height) == 0;
}

static void process_pointer_event(struct desktop *desktop,
                                  const struct user_input_event *event) {
        bool was_pressed =
            (desktop->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        bool is_pressed = (event->buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        desktop->pointer_x = event->x;
        desktop->pointer_y = event->y;

        if (!was_pressed && is_pressed && event->x >= desktop->window_x &&
            event->x < desktop->window_x + 610 &&
            event->y >= desktop->window_y &&
            event->y < desktop->window_y + 42) {
                desktop->dragging = true;
                desktop->drag_offset_x = event->x - desktop->window_x;
                desktop->drag_offset_y = event->y - desktop->window_y;
        }
        if (!is_pressed) {
                desktop->dragging = false;
        }
        if (desktop->dragging) {
                desktop->window_x = event->x - desktop->drag_offset_x;
                desktop->window_y = event->y - desktop->drag_offset_y;
                if (desktop->window_x < 12) {
                        desktop->window_x = 12;
                }
                if (desktop->window_y < 56) {
                        desktop->window_y = 56;
                }
                if (desktop->window_x >
                    (int32_t)desktop->graphics.width - 622) {
                        desktop->window_x =
                            (int32_t)desktop->graphics.width - 622;
                }
                if (desktop->window_y >
                    (int32_t)desktop->graphics.height - 452) {
                        desktop->window_y =
                            (int32_t)desktop->graphics.height - 452;
                }
        }
        desktop->pointer_buttons = event->buttons;
}

int rose_desktop_main(int argc, char **argv) {
        struct desktop desktop = {
            .window_x = 42,
            .window_y = 92,
            .pointer_x = 512,
            .pointer_y = 384,
        };
        if (rose_graphics_map(&desktop.graphics) != 0 ||
            desktop.graphics.pixel_format != USER_GRAPHICS_PIXEL_XRGB8888 ||
            desktop.graphics.width < 800U || desktop.graphics.height < 600U) {
                print("desktop: graphics unavailable\n");
                return 1;
        }
        desktop.pixels = (uint32_t *)desktop.graphics.framebuffer;
        desktop.pixel_stride = desktop.graphics.stride / sizeof(uint32_t);
        if (!redraw(&desktop)) {
                return 2;
        }

        if (argc == 2 && strings_equal(argv[1], "--test")) {
                print("Graphics userspace test passed\n");
                return 0;
        }

        while (true) {
                struct user_input_event event;
                long result = rose_input_read(&event);
                if (result < 0) {
                        return 3;
                }
                if (result == 0) {
                        rose_yield();
                        continue;
                }
                if (event.type == USER_INPUT_EVENT_KEY && event.value != 0 &&
                    (event.code == KEY_Q || event.code == KEY_ESCAPE)) {
                        return 0;
                }
                if (event.type == USER_INPUT_EVENT_POINTER) {
                        process_pointer_event(&desktop, &event);
                        if (!redraw(&desktop)) {
                                return 4;
                        }
                }
        }
}
