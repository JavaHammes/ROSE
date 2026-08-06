#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "user_abi.h"

enum {
        KEY_ESCAPE = 1,
        KEY_BACKSPACE = 14,
        KEY_TAB = 15,
        KEY_ENTER = 28,
        KEY_SPACE = 57,
        KEY_HOME = 102,
        KEY_UP = 103,
        KEY_LEFT = 105,
        KEY_RIGHT = 106,
        KEY_END = 107,
        KEY_DOWN = 108,
};

static size_t text_length(const char *text) {
        size_t length = 0U;
        while (text != NULL && text[length] != '\0') length++;
        return length;
}

static void bytes_zero(void *destination, size_t size) {
        uint8_t *bytes = destination;
        for (size_t index = 0U; index < size; index++) bytes[index] = 0U;
}

static bool is_container(enum rose_gui_widget_type type) {
        return type == ROSE_GUI_WIDGET_ROOT || type == ROSE_GUI_WIDGET_ROW ||
               type == ROSE_GUI_WIDGET_COLUMN ||
               type == ROSE_GUI_WIDGET_MENU ||
               type == ROSE_GUI_WIDGET_DIALOG;
}

static bool focusable_type(enum rose_gui_widget_type type) {
        return type == ROSE_GUI_WIDGET_BUTTON ||
               type == ROSE_GUI_WIDGET_ICON_BUTTON ||
               type == ROSE_GUI_WIDGET_TEXT_FIELD ||
               type == ROSE_GUI_WIDGET_CHECKBOX ||
               type == ROSE_GUI_WIDGET_LIST ||
               type == ROSE_GUI_WIDGET_SCROLLBAR ||
               type == ROSE_GUI_WIDGET_MENU ||
               type == ROSE_GUI_WIDGET_TABS;
}

void rose_gui_widget_initialize(struct rose_gui_widget *widget,
                                enum rose_gui_widget_type type,
                                const char *text) {
        if (widget == NULL) return;
        bytes_zero(widget, sizeof(*widget));
        widget->type = type;
        widget->text = text;
        widget->padding = 6U;
        widget->gap = 6U;
        widget->flex = 1U;
        if (focusable_type(type)) widget->flags |= ROSE_GUI_WIDGET_FOCUSABLE;
        switch (type) {
        case ROSE_GUI_WIDGET_LABEL:
                widget->minimum_width = rose_gui_text_width(text, 1U);
                widget->minimum_height = ROSE_GUI_FONT_HEIGHT;
                break;
        case ROSE_GUI_WIDGET_BUTTON:
                widget->minimum_width = rose_gui_text_width(text, 1U) + 24;
                widget->minimum_height = 28;
                break;
        case ROSE_GUI_WIDGET_ICON_BUTTON:
                widget->minimum_width = 28;
                widget->minimum_height = 28;
                break;
        case ROSE_GUI_WIDGET_TEXT_FIELD:
                widget->minimum_width = 96;
                widget->minimum_height = 28;
                break;
        case ROSE_GUI_WIDGET_CHECKBOX:
                widget->minimum_width = 22 + rose_gui_text_width(text, 1U);
                widget->minimum_height = 22;
                break;
        case ROSE_GUI_WIDGET_LIST:
                widget->minimum_width = 120;
                widget->minimum_height = 72;
                break;
        case ROSE_GUI_WIDGET_SCROLLBAR:
                widget->minimum_width = 14;
                widget->minimum_height = 48;
                widget->maximum = 100;
                widget->page = 10;
                break;
        case ROSE_GUI_WIDGET_MENU:
                widget->minimum_width = 140;
                widget->minimum_height = 32;
                widget->flags |= ROSE_GUI_WIDGET_SURFACE;
                break;
        case ROSE_GUI_WIDGET_DIALOG:
                widget->minimum_width = 240;
                widget->minimum_height = 120;
                widget->flags |= ROSE_GUI_WIDGET_SURFACE;
                break;
        case ROSE_GUI_WIDGET_TABS:
                widget->minimum_width = 120;
                widget->minimum_height = 30;
                break;
        case ROSE_GUI_WIDGET_STATUS_BAR:
                widget->minimum_height = 24;
                break;
        default: break;
        }
}

void rose_gui_widget_add(struct rose_gui_widget *parent,
                         struct rose_gui_widget *child) {
        if (parent == NULL || child == NULL || child->parent != NULL) return;
        child->parent = parent;
        if (parent->last_child == NULL) {
                parent->first_child = child;
        } else {
                parent->last_child->next_sibling = child;
        }
        parent->last_child = child;
}

