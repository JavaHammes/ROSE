#ifndef ROSE_USER_GUI_H
#define ROSE_USER_GUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "user_abi.h"

enum {
        ROSE_GUI_SURFACE_MAGIC = 0x52475549U,
        ROSE_GUI_SURFACE_VERSION = 2,
        ROSE_GUI_SURFACE_PIXEL_OFFSET = 4096,
        ROSE_GUI_EVENT_CAPACITY = 32,
        ROSE_GUI_FONT_WIDTH = ROSE_FONT_WIDTH,
        ROSE_GUI_FONT_HEIGHT = ROSE_FONT_HEIGHT,
        ROSE_GUI_ICON_SIZE = 8,
        ROSE_GUI_APP_LIMIT = 8,
        ROSE_GUI_APP_NAME_LIMIT = 24,
        ROSE_GUI_APP_TITLE_LIMIT = 32,
        ROSE_GUI_APP_PATH_LIMIT = 48,
};

struct rose_gui_rectangle {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
};

/* Drawing is deliberately independent of window transport. The compositor,
 * clients, tests, and future image-backed controls can all target a canvas. */
struct rose_gui_canvas {
        uint32_t *pixels;
        uint32_t width;
        uint32_t height;
        uint32_t pixel_stride;
        struct rose_gui_rectangle clip;
};

struct rose_gui_image {
        const uint32_t *pixels;
        uint32_t width;
        uint32_t height;
        uint32_t pixel_stride;
        bool alpha;
};

struct rose_gui_icon {
        uint8_t rows[ROSE_GUI_ICON_SIZE];
};

struct rose_gui_surface {
        uint32_t magic;
        uint32_t version;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        volatile uint32_t client_ready;
        volatile uint32_t close_requested;
        volatile uint32_t client_closed;
        volatile uint32_t focused;
        volatile uint32_t damage_sequence;
        volatile uint32_t damage_consumed;
        volatile int32_t damage_x;
        volatile int32_t damage_y;
        volatile int32_t damage_width;
        volatile int32_t damage_height;
        volatile uint32_t resize_sequence;
        volatile uint32_t input_read;
        volatile uint32_t input_write;
        struct user_input_event input_events[ROSE_GUI_EVENT_CAPACITY];
};

struct rose_gui_context {
        uint32_t identifier;
        struct user_shared_memory_info mapping;
        struct rose_gui_surface *surface;
        uint32_t *pixels;
        uint32_t width;
        uint32_t height;
        uint32_t pixel_stride;
        struct rose_gui_canvas canvas;
        uint32_t observed_resize_sequence;
        uint32_t observed_focus;
        bool shift;
        bool control;
        bool caps_lock;
};

struct rose_gui_theme {
        uint32_t background;
        uint32_t surface;
        uint32_t surface_alternate;
        uint32_t text;
        uint32_t muted;
        uint32_t accent;
        uint32_t accent_hover;
        uint32_t accent_pressed;
        uint32_t on_accent;
        uint32_t border;
        uint32_t focus;
        uint32_t error;
        uint32_t selected;
        uint32_t disabled;
        uint32_t shadow;
        uint32_t success;
        uint32_t warning;
        uint16_t padding;
        uint16_t gap;
        uint16_t control_height;
        uint16_t corner_radius;
};

struct rose_gui_app_metadata {
        char name[ROSE_GUI_APP_NAME_LIMIT];
        char title[ROSE_GUI_APP_TITLE_LIMIT];
        char program[ROSE_GUI_APP_PATH_LIMIT];
        char icon[ROSE_GUI_APP_NAME_LIMIT];
        uint32_t width;
        uint32_t height;
};

struct rose_gui_app_catalog {
        struct rose_gui_app_metadata apps[ROSE_GUI_APP_LIMIT];
        size_t count;
};

enum rose_gui_widget_type {
        ROSE_GUI_WIDGET_ROOT,
        ROSE_GUI_WIDGET_ROW,
        ROSE_GUI_WIDGET_COLUMN,
        ROSE_GUI_WIDGET_LABEL,
        ROSE_GUI_WIDGET_BUTTON,
        ROSE_GUI_WIDGET_ICON_BUTTON,
        ROSE_GUI_WIDGET_TEXT_FIELD,
        ROSE_GUI_WIDGET_CHECKBOX,
        ROSE_GUI_WIDGET_LIST,
        ROSE_GUI_WIDGET_SCROLLBAR,
        ROSE_GUI_WIDGET_MENU,
        ROSE_GUI_WIDGET_DIALOG,
        ROSE_GUI_WIDGET_TABS,
        ROSE_GUI_WIDGET_STATUS_BAR,
        ROSE_GUI_WIDGET_CUSTOM,
};

