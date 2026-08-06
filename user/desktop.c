/* Userspace window server for shared-memory client surfaces. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "rose/gui.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        WINDOW_LIMIT = 12,
        CAPACITY_STRESS_CYCLES = 3,
        PANEL_HEIGHT = 54,
        WINDOW_BORDER = 2,
        WINDOW_TITLE_HEIGHT = 30,
        WINDOW_SHADOW_X = 9,
        WINDOW_SHADOW_Y = 11,
        POINTER_SIZE = 22,
        KEY_ESCAPE = 1,
        KEY_TAB = 15,
        KEY_Q = 16,
        KEY_T = 20,
        KEY_L = 38,
        KEY_LEFT_CONTROL = 29,
        KEY_LEFT_ALT = 56,
        KEY_F4 = 62,
        KEY_F6 = 64,
        KEY_F7 = 65,
        KEY_F8 = 66,
        KEY_F9 = 67,
        KEY_F10 = 68,
        KEY_RIGHT_CONTROL = 97,
        KEY_RIGHT_ALT = 100,
        KEY_UP = 103,
        KEY_LEFT = 105,
        KEY_RIGHT = 106,
        KEY_DOWN = 108,
};

struct rectangle {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
};

struct desktop_window {
        char title[ROSE_GUI_APP_TITLE_LIMIT];
        char program[ROSE_GUI_APP_PATH_LIMIT];
        int32_t x;
        int32_t y;
        uint32_t width;
        uint32_t height;
        uint32_t capacity_width;
        uint32_t capacity_height;
        int32_t restore_x;
        int32_t restore_y;
        uint32_t restore_width;
        uint32_t restore_height;
        bool minimized;
        bool fullscreen;
        bool restore_valid;
        long pid;
        struct user_shared_memory_info mapping;
        struct rose_gui_surface *surface;
        uint32_t *pixels;
        uint32_t last_damage_sequence;
        uint32_t last_application_request_sequence;
};

struct desktop {
        struct user_graphics_info graphics;
        uint32_t *pixels;
        uint32_t pixel_stride;
        struct rose_gui_canvas canvas;
        struct rose_gui_theme theme;
        struct rose_gui_app_catalog catalog;
        const char *menu_items[ROSE_GUI_APP_LIMIT];
        struct rose_gui_context input_context;
        struct rose_gui_ui ui;
        struct rose_gui_widget ui_root;
        struct rose_gui_widget panel;
        struct rose_gui_widget launcher;
        struct rose_gui_widget panel_power;
        struct rose_gui_widget menu;
        struct rose_gui_widget power_dialog;
        struct rose_gui_widget power_shutdown;
        struct rose_gui_widget power_restart;
        struct rose_gui_widget power_cancel;
        struct desktop_window windows[WINDOW_LIMIT];
        size_t window_count;
        int32_t pointer_x;
        int32_t pointer_y;
        uint32_t pointer_buttons;
        bool dragging;
        bool resizing;
        bool control;
        bool alt;
        bool ui_keyboard_active;
        uint8_t keyboard_window_mode;
        int32_t drag_offset_x;
        int32_t drag_offset_y;
        struct rectangle dirty;
        bool has_dirty;
        bool exiting;
        int exit_status;
        bool compact_surfaces;
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

static void string_copy_bounded(char *destination, size_t capacity,
                                const char *source) {
        size_t index = 0U;
        while (index + 1U < capacity && source[index] != '\0') {
                destination[index] = source[index];
                index++;
        }
        destination[index] = '\0';
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
            .height = window->minimized
                          ? WINDOW_TITLE_HEIGHT
                          : (int32_t)window->height + WINDOW_TITLE_HEIGHT +
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

static void fill_rectangle(struct desktop *desktop, int32_t x, int32_t y,
                           int32_t width, int32_t height, uint32_t color) {
        rose_gui_canvas_fill(&desktop->canvas, x, y, width, height, color);
}

static void draw_text(struct desktop *desktop, int32_t x, int32_t y,
                      const char *text, uint32_t color, uint32_t scale) {
        rose_gui_canvas_text(&desktop->canvas, x, y, text, color, scale);
}

/* Keep machine transitions legible even when normal desktop resources fail to
 * load. This uses only the already mapped framebuffer and built-in colors. */
static bool show_lifecycle_screen(struct desktop *desktop, const char *title,
                                  const char *detail, uint32_t background) {
        const uint32_t title_scale = 5U;
        int32_t title_width =
            (int32_t)(string_length(title) * 6U * title_scale);
        int32_t detail_width = (int32_t)(string_length(detail) * 6U);
        int32_t center_y = (int32_t)desktop->graphics.height / 2;

        fill_rectangle(desktop, 0, 0, (int32_t)desktop->graphics.width,
                       (int32_t)desktop->graphics.height, background);
        draw_text(desktop,
                  ((int32_t)desktop->graphics.width - title_width) / 2,
                  center_y - 48, title, UINT32_C(0x00f2f5f8), title_scale);
        draw_text(desktop,
                  ((int32_t)desktop->graphics.width - detail_width) / 2,
                  center_y + 18, detail, UINT32_C(0x009ba9b8), 1U);
        return rose_graphics_flush(0U, 0U, desktop->graphics.width,
                                   desktop->graphics.height) == 0;
}

static void draw_background(struct desktop *desktop) {
        fill_rectangle(desktop, 0, 0, (int32_t)desktop->graphics.width,
                       (int32_t)desktop->graphics.height,
                       desktop->theme.background);
        rose_gui_canvas_blend(
            &desktop->canvas, 0, PANEL_HEIGHT,
            (int32_t)desktop->graphics.width,
            (int32_t)desktop->graphics.height - PANEL_HEIGHT,
            UINT32_C(0x10005f7c));
}