void rose_gui_widget_set_minimum(struct rose_gui_widget *widget, int32_t width,
                                 int32_t height) {
        if (widget == NULL) return;
        widget->minimum_width = width > 0 ? width : 0;
        widget->minimum_height = height > 0 ? height : 0;
}

void rose_gui_widget_set_flex(struct rose_gui_widget *widget, uint16_t flex) {
        if (widget == NULL) return;
        widget->flex = flex;
        if (flex == 0U)
                widget->flags &= ~ROSE_GUI_WIDGET_FLEX;
        else
                widget->flags |= ROSE_GUI_WIDGET_FLEX;
}

void rose_gui_text_field_initialize(struct rose_gui_widget *widget,
                                    char *buffer, size_t capacity) {
        rose_gui_widget_initialize(widget, ROSE_GUI_WIDGET_TEXT_FIELD, NULL);
        if (widget == NULL) return;
        widget->text_buffer = buffer;
        widget->text_capacity = capacity;
        if (buffer != NULL) {
                widget->text_length = text_length(buffer);
                if (widget->text_length >= capacity && capacity != 0U) {
                        widget->text_length = capacity - 1U;
                        buffer[widget->text_length] = '\0';
                }
        }
}

void rose_gui_items_initialize(struct rose_gui_widget *widget,
                               enum rose_gui_widget_type type,
                               const char *const *items, size_t count) {
        rose_gui_widget_initialize(widget, type, NULL);
        if (widget == NULL) return;
        widget->items = items;
        widget->item_count = count;
        widget->selected_index = 0U;
}

static int32_t child_minimum(const struct rose_gui_widget *child,
                             bool horizontal) {
        return horizontal ? child->minimum_width : child->minimum_height;
}

static void layout_widget(struct rose_gui_widget *widget,
                          struct rose_gui_rectangle bounds) {
        if (widget == NULL) return;
        widget->bounds = bounds;
        if (!is_container(widget->type)) return;

        bool horizontal = widget->type == ROSE_GUI_WIDGET_ROW;
        int32_t padding = (int32_t)widget->padding;
        int32_t gap = (int32_t)widget->gap;
        int32_t main_size = (horizontal ? bounds.width : bounds.height) -
                            padding * 2;
        int32_t cross_size = (horizontal ? bounds.height : bounds.width) -
                             padding * 2;
        if (main_size < 0) main_size = 0;
        if (cross_size < 0) cross_size = 0;

        int32_t minimum_sum = 0;
        uint32_t flex_sum = 0U;
        size_t count = 0U;
        for (struct rose_gui_widget *child = widget->first_child;
             child != NULL; child = child->next_sibling) {
                if ((child->state & ROSE_GUI_STATE_HIDDEN) != 0U ||
                    (child->flags & ROSE_GUI_WIDGET_ABSOLUTE) != 0U) {
                        continue;
                }
                minimum_sum += child_minimum(child, horizontal);
                if ((child->flags & ROSE_GUI_WIDGET_FLEX) != 0U)
                        flex_sum += child->flex;
                count++;
        }
        if (count > 1U) minimum_sum += (int32_t)(count - 1U) * gap;
        int32_t extra = main_size > minimum_sum ? main_size - minimum_sum : 0;
        int32_t cursor = (horizontal ? bounds.x : bounds.y) + padding;
        uint32_t flex_left = flex_sum;
        int32_t extra_left = extra;

        for (struct rose_gui_widget *child = widget->first_child;
             child != NULL; child = child->next_sibling) {
                if ((child->state & ROSE_GUI_STATE_HIDDEN) != 0U) {
                        /* Absolute overlays keep explicit bounds while hidden.
                         * Lay out their descendants now so showing a menu or
                         * dialog never exposes zero-sized controls. */
                        if ((child->flags & ROSE_GUI_WIDGET_ABSOLUTE) != 0U)
                                layout_widget(child, child->bounds);
                        continue;
                }
                if ((child->flags & ROSE_GUI_WIDGET_ABSOLUTE) != 0U) {
                        layout_widget(child, child->bounds);
                        continue;
                }
                int32_t main = child_minimum(child, horizontal);
                if ((child->flags & ROSE_GUI_WIDGET_FLEX) != 0U &&
                    flex_left != 0U) {
                        int32_t share =
                            (int32_t)((int64_t)extra_left * child->flex /
                                      (int64_t)flex_left);
                        main += share;
                        extra_left -= share;
                        flex_left -= child->flex;
                }
                struct rose_gui_rectangle child_bounds;
                if (horizontal) {
                        child_bounds = (struct rose_gui_rectangle){
                            cursor, bounds.y + padding, main, cross_size};
                } else {
                        child_bounds = (struct rose_gui_rectangle){
                            bounds.x + padding, cursor, cross_size, main};
                }
                layout_widget(child, child_bounds);
                cursor += main + gap;
        }
}