enum rose_gui_widget_state {
        ROSE_GUI_STATE_HOVERED = 1U << 0,
        ROSE_GUI_STATE_PRESSED = 1U << 1,
        ROSE_GUI_STATE_DISABLED = 1U << 2,
        ROSE_GUI_STATE_FOCUSED = 1U << 3,
        ROSE_GUI_STATE_SELECTED = 1U << 4,
        ROSE_GUI_STATE_ERROR = 1U << 5,
        ROSE_GUI_STATE_HIDDEN = 1U << 6,
};

enum rose_gui_widget_flag {
        ROSE_GUI_WIDGET_FOCUSABLE = 1U << 0,
        ROSE_GUI_WIDGET_FLEX = 1U << 1,
        ROSE_GUI_WIDGET_ABSOLUTE = 1U << 2,
        ROSE_GUI_WIDGET_SURFACE = 1U << 3,
        ROSE_GUI_WIDGET_TRANSPARENT = 1U << 4,
};

enum rose_gui_widget_action {
        ROSE_GUI_ACTION_ACTIVATE,
        ROSE_GUI_ACTION_CHANGE,
        ROSE_GUI_ACTION_SELECT,
        ROSE_GUI_ACTION_DISMISS,
};

struct rose_gui_widget;
struct rose_gui_ui;
typedef void (*rose_gui_widget_callback)(struct rose_gui_widget *widget,
                                         enum rose_gui_widget_action action,
                                         void *user_data);
typedef void (*rose_gui_custom_draw_callback)(
    struct rose_gui_widget *widget, struct rose_gui_canvas *canvas,
    const struct rose_gui_theme *theme, void *user_data);

/* Widgets are caller-owned and linked without allocation. This keeps the
 * retained tree deterministic and suitable for tiny freestanding programs. */
struct rose_gui_widget {
        enum rose_gui_widget_type type;
        uint32_t state;
        uint32_t flags;
        struct rose_gui_rectangle bounds;
        int32_t minimum_width;
        int32_t minimum_height;
        uint16_t padding;
        uint16_t gap;
        uint16_t flex;
        uint16_t reserved;
        const char *text;
        const struct rose_gui_icon *icon;
        char *text_buffer;
        size_t text_capacity;
        size_t text_length;
        const char *const *items;
        size_t item_count;
        size_t selected_index;
        int32_t value;
        int32_t maximum;
        int32_t page;
        struct rose_gui_widget *parent;
        struct rose_gui_widget *first_child;
        struct rose_gui_widget *last_child;
        struct rose_gui_widget *next_sibling;
        rose_gui_widget_callback callback;
        rose_gui_custom_draw_callback custom_draw;
        void *user_data;
};

struct rose_gui_ui {
        struct rose_gui_canvas *canvas;
        const struct rose_gui_theme *theme;
        struct rose_gui_widget *root;
        struct rose_gui_widget *hovered;
        struct rose_gui_widget *pressed;
        struct rose_gui_widget *focused;
        uint32_t pointer_buttons;
        bool dirty;
        bool dismissed;
};

enum rose_gui_loop_result {
        ROSE_GUI_LOOP_EVENT = 1U << 0,
        ROSE_GUI_LOOP_RESIZED = 1U << 1,
        ROSE_GUI_LOOP_FOCUS_CHANGED = 1U << 2,
        ROSE_GUI_LOOP_CLOSE = 1U << 3,
};

typedef void (*rose_gui_event_callback)(struct rose_gui_context *context,
                                        const struct user_input_event *event,
                                        void *user_data);

struct rose_gui_application;
typedef bool (*rose_gui_update_callback)(struct rose_gui_application *app,
                                         uint64_t now, void *user_data);

struct rose_gui_application {
        struct rose_gui_context context;
        struct rose_gui_theme theme;
        struct rose_gui_ui ui;
        struct rose_gui_widget *root;
        uint64_t update_interval;
        rose_gui_update_callback update;
        void *user_data;
};

/* Transport and common event loop. */
bool rose_gui_connect(const char *identifier, struct rose_gui_context *context);
bool rose_gui_refresh_size(struct rose_gui_context *context);
void rose_gui_disconnect(struct rose_gui_context *context);
void rose_gui_present(struct rose_gui_context *context, int32_t x, int32_t y,
                      int32_t width, int32_t height);
bool rose_gui_poll_event(struct rose_gui_context *context,
                         struct user_input_event *event);
long rose_gui_wait(struct rose_gui_context *context,
                   int64_t timeout_nanoseconds);
char rose_gui_key_character(struct rose_gui_context *context,
                            const struct user_input_event *event);