static void draw_window(struct desktop *desktop,
                        const struct desktop_window *window, bool focused) {
        struct rectangle bounds = window_rectangle(window);
        fill_rectangle(desktop, bounds.x + WINDOW_SHADOW_X,
                       bounds.y + WINDOW_SHADOW_Y, bounds.width, bounds.height,
                       desktop->theme.shadow);
        fill_rectangle(desktop, bounds.x, bounds.y, bounds.width, bounds.height,
                       desktop->theme.border);
        fill_rectangle(desktop, bounds.x + WINDOW_BORDER,
                       bounds.y + WINDOW_BORDER,
                       (int32_t)window->width, WINDOW_TITLE_HEIGHT - 2,
                       focused ? desktop->theme.surface_alternate
                               : desktop->theme.surface);
        uint32_t close_color = desktop->theme.error;
        uint32_t minimize_color = window->minimized ? desktop->theme.accent
                                                    : desktop->theme.warning;
        uint32_t fullscreen_color = window->fullscreen
                                        ? desktop->theme.accent
                                        : desktop->theme.success;
        bool close_hovered =
            desktop->pointer_x >= bounds.x + 7 &&
            desktop->pointer_x < bounds.x + 27 &&
            desktop->pointer_y >= bounds.y + 5 &&
            desktop->pointer_y < bounds.y + 25;
        bool minimize_hovered =
            desktop->pointer_x >= bounds.x + 25 &&
            desktop->pointer_x < bounds.x + 45 &&
            desktop->pointer_y >= bounds.y + 5 &&
            desktop->pointer_y < bounds.y + 25;
        bool fullscreen_hovered =
            desktop->pointer_x >= bounds.x + 43 &&
            desktop->pointer_x < bounds.x + 63 &&
            desktop->pointer_y >= bounds.y + 5 &&
            desktop->pointer_y < bounds.y + 25;
        if (close_hovered &&
            (desktop->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U) {
                close_color = desktop->theme.accent_pressed;
        } else if (close_hovered) {
                close_color = desktop->theme.accent_hover;
        }
        if (minimize_hovered &&
            (desktop->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U) {
                minimize_color = desktop->theme.accent_pressed;
        } else if (minimize_hovered) {
                minimize_color = desktop->theme.accent_hover;
        }
        if (fullscreen_hovered &&
            (desktop->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U) {
                fullscreen_color = desktop->theme.accent_pressed;
        } else if (fullscreen_hovered) {
                fullscreen_color = desktop->theme.accent_hover;
        }
        rose_gui_canvas_rounded_rectangle(
            &desktop->canvas,
            (struct rose_gui_rectangle){bounds.x + 12, bounds.y + 11, 10, 10},
            5, close_color);
        rose_gui_canvas_rounded_rectangle(
            &desktop->canvas,
            (struct rose_gui_rectangle){bounds.x + 30, bounds.y + 11, 10, 10},
            5, minimize_color);
        rose_gui_canvas_rounded_rectangle(
            &desktop->canvas,
            (struct rose_gui_rectangle){bounds.x + 48, bounds.y + 11, 10, 10},
            5, fullscreen_color);
        draw_text(desktop, bounds.x + 72, bounds.y + 11, window->title,
                  focused ? desktop->theme.text : desktop->theme.muted, 1U);
        if (focused) {
                fill_rectangle(desktop, bounds.x + WINDOW_BORDER,
                               bounds.y + WINDOW_TITLE_HEIGHT - 2,
                               (int32_t)window->width, 2,
                               desktop->theme.accent);
        }

        if (window->minimized) return;

        int32_t content_x = bounds.x + WINDOW_BORDER;
        int32_t content_y = bounds.y + WINDOW_TITLE_HEIGHT;
        struct rose_gui_rectangle previous = rose_gui_canvas_set_clip(
            &desktop->canvas,
            (struct rose_gui_rectangle){content_x, content_y,
                                        (int32_t)window->width,
                                        (int32_t)window->height});
        struct rose_gui_image image = {
            .pixels = window->pixels,
            .width = window->width,
            .height = window->height,
            .pixel_stride =
                window->surface->stride / sizeof(uint32_t),
            .alpha = false,
        };
        rose_gui_canvas_image(&desktop->canvas, content_x, content_y, &image);
        rose_gui_canvas_restore_clip(&desktop->canvas, previous);
        if (strings_equal(window->program, "/bin/gui-terminal")) {
                fill_rectangle(desktop, content_x + (int32_t)window->width - 10,
                               content_y + (int32_t)window->height,
                               12, WINDOW_BORDER, desktop->theme.accent);
                fill_rectangle(desktop, content_x + (int32_t)window->width,
                               content_y + (int32_t)window->height - 10,
                               WINDOW_BORDER, 12, desktop->theme.accent);
        }
}

static void draw_pointer(struct desktop *desktop) {
        for (int32_t offset = 0; offset < 18; offset++) {
                fill_rectangle(desktop, desktop->pointer_x + offset / 2,
                               desktop->pointer_y + offset, 3, 3,
                               desktop->theme.text);
        }
        fill_rectangle(desktop, desktop->pointer_x + 8,
                       desktop->pointer_y + 15, 7, 7,
                       desktop->theme.background);
}

static bool render(struct desktop *desktop) {
        if (!desktop->has_dirty) return true;
        struct rectangle dirty = desktop->dirty;
        struct rose_gui_rectangle previous = rose_gui_canvas_set_clip(
            &desktop->canvas,
            (struct rose_gui_rectangle){dirty.x, dirty.y, dirty.width,
                                        dirty.height});
        draw_background(desktop);
        for (size_t index = 0U; index < desktop->window_count; index++) {
                draw_window(desktop, &desktop->windows[index],
                            index + 1U == desktop->window_count);
        }
        rose_gui_ui_draw(&desktop->ui);
        draw_pointer(desktop);
        rose_gui_canvas_restore_clip(&desktop->canvas, previous);
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
                          uint32_t width, uint32_t height,
                          const char *argument) {
        uint32_t capacity_width = width;
        uint32_t capacity_height = height;
        if (!desktop->compact_surfaces) {
                capacity_width = desktop->graphics.width - WINDOW_BORDER * 2U;
                capacity_height = desktop->graphics.height - PANEL_HEIGHT -
                                  WINDOW_TITLE_HEIGHT - WINDOW_BORDER;
        }
        if (desktop->window_count == WINDOW_LIMIT || width > capacity_width ||
            height > capacity_height ||
            capacity_width > UINT32_MAX / sizeof(uint32_t) ||
            capacity_height > SIZE_MAX / capacity_width ||
            capacity_width * (size_t)capacity_height >
                (SIZE_MAX - ROSE_GUI_SURFACE_PIXEL_OFFSET) /
                    sizeof(uint32_t)) {
                return false;
        }
        size_t size = ROSE_GUI_SURFACE_PIXEL_OFFSET +
                      (size_t)capacity_width * capacity_height *
                          sizeof(uint32_t);
        struct desktop_window *window =
            &desktop->windows[desktop->window_count];
        zero_bytes(window, sizeof(*window));
        if (rose_shared_memory_create(size, &window->mapping) != 0) {
                return false;
        }
        string_copy_bounded(window->title, sizeof(window->title), title);
        string_copy_bounded(window->program, sizeof(window->program), program);
        window->x = x;
        window->y = y;
        window->width = width;
        window->height = height;
        window->capacity_width = capacity_width;
        window->capacity_height = capacity_height;
        window->surface = (struct rose_gui_surface *)window->mapping.address;
        window->pixels = (uint32_t *)(window->mapping.address +
                                      ROSE_GUI_SURFACE_PIXEL_OFFSET);
        window->surface->magic = ROSE_GUI_SURFACE_MAGIC;
        window->surface->version = ROSE_GUI_SURFACE_VERSION;
        window->surface->width = width;
        window->surface->height = height;
        window->surface->stride = capacity_width * sizeof(uint32_t);
        for (size_t pixel = 0U;
             pixel < (size_t)capacity_width * capacity_height; pixel++) {
                window->pixels[pixel] = UINT32_C(0x00101826);
        }

        char identifier[11];
        format_identifier(window->mapping.identifier, identifier);
        char *arguments[4] = {(char *)program, identifier, NULL, NULL};
        if (argument != NULL && argument[0] != '\0') {
                arguments[2] = (char *)argument;
        }
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
                struct rose_gui_surface *previous =
                    desktop->windows[desktop->window_count - 2U].surface;
                previous->focused = 0U;
                (void)rose_event_notify(&previous->focused);
                dirty_add(
                    desktop,
                    window_damage_rectangle(
                        &desktop->windows[desktop->window_count - 2U]));
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

static void resize_window(struct desktop_window *window, uint32_t width,
                          uint32_t height) {
        if (width == window->width && height == window->height) return;
        window->width = width;
        window->height = height;
        window->surface->width = width;
        window->surface->height = height;
        __sync_synchronize();
        window->surface->resize_sequence++;
        (void)rose_event_notify(&window->surface->resize_sequence);
}

static void toggle_window_minimized(struct desktop *desktop,
                                    struct desktop_window *window) {
        dirty_add(desktop, window_damage_rectangle(window));
        window->minimized = !window->minimized;
        desktop->dragging = false;
        desktop->resizing = false;
        dirty_add(desktop, window_damage_rectangle(window));
}

static void toggle_window_fullscreen(struct desktop *desktop,
                                     struct desktop_window *window) {
        dirty_add(desktop, window_damage_rectangle(window));
        window->minimized = false;
        if (!window->fullscreen) {
                window->restore_x = window->x;
                window->restore_y = window->y;
                window->restore_width = window->width;
                window->restore_height = window->height;
                window->restore_valid = true;
                window->x = 0;
                window->y = PANEL_HEIGHT;
                resize_window(
                    window, desktop->graphics.width - WINDOW_BORDER * 2U,
                    desktop->graphics.height - PANEL_HEIGHT -
                        WINDOW_TITLE_HEIGHT - WINDOW_BORDER);
                window->fullscreen = true;
        } else if (window->restore_valid) {
                window->x = window->restore_x;
                window->y = window->restore_y;
                resize_window(window, window->restore_width,
                              window->restore_height);
                window->fullscreen = false;
        }
        desktop->dragging = false;
        desktop->resizing = false;
        dirty_add(desktop, window_damage_rectangle(window));
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
                            const struct user_input_event *event,
                            bool route_windows) {
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

        if (!route_windows) {
                if (!is_pressed) {
                        desktop->dragging = false;
                        desktop->resizing = false;
                }
                desktop->pointer_buttons = event->buttons;
                return;
        }

        if (!was_pressed && is_pressed) {
                desktop->ui_keyboard_active = false;
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
                        bool resize_handle =
                            !window->minimized && !window->fullscreen &&
                            strings_equal(window->program,
                                          "/bin/gui-terminal") &&
                            event->x >= window->x + WINDOW_BORDER +
                                            (int32_t)window->width - 14 &&
                            event->y >= window->y + WINDOW_TITLE_HEIGHT +
                                            (int32_t)window->height - 14;
                        if (resize_handle) {
                                desktop->resizing = true;
                                desktop->dragging = false;
                        } else if (event->y <
                                   window->y + WINDOW_TITLE_HEIGHT) {
                                if (event->x >= window->x + 7 &&
                                    event->x < window->x + 27) {
                                        window->surface->close_requested = 1U;
                                        (void)rose_event_notify(
                                            &window->surface->close_requested);
                                } else if (event->x >= window->x + 25 &&
                                           event->x < window->x + 45) {
                                        toggle_window_minimized(desktop,
                                                                window);
                                } else if (event->x >= window->x + 43 &&
                                           event->x < window->x + 63) {
                                        toggle_window_fullscreen(desktop,
                                                                 window);
                                } else if (!window->fullscreen) {
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

        if (desktop->resizing && desktop->window_count != 0U) {
                struct desktop_window *window =
                    &desktop->windows[desktop->window_count - 1U];
                uint32_t width = event->x <= window->x + WINDOW_BORDER
                                     ? 1U
                                     : (uint32_t)(event->x - window->x -
                                                  WINDOW_BORDER + 1);
                uint32_t height =
                    event->y <= window->y + WINDOW_TITLE_HEIGHT
                        ? 1U
                        : (uint32_t)(event->y - window->y -
                                     WINDOW_TITLE_HEIGHT + 1);
                if (width < 180U) width = 180U;
                if (height < 100U) height = 100U;
                if (width > window->capacity_width)
                        width = window->capacity_width;
                if (height > window->capacity_height)
                        height = window->capacity_height;
                if (width != window->width || height != window->height) {
                        dirty_add(desktop, window_damage_rectangle(window));
                        resize_window(window, width, height);
                        dirty_add(desktop, window_damage_rectangle(window));
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
        if (!is_pressed) {
                desktop->dragging = false;
                desktop->resizing = false;
        }
        desktop->pointer_buttons = event->buttons;

        if (!desktop->dragging && !desktop->resizing &&
            desktop->window_count != 0U) {
                struct desktop_window *focused =
                    &desktop->windows[desktop->window_count - 1U];
                /* Out-of-content coordinates deliberately reach the focused
                 * client so retained hover/pressed states clear on leave. */
                if (!focused->minimized) send_event(focused, event, true);
        }
}

static void collect_damage(struct desktop *desktop) {
        for (size_t index = 0U; index < desktop->window_count; index++) {
                struct desktop_window *window = &desktop->windows[index];
                uint32_t sequence = window->surface->damage_sequence;
                if (sequence == window->last_damage_sequence) continue;
                __sync_synchronize();
                if (window->minimized) {
                        window->last_damage_sequence = sequence;
                        window->surface->damage_consumed = sequence;
                        continue;
                }
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

static void collect_application_requests(struct desktop *desktop) {
        size_t requesters = desktop->window_count;
        for (size_t index = 0U; index < requesters; index++) {
                struct desktop_window *source = &desktop->windows[index];
                uint32_t sequence =
                    source->surface->application_request_sequence;
                if (sequence == source->last_application_request_sequence)
                        continue;
                char title[ROSE_GUI_APP_TITLE_LIMIT];
                char program[ROSE_GUI_APP_PATH_LIMIT];
                char argument[ROSE_GUI_APPLICATION_ARGUMENT_LIMIT];
                string_copy_bounded(
                    title, sizeof(title),
                    source->surface->application_request_title);
                string_copy_bounded(
                    program, sizeof(program),
                    source->surface->application_request_program);
                string_copy_bounded(
                    argument, sizeof(argument),
                    source->surface->application_request_argument);
                uint32_t width =
                    source->surface->application_request_width;
                uint32_t height =
                    source->surface->application_request_height;
                source->last_application_request_sequence = sequence;
                source->surface->application_request_consumed = sequence;
                (void)rose_event_notify(
                    &source->surface->application_request_consumed);
                if (title[0] == '\0' || program[0] != '/' || width == 0U ||
                    height == 0U) {
                        continue;
                }
                int32_t x = 42 + (int32_t)desktop->window_count * 36;
                int32_t y = 78 + (int32_t)desktop->window_count * 24;
                int32_t max_x = (int32_t)desktop->graphics.width -
                                (int32_t)width - WINDOW_BORDER * 2 - 10;
                int32_t max_y = (int32_t)desktop->graphics.height -
                                (int32_t)height - WINDOW_TITLE_HEIGHT -
                                WINDOW_BORDER - 10;
                if (x > max_x) x = max_x;
                if (y > max_y) y = max_y;
                if (!create_window(desktop, title, program, x, y, width,
                                   height, argument)) {
                        print("desktop: unable to open requested application\n");
                }
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
        desktop->resizing = false;
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
        for (size_t turns = 0U; desktop->window_count != 0U && turns < 500U;
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
        struct user_wait_item items[WINDOW_LIMIT * 2U + 2U];
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
                items[count++] = (struct user_wait_item){
                    .type = USER_WAIT_OBJECT_SHARED_WORD,
                    .events = USER_WAIT_EVENT_CHANGED,
                    .identifier =
                        (int64_t)(uintptr_t)&window->surface
                            ->application_request_sequence,
                    .value = window->last_application_request_sequence,
                };
        }
        return rose_wait_events(items, count, -1);
}

/* A terminal window owns a frontend process, a shell process, one shared
 * surface, a PTY, and two pipes. Repeating this test exercises every capacity
 * table used by the heaviest small graphical client and verifies that teardown
 * returns both process slots and physical pages to the pre-test baseline. */
static bool capacity_stress_wait_ready(struct desktop *desktop,
                                       uint32_t expected_processes) {
        const uint64_t wait_quantum = UINT64_C(10000000);
        const uint64_t deadline =
            rose_monotonic_time() + UINT64_C(10000000000);

        while (true) {
                reap_windows(desktop);
                if (desktop->window_count != WINDOW_LIMIT) return false;

                bool clients_ready = true;
                for (size_t index = 0U; index < desktop->window_count;
                     index++) {
                        if (desktop->windows[index].surface->client_ready ==
                            0U) {
                                clients_ready = false;
                        }
                }
                struct user_system_info information;
                if (rose_system_info(&information) != 0) return false;
                if (clients_ready &&
                    information.process_count == expected_processes) {
                        return true;
                }

                uint64_t now = rose_monotonic_time();
                if (now >= deadline) return false;
                uint64_t remaining = deadline - now;
                int64_t timeout =
                    (int64_t)(remaining < wait_quantum ? remaining
                                                       : wait_quantum);
                struct user_wait_item items[WINDOW_LIMIT + 1U];
                size_t count = 0U;
                items[count++] = (struct user_wait_item){
                    .type = USER_WAIT_OBJECT_CHILD,
                    .events = USER_WAIT_EVENT_CHILD_EXITED,
                    .identifier = -1,
                };
                for (size_t index = 0U; index < desktop->window_count;
                     index++) {
                        struct desktop_window *window =
                            &desktop->windows[index];
                        if (window->surface->client_ready == 0U) {
                                items[count++] = (struct user_wait_item){
                                    .type = USER_WAIT_OBJECT_SHARED_WORD,
                                    .events = USER_WAIT_EVENT_CHANGED,
                                    .identifier =
                                        (int64_t)(uintptr_t)&window->surface
                                              ->client_ready,
                                    .value = 0U,
                                };
                        }
                }
                long result = rose_wait_events(items, count, timeout);
                if (result < 0 && result != -USER_ERROR_INTERRUPTED) {
                        return false;
                }
        }
}

static bool run_capacity_stress(struct desktop *desktop) {
        struct user_system_info baseline;
        if (rose_system_info(&baseline) != 0) return false;

        for (size_t cycle = 0U; cycle < CAPACITY_STRESS_CYCLES; cycle++) {
                for (size_t index = 0U; index < WINDOW_LIMIT; index++) {
                        int32_t x = 12 + (int32_t)(index % 4U) * 250;
                        int32_t y = 62 + (int32_t)(index / 4U) * 215;
                        if (!create_window(desktop, "TERMINAL",
                                           "/bin/gui-terminal", x, y, 220,
                                           150, NULL)) {
                                print("desktop: capacity exhausted while "
                                      "opening terminals\n");
                                close_windows(desktop);
                                return false;
                        }
                }
                if (!capacity_stress_wait_ready(
                        desktop,
                        baseline.process_count + WINDOW_LIMIT * 2U)) {
                        print("desktop: terminal startup capacity test "
                              "timed out\n");
                        close_windows(desktop);
                        return false;
                }
                collect_damage(desktop);
                if (!render(desktop)) {
                        close_windows(desktop);
                        return false;
                }
                close_windows(desktop);

                struct user_system_info released;
                if (rose_system_info(&released) != 0 ||
                    released.process_count != baseline.process_count ||
                    released.used_pages != baseline.used_pages) {
                        print("desktop: capacity resources leaked after "
                              "close\n");
                        return false;
                }
        }
        return true;
}

static void test_pointer_click(struct desktop *desktop, int32_t x, int32_t y) {
        struct user_input_event event;
        zero_bytes(&event, sizeof(event));
        event.type = USER_INPUT_EVENT_POINTER;
        event.x = x;
        event.y = y;
        event.buttons = USER_POINTER_BUTTON_LEFT;
        process_pointer(desktop, &event, true);
        event.buttons = 0U;
        process_pointer(desktop, &event, true);
}

static bool run_window_control_test(struct desktop *desktop) {
        const int32_t initial_x = 42;
        const int32_t initial_y = 78;
        const uint32_t initial_width = 500U;
        const uint32_t initial_height = 350U;
        if (!create_window(desktop, "FILES", "/bin/gui-files", initial_x,
                           initial_y, initial_width, initial_height, NULL)) {
                return false;
        }
        struct desktop_window *window = &desktop->windows[0];

        test_pointer_click(desktop, initial_x + 35, initial_y + 15);
        if (!window->minimized ||
            window_rectangle(window).height != WINDOW_TITLE_HEIGHT) {
                close_windows(desktop);
                return false;
        }
        test_pointer_click(desktop, initial_x + 35, initial_y + 15);
        if (window->minimized || window->width != initial_width ||
            window->height != initial_height) {
                close_windows(desktop);
                return false;
        }

        test_pointer_click(desktop, initial_x + 53, initial_y + 15);
        if (!window->fullscreen || window->minimized || window->x != 0 ||
            window->y != PANEL_HEIGHT ||
            window->width != desktop->graphics.width - WINDOW_BORDER * 2U ||
            window->height != desktop->graphics.height - PANEL_HEIGHT -
                                  WINDOW_TITLE_HEIGHT - WINDOW_BORDER) {
                close_windows(desktop);
                return false;
        }
        test_pointer_click(desktop, 53, PANEL_HEIGHT + 15);
        if (window->fullscreen || window->x != initial_x ||
            window->y != initial_y || window->width != initial_width ||
            window->height != initial_height) {
                close_windows(desktop);
                return false;
        }

        close_windows(desktop);
        return true;
}

static bool launch_terminal(struct desktop *desktop) {
        const struct rose_gui_app_metadata *terminal =
            rose_gui_app_find(&desktop->catalog, "terminal");
        uint32_t width = terminal != NULL ? terminal->width : 570U;
        uint32_t height = terminal != NULL ? terminal->height : 390U;
        int32_t x = 28 + (int32_t)desktop->window_count * 34;
        int32_t y = 74 + (int32_t)desktop->window_count * 22;
        int32_t max_x = (int32_t)desktop->graphics.width -
                        (int32_t)width - WINDOW_BORDER * 2 - 10;
        int32_t max_y = (int32_t)desktop->graphics.height -
                        (int32_t)height -
                        WINDOW_TITLE_HEIGHT - WINDOW_BORDER - 10;
        if (x > max_x) x = max_x;
        if (y > max_y) y = max_y;
        return create_window(
            desktop, terminal != NULL ? terminal->title : "TERMINAL",
            terminal != NULL ? terminal->program : "/bin/gui-terminal", x, y,
            width, height, NULL);
}

static void invalidate_desktop(struct desktop *desktop) {
        dirty_add(desktop,
                  (struct rectangle){0, 0,
                                     (int32_t)desktop->graphics.width,
                                     (int32_t)desktop->graphics.height});
        rose_gui_ui_invalidate(&desktop->ui);
}

static bool launch_catalog_app(struct desktop *desktop, size_t index) {
        if (index >= desktop->catalog.count) return false;
        const struct rose_gui_app_metadata *app = &desktop->catalog.apps[index];
        int32_t x = 42 + (int32_t)desktop->window_count * 36;
        int32_t y = 78 + (int32_t)desktop->window_count * 24;
        int32_t max_x = (int32_t)desktop->graphics.width -
                        (int32_t)app->width - WINDOW_BORDER * 2 - 10;
        int32_t max_y = (int32_t)desktop->graphics.height -
                        (int32_t)app->height - WINDOW_TITLE_HEIGHT -
                        WINDOW_BORDER - 10;
        if (x > max_x) x = max_x;
        if (y > max_y) y = max_y;
        return create_window(desktop, app->title, app->program, x, y,
                             app->width, app->height, NULL);
}

static void launcher_action(struct rose_gui_widget *widget,
                            enum rose_gui_widget_action action,
                            void *user_data) {
        (void)widget;
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct desktop *desktop = user_data;
        desktop->menu.state ^= ROSE_GUI_STATE_HIDDEN;
        if ((desktop->menu.state & ROSE_GUI_STATE_HIDDEN) == 0U) {
                rose_gui_ui_focus(&desktop->ui, &desktop->menu);
                desktop->ui_keyboard_active = true;
        }
        invalidate_desktop(desktop);
}

static void show_desktop_dialog(struct desktop *desktop, const char *message,
                                bool power_options);

static void menu_action(struct rose_gui_widget *widget,
                        enum rose_gui_widget_action action, void *user_data) {
        if (action != ROSE_GUI_ACTION_SELECT) return;
        struct desktop *desktop = user_data;
        bool launched =
            launch_catalog_app(desktop, widget->selected_index);
        widget->state |= ROSE_GUI_STATE_HIDDEN;
        if (!launched) {
                show_desktop_dialog(desktop,
                                    "APPLICATION FAILED TO START", false);
        } else {
                rose_gui_ui_focus(&desktop->ui, &desktop->launcher);
                invalidate_desktop(desktop);
        }
}

static void power_button_action(struct rose_gui_widget *widget,
                                enum rose_gui_widget_action action,
                                void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct desktop *desktop = user_data;
        if (widget == &desktop->power_shutdown) {
                desktop->exiting = true;
                desktop->exit_status = USER_SYSTEM_ACTION_SHUTDOWN;
        } else if (widget == &desktop->power_restart) {
                desktop->exiting = true;
                desktop->exit_status = USER_SYSTEM_ACTION_RESTART;
        } else {
                desktop->power_dialog.state |= ROSE_GUI_STATE_HIDDEN;
                rose_gui_ui_focus(&desktop->ui, &desktop->launcher);
                invalidate_desktop(desktop);
        }
}

static void show_desktop_dialog(struct desktop *desktop, const char *message,
                                bool power_options) {
        desktop->menu.state |= ROSE_GUI_STATE_HIDDEN;
        desktop->power_dialog.text = message;
        desktop->power_dialog.state &= ~ROSE_GUI_STATE_HIDDEN;
        if (power_options) {
                desktop->power_shutdown.state &= ~ROSE_GUI_STATE_HIDDEN;
                desktop->power_restart.state &= ~ROSE_GUI_STATE_HIDDEN;
        } else {
                desktop->power_shutdown.state |= ROSE_GUI_STATE_HIDDEN;
                desktop->power_restart.state |= ROSE_GUI_STATE_HIDDEN;
        }
        desktop->ui_keyboard_active = true;
        rose_gui_ui_focus(&desktop->ui, &desktop->power_cancel);
        invalidate_desktop(desktop);
}

static void show_power_dialog(struct desktop *desktop) {
        show_desktop_dialog(desktop, "POWER OPTIONS", true);
}

static void panel_power_action(struct rose_gui_widget *widget,
                               enum rose_gui_widget_action action,
                               void *user_data) {
        (void)widget;
        if (action == ROSE_GUI_ACTION_ACTIVATE)
                show_power_dialog(user_data);
}

static void build_desktop_ui(struct desktop *desktop) {
        rose_gui_widget_initialize(&desktop->ui_root, ROSE_GUI_WIDGET_ROOT,
                                   NULL);
        desktop->ui_root.flags |= ROSE_GUI_WIDGET_TRANSPARENT;
        desktop->ui_root.padding = 0U;
        desktop->ui_root.gap = 0U;

        rose_gui_widget_initialize(&desktop->panel,
                                   ROSE_GUI_WIDGET_STATUS_BAR, NULL);
        desktop->panel.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        desktop->panel.bounds = (struct rose_gui_rectangle){
            0, 0, (int32_t)desktop->graphics.width, PANEL_HEIGHT};
        rose_gui_widget_add(&desktop->ui_root, &desktop->panel);

        rose_gui_widget_initialize(&desktop->launcher, ROSE_GUI_WIDGET_BUTTON,
                                   "ROSE");
        desktop->launcher.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        desktop->launcher.bounds =
            (struct rose_gui_rectangle){12, 12, 82, 30};
        desktop->launcher.callback = launcher_action;
        desktop->launcher.user_data = desktop;
        rose_gui_widget_add(&desktop->ui_root, &desktop->launcher);

        rose_gui_widget_initialize(&desktop->panel_power,
                                   ROSE_GUI_WIDGET_BUTTON, "POWER");
        desktop->panel_power.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        desktop->panel_power.bounds = (struct rose_gui_rectangle){
            (int32_t)desktop->graphics.width - 70, 12, 58, 30};
        desktop->panel_power.callback = panel_power_action;
        desktop->panel_power.user_data = desktop;
        rose_gui_widget_add(&desktop->ui_root, &desktop->panel_power);

        for (size_t index = 0U; index < desktop->catalog.count; index++) {
                const struct rose_gui_app_metadata *app =
                    &desktop->catalog.apps[index];
                desktop->menu_items[index] = app->title;
        }

        rose_gui_items_initialize(&desktop->menu, ROSE_GUI_WIDGET_MENU,
                                  desktop->menu_items,
                                  desktop->catalog.count);
        desktop->menu.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        desktop->menu.bounds = (struct rose_gui_rectangle){
            12, PANEL_HEIGHT + 4, 210,
            10 + (int32_t)desktop->catalog.count * 22};
        desktop->menu.state |= ROSE_GUI_STATE_HIDDEN;
        desktop->menu.callback = menu_action;
        desktop->menu.user_data = desktop;
        rose_gui_widget_add(&desktop->ui_root, &desktop->menu);

        rose_gui_widget_initialize(&desktop->power_dialog,
                                   ROSE_GUI_WIDGET_DIALOG, "POWER OPTIONS");
        desktop->power_dialog.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        desktop->power_dialog.padding = 38U;
        desktop->power_dialog.gap = 10U;
        desktop->power_dialog.bounds = (struct rose_gui_rectangle){
            ((int32_t)desktop->graphics.width - 330) / 2,
            ((int32_t)desktop->graphics.height - 160) / 2, 330, 160};
        desktop->power_dialog.state |= ROSE_GUI_STATE_HIDDEN;

        rose_gui_widget_initialize(&desktop->power_shutdown,
                                   ROSE_GUI_WIDGET_BUTTON, "SHUT DOWN");
        rose_gui_widget_initialize(&desktop->power_restart,
                                   ROSE_GUI_WIDGET_BUTTON, "RESTART");
        rose_gui_widget_initialize(&desktop->power_cancel,
                                   ROSE_GUI_WIDGET_BUTTON, "CANCEL");
        rose_gui_widget_set_flex(&desktop->power_shutdown, 1U);
        rose_gui_widget_set_flex(&desktop->power_restart, 1U);
        rose_gui_widget_set_flex(&desktop->power_cancel, 1U);
        desktop->power_shutdown.callback = power_button_action;
        desktop->power_restart.callback = power_button_action;
        desktop->power_cancel.callback = power_button_action;
        desktop->power_shutdown.user_data = desktop;
        desktop->power_restart.user_data = desktop;
        desktop->power_cancel.user_data = desktop;
        rose_gui_widget_add(&desktop->power_dialog,
                            &desktop->power_shutdown);
        rose_gui_widget_add(&desktop->power_dialog, &desktop->power_restart);
        rose_gui_widget_add(&desktop->power_dialog, &desktop->power_cancel);
        rose_gui_widget_add(&desktop->ui_root, &desktop->power_dialog);

        rose_gui_ui_initialize(&desktop->ui, &desktop->canvas,
                               &desktop->theme, &desktop->ui_root);
        rose_gui_ui_layout(&desktop->ui,
                           (struct rose_gui_rectangle){
                               0, 0, (int32_t)desktop->graphics.width,
                               (int32_t)desktop->graphics.height});
}

static void set_window_keyboard_mode(struct desktop *desktop, uint8_t mode) {
        desktop->keyboard_window_mode = mode;
        dirty_add(desktop, (struct rectangle){
                               0, 0, (int32_t)desktop->graphics.width,
                               PANEL_HEIGHT});
}

static bool handle_window_keyboard(struct desktop *desktop,
                                   const struct user_input_event *event) {
        if (event->type != USER_INPUT_EVENT_KEY || event->value == 0)
                return false;
        if (desktop->keyboard_window_mode != 0U &&
            event->code == KEY_ESCAPE) {
                set_window_keyboard_mode(desktop, 0U);
                return true;
        }
        if (desktop->alt && event->code == KEY_TAB &&
            desktop->window_count > 1U) {
                focus_window(desktop, 0U);
                return true;
        }
        if (desktop->window_count == 0U) return false;
        struct desktop_window *window =
            &desktop->windows[desktop->window_count - 1U];
        if (desktop->alt && event->code == KEY_F4) {
                window->surface->close_requested = 1U;
                (void)rose_event_notify(&window->surface->close_requested);
                return true;
        }
        if (desktop->alt && event->code == KEY_F9) {
                toggle_window_minimized(desktop, window);
                return true;
        }
        if (desktop->alt && event->code == KEY_F10) {
                toggle_window_fullscreen(desktop, window);
                return true;
        }
        if (desktop->alt && event->code == KEY_F7) {
                if (window->minimized || window->fullscreen) return true;
                set_window_keyboard_mode(desktop, 1U);
                desktop->ui_keyboard_active = false;
                return true;
        }
        if (desktop->alt && event->code == KEY_F8) {
                if (window->minimized || window->fullscreen) return true;
                set_window_keyboard_mode(desktop, 2U);
                desktop->ui_keyboard_active = false;
                return true;
        }
        if (desktop->keyboard_window_mode == 0U ||
            (event->code != KEY_LEFT && event->code != KEY_RIGHT &&
             event->code != KEY_UP && event->code != KEY_DOWN)) {
                return false;
        }

        dirty_add(desktop, window_damage_rectangle(window));
        if (desktop->keyboard_window_mode == 1U) {
                if (event->code == KEY_LEFT) window->x -= 10;
                if (event->code == KEY_RIGHT) window->x += 10;
                if (event->code == KEY_UP) window->y -= 10;
                if (event->code == KEY_DOWN) window->y += 10;
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
        } else {
                uint32_t width = window->width;
                uint32_t height = window->height;
                if (event->code == KEY_LEFT && width > 180U) width -= 10U;
                if (event->code == KEY_RIGHT &&
                    width < window->capacity_width)
                        width += 10U;
                if (event->code == KEY_UP && height > 100U) height -= 10U;
                if (event->code == KEY_DOWN &&
                    height < window->capacity_height)
                        height += 10U;
                if (width > window->capacity_width)
                        width = window->capacity_width;
                if (height > window->capacity_height)
                        height = window->capacity_height;
                if (width != window->width || height != window->height) {
                        resize_window(window, width, height);
                }
        }
        dirty_add(desktop, window_damage_rectangle(window));
        return true;
}

int rose_desktop_main(int argc, char **argv) {
        static struct desktop desktop;
        bool session_mode =
            argc == 2 && strings_equal(argv[1], "--session");
        zero_bytes(&desktop, sizeof(desktop));
        desktop.compact_surfaces =
            argc == 2 && strings_equal(argv[1], "--stress");
        desktop.pointer_x = 512;
        desktop.pointer_y = 384;
        if (!rose_font_load()) {
                print("desktop: font resource unavailable\n");
                return 1;
        }
        if (rose_graphics_map(&desktop.graphics) != 0 ||
            desktop.graphics.pixel_format != USER_GRAPHICS_PIXEL_XRGB8888 ||
            desktop.graphics.width < 800U || desktop.graphics.height < 600U) {
                print("desktop: graphics unavailable\n");
                return 1;
        }
        desktop.pixels = (uint32_t *)desktop.graphics.framebuffer;
        desktop.pixel_stride = desktop.graphics.stride / sizeof(uint32_t);
        rose_gui_canvas_initialize(&desktop.canvas, desktop.pixels,
                                   desktop.graphics.width,
                                   desktop.graphics.height,
                                   desktop.pixel_stride);
        if (session_mode &&
            !show_lifecycle_screen(&desktop, "ROSE", "STARTING SYSTEM",
                                   UINT32_C(0x000c1828))) {
                return 2;
        }
        if (!rose_gui_theme_load(&desktop.theme, "/share/gui/theme.conf")) {
                print("desktop: theme resource unavailable\n");
                if (session_mode) {
                        (void)show_lifecycle_screen(
                            &desktop, "ROSE", "STARTUP FAILED: THEME MISSING",
                            UINT32_C(0x002b1118));
                }
                return 1;
        }
        if (!rose_gui_app_catalog_load(&desktop.catalog,
                                       "/share/gui/apps.conf")) {
                print("desktop: application metadata unavailable\n");
                if (session_mode) {
                        (void)show_lifecycle_screen(
                            &desktop, "ROSE",
                            "STARTUP FAILED: APPLICATION DATA MISSING",
                            UINT32_C(0x002b1118));
                }
                return 1;
        }
        desktop.input_context.width = desktop.graphics.width;
        desktop.input_context.height = desktop.graphics.height;
        build_desktop_ui(&desktop);
        dirty_add(&desktop, (struct rectangle){
                                0, 0, (int32_t)desktop.graphics.width,
                                (int32_t)desktop.graphics.height});

        if (argc == 2 && strings_equal(argv[1], "--test")) {
                if (!render(&desktop)) return 2;
                print("Graphics userspace test passed\n");
                return 0;
        }
        if (argc == 2 && strings_equal(argv[1], "--stress")) {
                if (!run_capacity_stress(&desktop)) return 7;
                print("GUI capacity stress passed\n");
                return 0;
        }
        if (argc == 2 && strings_equal(argv[1], "--test-controls")) {
                if (!run_window_control_test(&desktop)) return 8;
                print("Window control test passed\n");
                return 0;
        }

        if (!render(&desktop)) {
                close_windows(&desktop);
                return 4;
        }

        while (!desktop.exiting) {
                collect_damage(&desktop);
                collect_application_requests(&desktop);
                reap_windows(&desktop);
                struct user_input_event event;
                long result;
                while ((result = rose_input_read(&event)) > 0) {
                        if (event.type == USER_INPUT_EVENT_KEY) {
                                bool pressed = event.value != 0U;
                                if (event.code == KEY_LEFT_CONTROL ||
                                    event.code == KEY_RIGHT_CONTROL) {
                                        desktop.control = pressed;
                                }
                                if (event.code == KEY_LEFT_ALT ||
                                    event.code == KEY_RIGHT_ALT) {
                                        desktop.alt = pressed;
                                }
                                if (event.value == 1U && desktop.control &&
                                    desktop.alt &&
                                    (event.code == KEY_ESCAPE ||
                                     event.code == KEY_Q)) {
                                        show_power_dialog(&desktop);
                                } else if (event.value == 1U &&
                                           desktop.control && desktop.alt &&
                                           event.code == KEY_T) {
                                        if (!launch_terminal(&desktop)) {
                                                print("desktop: unable to open "
                                                      "terminal\n");
                                                show_desktop_dialog(
                                                    &desktop,
                                                    "TERMINAL FAILED TO START",
                                                    false);
                                        }
                                } else if (event.value == 1U &&
                                           desktop.control && desktop.alt &&
                                           event.code == KEY_L) {
                                        launcher_action(
                                            &desktop.launcher,
                                            ROSE_GUI_ACTION_ACTIVATE,
                                            &desktop);
                                } else if (event.value == 1U &&
                                           event.code == KEY_F6) {
                                        desktop.ui_keyboard_active = true;
                                        rose_gui_ui_focus(&desktop.ui,
                                                          &desktop.launcher);
                                        dirty_add(
                                            &desktop,
                                            (struct rectangle){
                                                0, 0,
                                                (int32_t)desktop.graphics.width,
                                                PANEL_HEIGHT});
                                } else if (
                                    (desktop.power_dialog.state &
                                     ROSE_GUI_STATE_HIDDEN) == 0U) {
                                        (void)rose_gui_ui_handle_event(
                                            &desktop.ui,
                                            &desktop.input_context, &event);
                                        if (desktop.ui.dirty)
                                                invalidate_desktop(&desktop);
                                } else if (handle_window_keyboard(&desktop,
                                                                  &event)) {
                                        /* Window management consumed it. */
                                } else if (desktop.ui_keyboard_active ||
                                           (desktop.menu.state &
                                            ROSE_GUI_STATE_HIDDEN) == 0U) {
                                        bool handled =
                                            rose_gui_ui_handle_event(
                                                &desktop.ui,
                                                &desktop.input_context,
                                                &event);
                                        if (desktop.ui.dirty)
                                                invalidate_desktop(&desktop);
                                        if (!handled &&
                                            event.value == 1U &&
                                            event.code == KEY_ESCAPE) {
                                                desktop.ui_keyboard_active =
                                                    false;
                                        }
                                } else if (desktop.window_count != 0U) {
                                        send_event(
                                            &desktop.windows
                                                 [desktop.window_count - 1U],
                                            &event, false);
                                }
                        } else if (event.type == USER_INPUT_EVENT_POINTER) {
                                bool handled = rose_gui_ui_handle_event(
                                    &desktop.ui, &desktop.input_context,
                                    &event);
                                if (handled)
                                        desktop.ui_keyboard_active = true;
                                if (desktop.ui.dirty)
                                        invalidate_desktop(&desktop);
                                process_pointer(&desktop, &event, !handled);
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
        if (desktop.exit_status == USER_SYSTEM_ACTION_RESTART) {
                (void)show_lifecycle_screen(
                    &desktop, "ROSE", "RESTARTING SYSTEM",
                    UINT32_C(0x000c1828));
        } else if (desktop.exit_status == USER_SYSTEM_ACTION_SHUTDOWN) {
                (void)show_lifecycle_screen(
                    &desktop, "ROSE", "SHUTTING DOWN APPLICATIONS",
                    UINT32_C(0x00090f18));
        }
        close_windows(&desktop);
        return desktop.exit_status;
}