void rose_gui_ui_initialize(struct rose_gui_ui *ui,
                            struct rose_gui_canvas *canvas,
                            const struct rose_gui_theme *theme,
                            struct rose_gui_widget *root) {
        if (ui == NULL) return;
        bytes_zero(ui, sizeof(*ui));
        ui->canvas = canvas;
        ui->theme = theme;
        ui->root = root;
        ui->dirty = true;
}

void rose_gui_ui_layout(struct rose_gui_ui *ui,
                        struct rose_gui_rectangle bounds) {
        if (ui == NULL || ui->root == NULL) return;
        layout_widget(ui->root, bounds);
        ui->dirty = true;
}

static bool contains(const struct rose_gui_rectangle *bounds, int32_t x,
                     int32_t y) {
        return x >= bounds->x && y >= bounds->y &&
               x < bounds->x + bounds->width &&
               y < bounds->y + bounds->height;
}

static uint32_t control_color(const struct rose_gui_widget *widget,
                              const struct rose_gui_theme *theme) {
        if ((widget->state & ROSE_GUI_STATE_DISABLED) != 0U)
                return theme->disabled;
        if ((widget->state & ROSE_GUI_STATE_ERROR) != 0U) return theme->error;
        if ((widget->state & ROSE_GUI_STATE_PRESSED) != 0U)
                return theme->accent_pressed;
        if ((widget->state & ROSE_GUI_STATE_HOVERED) != 0U)
                return theme->accent_hover;
        if ((widget->state & ROSE_GUI_STATE_SELECTED) != 0U)
                return theme->selected;
        return theme->accent;
}

static int32_t centered_text_x(const struct rose_gui_rectangle *bounds,
                               const char *text) {
        return bounds->x +
               (bounds->width - rose_gui_text_width(text, 1U)) / 2;
}

static void draw_focus(struct rose_gui_canvas *canvas,
                       const struct rose_gui_widget *widget,
                       const struct rose_gui_theme *theme) {
        if ((widget->state & ROSE_GUI_STATE_FOCUSED) == 0U) return;
        struct rose_gui_rectangle ring = widget->bounds;
        rose_gui_canvas_border(canvas, ring, 2, theme->focus);
}

static void draw_items(struct rose_gui_canvas *canvas,
                       const struct rose_gui_widget *widget,
                       const struct rose_gui_theme *theme, bool menu) {
        int32_t row_height = 22;
        int32_t top = widget->bounds.y + (menu ? 5 : 3);
        for (size_t index = 0U; index < widget->item_count; index++) {
                int32_t y = top + (int32_t)index * row_height;
                if (y + row_height > widget->bounds.y + widget->bounds.height)
                        break;
                if (index == widget->selected_index) {
                        rose_gui_canvas_rounded_rectangle(
                            canvas,
                            (struct rose_gui_rectangle){
                                widget->bounds.x + 3, y,
                                widget->bounds.width - 6, row_height},
                            theme->corner_radius, theme->selected);
                }
                rose_gui_canvas_text(canvas, widget->bounds.x + 10, y + 7,
                                     widget->items[index], theme->text, 1U);
        }
}