uint32_t rose_gui_event_loop_poll(struct rose_gui_context *context,
                                  struct rose_gui_ui *ui,
                                  rose_gui_event_callback callback,
                                  void *user_data);
bool rose_gui_application_initialize(struct rose_gui_application *app,
                                     const char *identifier,
                                     struct rose_gui_widget *root);
int rose_gui_application_run(struct rose_gui_application *app);
void rose_gui_application_render(struct rose_gui_application *app);

/* Pixel drawing, clipping, compositing, text, and images. */
void rose_gui_canvas_initialize(struct rose_gui_canvas *canvas,
                                uint32_t *pixels, uint32_t width,
                                uint32_t height, uint32_t pixel_stride);
struct rose_gui_rectangle rose_gui_canvas_set_clip(
    struct rose_gui_canvas *canvas, struct rose_gui_rectangle clip);
void rose_gui_canvas_restore_clip(struct rose_gui_canvas *canvas,
                                  struct rose_gui_rectangle clip);
void rose_gui_canvas_fill(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          int32_t width, int32_t height, uint32_t color);
void rose_gui_canvas_blend(struct rose_gui_canvas *canvas, int32_t x,
                           int32_t y, int32_t width, int32_t height,
                           uint32_t color);
void rose_gui_canvas_line(struct rose_gui_canvas *canvas, int32_t x0,
                          int32_t y0, int32_t x1, int32_t y1,
                          uint32_t color);
void rose_gui_canvas_border(struct rose_gui_canvas *canvas,
                            struct rose_gui_rectangle rectangle,
                            int32_t thickness, uint32_t color);
void rose_gui_canvas_rounded_rectangle(struct rose_gui_canvas *canvas,
                                       struct rose_gui_rectangle rectangle,
                                       int32_t radius, uint32_t color);
void rose_gui_canvas_image(struct rose_gui_canvas *canvas, int32_t x,
                           int32_t y, const struct rose_gui_image *image);
void rose_gui_canvas_text(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          const char *text, uint32_t color, uint32_t scale);
void rose_gui_canvas_icon(struct rose_gui_canvas *canvas, int32_t x, int32_t y,
                          const struct rose_gui_icon *icon, uint32_t color,
                          uint32_t scale);
int32_t rose_gui_text_width(const char *text, uint32_t scale);

/* Compatibility conveniences for applications that draw custom content. */
void rose_gui_fill(struct rose_gui_context *context, int32_t x, int32_t y,
                   int32_t width, int32_t height, uint32_t color);
void rose_gui_text(struct rose_gui_context *context, int32_t x, int32_t y,
                   const char *text, uint32_t color, uint32_t scale);

/* File-backed visual resources. */
void rose_gui_theme_defaults(struct rose_gui_theme *theme);
bool rose_gui_theme_load(struct rose_gui_theme *theme, const char *path);
bool rose_gui_icon_load(struct rose_gui_icon *icon, const char *name,
                        const char *path);
bool rose_gui_app_catalog_load(struct rose_gui_app_catalog *catalog,
                               const char *path);
const struct rose_gui_app_metadata *rose_gui_app_find(
    const struct rose_gui_app_catalog *catalog, const char *name);

/* Retained widgets and deterministic minimum-size row/column layout. */
void rose_gui_widget_initialize(struct rose_gui_widget *widget,
                                enum rose_gui_widget_type type,
                                const char *text);
void rose_gui_widget_add(struct rose_gui_widget *parent,
                         struct rose_gui_widget *child);
void rose_gui_widget_set_minimum(struct rose_gui_widget *widget, int32_t width,
                                 int32_t height);
void rose_gui_widget_set_flex(struct rose_gui_widget *widget, uint16_t flex);
void rose_gui_text_field_initialize(struct rose_gui_widget *widget,
                                    char *buffer, size_t capacity);
void rose_gui_items_initialize(struct rose_gui_widget *widget,
                               enum rose_gui_widget_type type,
                               const char *const *items, size_t count);
void rose_gui_ui_initialize(struct rose_gui_ui *ui,
                            struct rose_gui_canvas *canvas,
                            const struct rose_gui_theme *theme,
                            struct rose_gui_widget *root);
void rose_gui_ui_layout(struct rose_gui_ui *ui,
                        struct rose_gui_rectangle bounds);
void rose_gui_ui_draw(struct rose_gui_ui *ui);
bool rose_gui_ui_handle_event(struct rose_gui_ui *ui,
                              struct rose_gui_context *context,
                              const struct user_input_event *event);
void rose_gui_ui_focus(struct rose_gui_ui *ui,
                       struct rose_gui_widget *widget);
void rose_gui_ui_invalidate(struct rose_gui_ui *ui);

void rose_gui_unsigned(char *buffer, size_t size, uint64_t value);

#endif