static void draw_widget(struct rose_gui_canvas *canvas,
                        struct rose_gui_widget *widget,
                        const struct rose_gui_theme *theme) {
        if (widget == NULL || (widget->state & ROSE_GUI_STATE_HIDDEN) != 0U)
                return;
        struct rose_gui_rectangle previous =
            rose_gui_canvas_set_clip(canvas, widget->bounds);
        int32_t text_y = widget->bounds.y +
                         (widget->bounds.height - ROSE_GUI_FONT_HEIGHT) / 2;
        uint32_t text_color =
            (widget->state & ROSE_GUI_STATE_DISABLED) != 0U ? theme->muted
                                                            : theme->text;
        switch (widget->type) {
        case ROSE_GUI_WIDGET_ROOT:
                if ((widget->flags & ROSE_GUI_WIDGET_TRANSPARENT) == 0U) {
                        rose_gui_canvas_fill(canvas, widget->bounds.x,
                                             widget->bounds.y,
                                             widget->bounds.width,
                                             widget->bounds.height,
                                             theme->background);
                }
                break;
        case ROSE_GUI_WIDGET_ROW:
        case ROSE_GUI_WIDGET_COLUMN:
                if ((widget->flags & ROSE_GUI_WIDGET_SURFACE) != 0U) {
                        rose_gui_canvas_fill(canvas, widget->bounds.x,
                                             widget->bounds.y,
                                             widget->bounds.width,
                                             widget->bounds.height,
                                             theme->surface);
                }
                break;
        case ROSE_GUI_WIDGET_LABEL:
                rose_gui_canvas_text(canvas, widget->bounds.x, text_y,
                                     widget->text, text_color, 1U);
                break;
        case ROSE_GUI_WIDGET_BUTTON:
        case ROSE_GUI_WIDGET_ICON_BUTTON: {
                uint32_t background = control_color(widget, theme);
                rose_gui_canvas_rounded_rectangle(canvas, widget->bounds,
                                                  theme->corner_radius,
                                                  background);
                if (widget->icon != NULL) {
                        int32_t icon_x = widget->bounds.x +
                                         (widget->bounds.width -
                                          ROSE_GUI_ICON_SIZE) /
                                             2;
                        rose_gui_canvas_icon(canvas, icon_x,
                                             widget->bounds.y +
                                                 (widget->bounds.height -
                                                  ROSE_GUI_ICON_SIZE) /
                                                     2,
                                             widget->icon, theme->on_accent,
                                             1U);
                } else {
                        rose_gui_canvas_text(
                            canvas, centered_text_x(&widget->bounds,
                                                    widget->text),
                            text_y, widget->text, theme->on_accent, 1U);
                }
                draw_focus(canvas, widget, theme);
                break;
        }
        case ROSE_GUI_WIDGET_TEXT_FIELD: {
                rose_gui_canvas_rounded_rectangle(canvas, widget->bounds,
                                                  theme->corner_radius,
                                                  theme->surface_alternate);
                rose_gui_canvas_border(
                    canvas, widget->bounds, 1,
                    (widget->state & ROSE_GUI_STATE_ERROR) != 0U
                        ? theme->error
                        : theme->border);
                rose_gui_canvas_text(canvas, widget->bounds.x + 8, text_y,
                                     widget->text_buffer, text_color, 1U);
                if ((widget->state & ROSE_GUI_STATE_FOCUSED) != 0U) {
                        int32_t cursor = widget->bounds.x + 8 +
                                         rose_gui_text_width(
                                             widget->text_buffer, 1U);
                        rose_gui_canvas_line(canvas, cursor,
                                             widget->bounds.y + 6, cursor,
                                             widget->bounds.y +
                                                 widget->bounds.height - 7,
                                             theme->focus);
                }
                draw_focus(canvas, widget, theme);
                break;
        }
        case ROSE_GUI_WIDGET_CHECKBOX: {
                struct rose_gui_rectangle box = {
                    widget->bounds.x, widget->bounds.y +
                                          (widget->bounds.height - 16) / 2,
                    16, 16};
                rose_gui_canvas_rounded_rectangle(
                    canvas, box, 3,
                    widget->value != 0 ? control_color(widget, theme)
                                       : theme->surface_alternate);
                rose_gui_canvas_border(canvas, box, 1, theme->border);
                if (widget->value != 0) {
                        rose_gui_canvas_line(canvas, box.x + 3, box.y + 8,
                                             box.x + 7, box.y + 12,
                                             theme->on_accent);
                        rose_gui_canvas_line(canvas, box.x + 7, box.y + 12,
                                             box.x + 13, box.y + 3,
                                             theme->on_accent);
                }
                rose_gui_canvas_text(canvas, widget->bounds.x + 22, text_y,
                                     widget->text, text_color, 1U);
                draw_focus(canvas, widget, theme);
                break;
        }
        case ROSE_GUI_WIDGET_LIST:
                rose_gui_canvas_fill(canvas, widget->bounds.x,
                                     widget->bounds.y, widget->bounds.width,
                                     widget->bounds.height, theme->surface);
                rose_gui_canvas_border(canvas, widget->bounds, 1,
                                       theme->border);
                draw_items(canvas, widget, theme, false);
                draw_focus(canvas, widget, theme);
                break;
        case ROSE_GUI_WIDGET_SCROLLBAR: {
                rose_gui_canvas_rounded_rectangle(canvas, widget->bounds,
                                                  theme->corner_radius,
                                                  theme->surface_alternate);
                int32_t track = widget->bounds.height - 4;
                int32_t maximum = widget->maximum > 0 ? widget->maximum : 1;
                int32_t thumb = widget->page > 0
                                    ? track * widget->page /
                                          (maximum + widget->page)
                                    : 8;
                if (thumb < 8) thumb = 8;
                if (thumb > track) thumb = track;
                int32_t range = track - thumb;
                int32_t offset = range * widget->value / maximum;
                rose_gui_canvas_rounded_rectangle(
                    canvas,
                    (struct rose_gui_rectangle){widget->bounds.x + 2,
                                                widget->bounds.y + 2 + offset,
                                                widget->bounds.width - 4,
                                                thumb},
                    theme->corner_radius, control_color(widget, theme));
                draw_focus(canvas, widget, theme);
                break;
        }
        case ROSE_GUI_WIDGET_MENU:
                rose_gui_canvas_blend(canvas, widget->bounds.x + 5,
                                      widget->bounds.y + 5,
                                      widget->bounds.width,
                                      widget->bounds.height,
                                      UINT32_C(0x80000000));
                rose_gui_canvas_rounded_rectangle(canvas, widget->bounds,
                                                  theme->corner_radius,
                                                  theme->surface);
                rose_gui_canvas_border(canvas, widget->bounds, 1,
                                       theme->border);
                draw_items(canvas, widget, theme, true);
                draw_focus(canvas, widget, theme);
                break;
        case ROSE_GUI_WIDGET_DIALOG:
                rose_gui_canvas_blend(canvas, widget->bounds.x + 7,
                                      widget->bounds.y + 8,
                                      widget->bounds.width,
                                      widget->bounds.height,
                                      UINT32_C(0x99000000));
                rose_gui_canvas_rounded_rectangle(canvas, widget->bounds,
                                                  theme->corner_radius + 2,
                                                  theme->surface);
                rose_gui_canvas_border(canvas, widget->bounds, 1,
                                       theme->border);
                if (widget->text != NULL) {
                        rose_gui_canvas_text(canvas, widget->bounds.x + 14,
                                             widget->bounds.y + 14,
                                             widget->text, theme->text, 1U);
                }
                break;
        case ROSE_GUI_WIDGET_TABS: {
                int32_t tab_width = widget->item_count == 0U
                                        ? widget->bounds.width
                                        : widget->bounds.width /
                                              (int32_t)widget->item_count;
                for (size_t index = 0U; index < widget->item_count; index++) {
                        struct rose_gui_rectangle tab = {
                            widget->bounds.x + (int32_t)index * tab_width,
                            widget->bounds.y, tab_width,
                            widget->bounds.height};
                        rose_gui_canvas_fill(
                            canvas, tab.x, tab.y, tab.width, tab.height,
                            index == widget->selected_index ? theme->selected
                                                            : theme->surface);
                        rose_gui_canvas_text(
                            canvas, centered_text_x(&tab, widget->items[index]),
                            text_y, widget->items[index], text_color, 1U);
                }
                rose_gui_canvas_fill(canvas, widget->bounds.x,
                                     widget->bounds.y +
                                         widget->bounds.height - 2,
                                     widget->bounds.width, 2, theme->accent);
                draw_focus(canvas, widget, theme);
                break;
        }
        case ROSE_GUI_WIDGET_STATUS_BAR:
                rose_gui_canvas_fill(canvas, widget->bounds.x,
                                     widget->bounds.y, widget->bounds.width,
                                     widget->bounds.height,
                                     theme->surface_alternate);
                rose_gui_canvas_text(canvas, widget->bounds.x + 8, text_y,
                                     widget->text, theme->muted, 1U);
                break;
        case ROSE_GUI_WIDGET_CUSTOM:
                if (widget->custom_draw != NULL) {
                        widget->custom_draw(widget, canvas, theme,
                                            widget->user_data);
                }
                break;
        }
        for (struct rose_gui_widget *child = widget->first_child;
             child != NULL; child = child->next_sibling) {
                draw_widget(canvas, child, theme);
        }
        rose_gui_canvas_restore_clip(canvas, previous);
}

void rose_gui_ui_draw(struct rose_gui_ui *ui) {
        if (ui == NULL || ui->canvas == NULL || ui->theme == NULL ||
            ui->root == NULL) {
                return;
        }
        draw_widget(ui->canvas, ui->root, ui->theme);
        ui->dirty = false;
}

static bool enabled_focusable(const struct rose_gui_widget *widget) {
        if (widget == NULL ||
            (widget->flags & ROSE_GUI_WIDGET_FOCUSABLE) == 0U) {
                return false;
        }
        for (const struct rose_gui_widget *item = widget; item != NULL;
             item = item->parent) {
                if ((item->state &
                     (ROSE_GUI_STATE_DISABLED | ROSE_GUI_STATE_HIDDEN)) !=
                    0U) {
                        return false;
                }
        }
        return true;
}

static struct rose_gui_widget *next_node(struct rose_gui_widget *node);

static struct rose_gui_widget *hit_test(struct rose_gui_widget *widget,
                                        int32_t x, int32_t y) {
        if (widget == NULL ||
            (widget->state &
             (ROSE_GUI_STATE_DISABLED | ROSE_GUI_STATE_HIDDEN)) != 0U ||
            !contains(&widget->bounds, x, y)) {
                return NULL;
        }
        struct rose_gui_widget *result = NULL;
        for (struct rose_gui_widget *child = widget->first_child;
             child != NULL; child = child->next_sibling) {
                struct rose_gui_widget *candidate = hit_test(child, x, y);
                if (candidate != NULL) result = candidate;
        }
        if (result != NULL) return result;
        return enabled_focusable(widget) ? widget : NULL;
}

static struct rose_gui_widget *visible_overlay(struct rose_gui_widget *root) {
        struct rose_gui_widget *overlay = NULL;
        for (struct rose_gui_widget *item = root; item != NULL;
             item = next_node(item)) {
                if ((item->type == ROSE_GUI_WIDGET_MENU ||
                     item->type == ROSE_GUI_WIDGET_DIALOG) &&
                    (item->state & ROSE_GUI_STATE_HIDDEN) == 0U) {
                        overlay = item;
                }
        }
        return overlay;
}

void rose_gui_ui_focus(struct rose_gui_ui *ui,
                       struct rose_gui_widget *widget) {
        if (ui == NULL || (widget != NULL && !enabled_focusable(widget)))
                return;
        if (ui->focused == widget) return;
        if (ui->focused != NULL)
                ui->focused->state &= ~ROSE_GUI_STATE_FOCUSED;
        ui->focused = widget;
        if (widget != NULL) widget->state |= ROSE_GUI_STATE_FOCUSED;
        ui->dirty = true;
}

static struct rose_gui_widget *next_node(struct rose_gui_widget *node) {
        if (node == NULL) return NULL;
        if (node->first_child != NULL) return node->first_child;
        while (node != NULL) {
                if (node->next_sibling != NULL) return node->next_sibling;
                node = node->parent;
        }
        return NULL;
}

static struct rose_gui_widget *first_focusable(struct rose_gui_widget *root) {
        for (struct rose_gui_widget *item = root; item != NULL;
             item = next_node(item)) {
                if (enabled_focusable(item)) return item;
        }
        return NULL;
}

static void focus_next(struct rose_gui_ui *ui, bool reverse) {
        if (ui->root == NULL) return;
        if (ui->focused == NULL) {
                rose_gui_ui_focus(ui, first_focusable(ui->root));
                return;
        }
        if (!reverse) {
                struct rose_gui_widget *item = next_node(ui->focused);
                while (item != NULL && !enabled_focusable(item))
                        item = next_node(item);
                if (item == NULL) item = first_focusable(ui->root);
                rose_gui_ui_focus(ui, item);
                return;
        }
        struct rose_gui_widget *previous = NULL;
        struct rose_gui_widget *last = NULL;
        for (struct rose_gui_widget *item = ui->root; item != NULL;
             item = next_node(item)) {
                if (!enabled_focusable(item)) continue;
                if (item == ui->focused) {
                        previous = last;
                        break;
                }
                last = item;
        }
        if (previous == NULL) {
                for (struct rose_gui_widget *item = ui->root; item != NULL;
                     item = next_node(item)) {
                        if (enabled_focusable(item)) previous = item;
                }
        }
        rose_gui_ui_focus(ui, previous);
}

static void notify(struct rose_gui_widget *widget,
                   enum rose_gui_widget_action action) {
        if (widget->callback != NULL)
                widget->callback(widget, action, widget->user_data);
}

static void activate(struct rose_gui_widget *widget) {
        if (widget == NULL) return;
        if (widget->type == ROSE_GUI_WIDGET_CHECKBOX) {
                widget->value = widget->value == 0 ? 1 : 0;
                notify(widget, ROSE_GUI_ACTION_CHANGE);
        } else if (widget->type == ROSE_GUI_WIDGET_LIST ||
                   widget->type == ROSE_GUI_WIDGET_MENU ||
                   widget->type == ROSE_GUI_WIDGET_TABS) {
                notify(widget, ROSE_GUI_ACTION_SELECT);
        } else {
                notify(widget, ROSE_GUI_ACTION_ACTIVATE);
        }
}

static void select_at_pointer(struct rose_gui_widget *widget, int32_t x,
                              int32_t y) {
        if (widget->item_count == 0U) return;
        size_t selected = widget->selected_index;
        if (widget->type == ROSE_GUI_WIDGET_TABS) {
                int32_t relative = x - widget->bounds.x;
                if (relative >= 0 && widget->bounds.width > 0) {
                        selected = (size_t)((int64_t)relative *
                                            (int64_t)widget->item_count /
                                            widget->bounds.width);
                }
        } else if (widget->type == ROSE_GUI_WIDGET_LIST ||
                   widget->type == ROSE_GUI_WIDGET_MENU) {
                int32_t relative = y - widget->bounds.y -
                                   (widget->type == ROSE_GUI_WIDGET_MENU ? 5
                                                                         : 3);
                if (relative >= 0) selected = (size_t)(relative / 22);
        }
        if (selected >= widget->item_count) selected = widget->item_count - 1U;
        if (selected != widget->selected_index) {
                widget->selected_index = selected;
                notify(widget, ROSE_GUI_ACTION_CHANGE);
        }
}

static void scrollbar_at_pointer(struct rose_gui_widget *widget, int32_t y) {
        int32_t relative = y - widget->bounds.y;
        int32_t range = widget->bounds.height > 1 ? widget->bounds.height - 1
                                                  : 1;
        int32_t value = relative * widget->maximum / range;
        if (value < 0) value = 0;
        if (value > widget->maximum) value = widget->maximum;
        if (value != widget->value) {
                widget->value = value;
                notify(widget, ROSE_GUI_ACTION_CHANGE);
        }
}

static bool pointer_event(struct rose_gui_ui *ui,
                          const struct user_input_event *event) {
        bool was_pressed =
            (ui->pointer_buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        bool now_pressed =
            (event->buttons & USER_POINTER_BUTTON_LEFT) != 0U;
        bool captured = ui->pressed != NULL;
        struct rose_gui_widget *overlay = visible_overlay(ui->root);
        struct rose_gui_widget *target = hit_test(
            overlay != NULL ? overlay : ui->root, event->x, event->y);
        if (target != ui->hovered) {
                if (ui->hovered != NULL)
                        ui->hovered->state &= ~ROSE_GUI_STATE_HOVERED;
                ui->hovered = target;
                if (target != NULL) target->state |= ROSE_GUI_STATE_HOVERED;
                ui->dirty = true;
        }
        if (!was_pressed && now_pressed && target != NULL) {
                rose_gui_ui_focus(ui, target);
                ui->pressed = target;
                target->state |= ROSE_GUI_STATE_PRESSED;
                if (target->type == ROSE_GUI_WIDGET_LIST ||
                    target->type == ROSE_GUI_WIDGET_MENU ||
                    target->type == ROSE_GUI_WIDGET_TABS) {
                        select_at_pointer(target, event->x, event->y);
                } else if (target->type == ROSE_GUI_WIDGET_SCROLLBAR) {
                        scrollbar_at_pointer(target, event->y);
                }
                ui->dirty = true;
        } else if (!was_pressed && now_pressed && overlay != NULL &&
                   overlay->type == ROSE_GUI_WIDGET_MENU) {
                overlay->state |= ROSE_GUI_STATE_HIDDEN;
                notify(overlay, ROSE_GUI_ACTION_DISMISS);
                rose_gui_ui_focus(ui, first_focusable(ui->root));
                ui->dirty = true;
        } else if (was_pressed && now_pressed && ui->pressed != NULL &&
                   ui->pressed->type == ROSE_GUI_WIDGET_SCROLLBAR) {
                scrollbar_at_pointer(ui->pressed, event->y);
                ui->dirty = true;
        } else if (was_pressed && !now_pressed && ui->pressed != NULL) {
                struct rose_gui_widget *pressed = ui->pressed;
                pressed->state &= ~ROSE_GUI_STATE_PRESSED;
                ui->pressed = NULL;
                if (pressed == target) activate(pressed);
                ui->dirty = true;
        }
        ui->pointer_buttons = event->buttons;
        return overlay != NULL || target != NULL || captured ||
               ui->pressed != NULL;
}

static bool change_selection(struct rose_gui_widget *widget, int direction) {
        if (widget->item_count == 0U) return false;
        size_t previous = widget->selected_index;
        if (direction < 0 && widget->selected_index != 0U)
                widget->selected_index--;
        if (direction > 0 && widget->selected_index + 1U < widget->item_count)
                widget->selected_index++;
        if (previous != widget->selected_index) {
                notify(widget, ROSE_GUI_ACTION_CHANGE);
                return true;
        }
        return false;
}

static bool dismiss_overlay(struct rose_gui_ui *ui) {
        struct rose_gui_widget *widget = ui->focused;
        while (widget != NULL && widget->type != ROSE_GUI_WIDGET_MENU &&
               widget->type != ROSE_GUI_WIDGET_DIALOG) {
                widget = widget->parent;
        }
        if (widget == NULL) {
                ui->dismissed = true;
                return false;
        }
        widget->state |= ROSE_GUI_STATE_HIDDEN;
        notify(widget, ROSE_GUI_ACTION_DISMISS);
        rose_gui_ui_focus(ui, first_focusable(ui->root));
        ui->dirty = true;
        return true;
}

static bool key_event(struct rose_gui_ui *ui,
                      struct rose_gui_context *context,
                      const struct user_input_event *event) {
        char character = rose_gui_key_character(context, event);
        if (event->value == 0) return false;
        if (event->code == KEY_TAB) {
                focus_next(ui, context->shift);
                return true;
        }
        if (event->code == KEY_ESCAPE) return dismiss_overlay(ui);
        struct rose_gui_widget *focused = ui->focused;
        if (focused == NULL) return false;
        if (event->code == KEY_ENTER || event->code == KEY_SPACE) {
                if (focused->type != ROSE_GUI_WIDGET_TEXT_FIELD ||
                    event->code == KEY_ENTER) {
                        activate(focused);
                        ui->dirty = true;
                        return true;
                }
        }
        if (focused->type == ROSE_GUI_WIDGET_TEXT_FIELD &&
            focused->text_buffer != NULL) {
                if (event->code == KEY_BACKSPACE) {
                        if (focused->text_length != 0U) {
                                focused->text_length--;
                                focused->text_buffer[focused->text_length] =
                                    '\0';
                                notify(focused, ROSE_GUI_ACTION_CHANGE);
                                ui->dirty = true;
                        }
                        return true;
                }
                if (character >= 32 && character <= 126 &&
                    focused->text_length + 1U < focused->text_capacity) {
                        focused->text_buffer[focused->text_length++] =
                            character;
                        focused->text_buffer[focused->text_length] = '\0';
                        focused->state &= ~ROSE_GUI_STATE_ERROR;
                        notify(focused, ROSE_GUI_ACTION_CHANGE);
                        ui->dirty = true;
                        return true;
                }
        }
        if (focused->type == ROSE_GUI_WIDGET_LIST ||
            focused->type == ROSE_GUI_WIDGET_MENU ||
            focused->type == ROSE_GUI_WIDGET_TABS) {
                if (event->code == KEY_UP || event->code == KEY_LEFT ||
                    event->code == KEY_HOME) {
                        if (event->code == KEY_HOME &&
                            focused->selected_index != 0U) {
                                focused->selected_index = 0U;
                                notify(focused, ROSE_GUI_ACTION_CHANGE);
                        } else {
                                (void)change_selection(focused, -1);
                        }
                        ui->dirty = true;
                        return true;
                }
                if (event->code == KEY_DOWN || event->code == KEY_RIGHT ||
                    event->code == KEY_END) {
                        if (event->code == KEY_END &&
                            focused->item_count != 0U &&
                            focused->selected_index !=
                                focused->item_count - 1U) {
                                focused->selected_index =
                                    focused->item_count - 1U;
                                notify(focused, ROSE_GUI_ACTION_CHANGE);
                        } else {
                                (void)change_selection(focused, 1);
                        }
                        ui->dirty = true;
                        return true;
                }
        }
        if (focused->type == ROSE_GUI_WIDGET_SCROLLBAR &&
            (event->code == KEY_UP || event->code == KEY_LEFT ||
             event->code == KEY_DOWN || event->code == KEY_RIGHT)) {
                int32_t change =
                    event->code == KEY_UP || event->code == KEY_LEFT ? -1 : 1;
                int32_t value = focused->value + change;
                if (value < 0) value = 0;
                if (value > focused->maximum) value = focused->maximum;
                if (value != focused->value) {
                        focused->value = value;
                        notify(focused, ROSE_GUI_ACTION_CHANGE);
                        ui->dirty = true;
                }
                return true;
        }
        return false;
}

bool rose_gui_ui_handle_event(struct rose_gui_ui *ui,
                              struct rose_gui_context *context,
                              const struct user_input_event *event) {
        if (ui == NULL || context == NULL || event == NULL) return false;
        if (event->type == USER_INPUT_EVENT_POINTER)
                return pointer_event(ui, event);
        if (event->type == USER_INPUT_EVENT_KEY)
                return key_event(ui, context, event);
        return false;
}

void rose_gui_ui_invalidate(struct rose_gui_ui *ui) {
        if (ui != NULL) ui->dirty = true;
}
