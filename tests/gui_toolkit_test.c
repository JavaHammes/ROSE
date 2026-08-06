#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "rose/gui.h"
#include "rose/syscall.h"

static const uint8_t test_glyph[ROSE_FONT_WIDTH] = {0x7f, 0x7f, 0x7f, 0x7f,
                                                    0x7f};
static const char theme_resource[] =
    "background=#010203\naccent=#112233\npadding=9\n";
static const char icon_resource[] = "test=0102040810204080\n";
static const char app_resource[] =
    "terminal|TERMINAL|/bin/gui-terminal|test|570|390\n"
    "files|FILES|/bin/gui-files|test|340|480\n";
static const char *active_resource;
static size_t active_resource_offset;

static bool same_text(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }
        return *left == *right;
}

long rose_open(const char *path, uint32_t flags) {
        (void)flags;
        active_resource_offset = 0U;
        if (same_text(path, "/theme"))
                active_resource = theme_resource;
        else if (same_text(path, "/icons"))
                active_resource = icon_resource;
        else if (same_text(path, "/apps"))
                active_resource = app_resource;
        else
                return -1;
        return 3;
}

long rose_read(int descriptor, void *buffer, size_t length) {
        (void)descriptor;
        char *output = buffer;
        size_t count = 0U;
        while (count < length && active_resource[active_resource_offset] !=
                                     '\0') {
                output[count++] = active_resource[active_resource_offset++];
        }
        return (long)count;
}

long rose_close(int descriptor) {
        (void)descriptor;
        return 0;
}

bool rose_font_load(void) {
        return true;
}

const uint8_t *rose_font_glyph(char character) {
        (void)character;
        return test_glyph;
}

char rose_gui_key_character(struct rose_gui_context *context,
                            const struct user_input_event *event) {
        if (event->code == 42U) {
                context->shift = event->value != 0;
                return 0;
        }
        return 0;
}

static int activations;

static void activated(struct rose_gui_widget *widget,
                      enum rose_gui_widget_action action, void *user_data) {
        (void)widget;
        (void)user_data;
        if (action == ROSE_GUI_ACTION_ACTIVATE) activations++;
}

static void test_drawing(void) {
        uint32_t pixels[12 * 8] = {0};
        struct rose_gui_canvas canvas;
        rose_gui_canvas_initialize(&canvas, pixels, 12U, 8U, 12U);
        struct rose_gui_rectangle original = rose_gui_canvas_set_clip(
            &canvas, (struct rose_gui_rectangle){2, 2, 4, 3});
        rose_gui_canvas_fill(&canvas, 0, 0, 12, 8, UINT32_C(0x000000ff));
        assert(pixels[0] == 0U);
        assert(pixels[2U * 12U + 2U] == UINT32_C(0x000000ff));
        assert(pixels[4U * 12U + 5U] == UINT32_C(0x000000ff));
        assert(pixels[5U * 12U + 5U] == 0U);
        rose_gui_canvas_restore_clip(&canvas, original);
        rose_gui_canvas_blend(&canvas, 2, 2, 1, 1,
                              UINT32_C(0x80ff0000));
        assert(pixels[2U * 12U + 2U] == UINT32_C(0x0080007f));

        const uint32_t image_pixels[4] = {
            UINT32_C(0x00112233), UINT32_C(0x00445566),
            UINT32_C(0x00778899), UINT32_C(0x00aabbcc)};
        struct rose_gui_image image = {
            .pixels = image_pixels,
            .width = 2U,
            .height = 2U,
            .pixel_stride = 2U,
            .alpha = false,
        };
        rose_gui_canvas_image(&canvas, 9, 6, &image);
        assert(pixels[6U * 12U + 9U] == UINT32_C(0x00112233));
        assert(pixels[7U * 12U + 10U] == UINT32_C(0x00aabbcc));

        uint32_t rounded_pixels[6 * 6] = {0};
        rose_gui_canvas_initialize(&canvas, rounded_pixels, 6U, 6U, 6U);
        rose_gui_canvas_rounded_rectangle(
            &canvas, (struct rose_gui_rectangle){0, 0, 6, 6}, 2,
            UINT32_C(0x00abcdef));
        assert(rounded_pixels[0] == 0U);
        assert(rounded_pixels[2] == UINT32_C(0x00abcdef));
        assert(rounded_pixels[2U * 6U] == UINT32_C(0x00abcdef));
}

static void test_layout_and_keyboard(void) {
        uint32_t pixels[20 * 10] = {0};
        struct rose_gui_canvas canvas;
        struct rose_gui_theme theme = {0};
        struct rose_gui_widget root;
        struct rose_gui_widget first;
        struct rose_gui_widget second;
        struct rose_gui_ui ui;
        struct rose_gui_context context = {0};

        rose_gui_canvas_initialize(&canvas, pixels, 20U, 10U, 20U);
        rose_gui_widget_initialize(&root, ROSE_GUI_WIDGET_ROW, NULL);
        root.padding = 0U;
        root.gap = 2U;
        rose_gui_widget_initialize(&first, ROSE_GUI_WIDGET_BUTTON, "A");
        rose_gui_widget_initialize(&second, ROSE_GUI_WIDGET_BUTTON, "B");
        rose_gui_widget_set_minimum(&first, 4, 4);
        rose_gui_widget_set_minimum(&second, 6, 4);
        rose_gui_widget_set_flex(&first, 1U);
        rose_gui_widget_set_flex(&second, 1U);
        first.callback = activated;
        rose_gui_widget_add(&root, &first);
        rose_gui_widget_add(&root, &second);
        rose_gui_ui_initialize(&ui, &canvas, &theme, &root);
        rose_gui_ui_layout(&ui, (struct rose_gui_rectangle){0, 0, 20, 10});
        assert(first.bounds.width == 8);
        assert(second.bounds.x == 10);
        assert(second.bounds.width == 10);

        struct user_input_event tab = {
            .type = USER_INPUT_EVENT_KEY, .code = 15U, .value = 1};
        assert(rose_gui_ui_handle_event(&ui, &context, &tab));
        assert(ui.focused == &first);
        assert((first.state & ROSE_GUI_STATE_FOCUSED) != 0U);
        assert(rose_gui_ui_handle_event(&ui, &context, &tab));
        assert(ui.focused == &second);

        struct user_input_event shift = {
            .type = USER_INPUT_EVENT_KEY, .code = 42U, .value = 1};
        (void)rose_gui_ui_handle_event(&ui, &context, &shift);
        assert(rose_gui_ui_handle_event(&ui, &context, &tab));
        assert(ui.focused == &first);

        struct user_input_event enter = {
            .type = USER_INPUT_EVENT_KEY, .code = 28U, .value = 1};
        assert(rose_gui_ui_handle_event(&ui, &context, &enter));
        assert(activations == 1);
}

static void test_escape_dismisses_menu(void) {
        uint32_t pixels[20 * 10] = {0};
        struct rose_gui_canvas canvas;
        struct rose_gui_theme theme = {0};
        struct rose_gui_widget root;
        struct rose_gui_widget menu;
        struct rose_gui_ui ui;
        struct rose_gui_context context = {0};
        const char *const items[] = {"ONE", "TWO"};

        rose_gui_canvas_initialize(&canvas, pixels, 20U, 10U, 20U);
        rose_gui_widget_initialize(&root, ROSE_GUI_WIDGET_ROOT, NULL);
        rose_gui_items_initialize(&menu, ROSE_GUI_WIDGET_MENU, items, 2U);
        rose_gui_widget_add(&root, &menu);
        rose_gui_ui_initialize(&ui, &canvas, &theme, &root);
        rose_gui_ui_layout(&ui, (struct rose_gui_rectangle){0, 0, 20, 10});
        rose_gui_ui_focus(&ui, &menu);
        struct user_input_event escape = {
            .type = USER_INPUT_EVENT_KEY, .code = 1U, .value = 1};
        assert(rose_gui_ui_handle_event(&ui, &context, &escape));
        assert((menu.state & ROSE_GUI_STATE_HIDDEN) != 0U);
}

static void test_hidden_dialog_children_are_laid_out(void) {
        uint32_t pixels[320 * 200] = {0};
        struct rose_gui_canvas canvas;
        struct rose_gui_theme theme = {0};
        struct rose_gui_widget root;
        struct rose_gui_widget dialog;
        struct rose_gui_widget accept;
        struct rose_gui_widget cancel;
        struct rose_gui_ui ui;

        rose_gui_canvas_initialize(&canvas, pixels, 320U, 200U, 320U);
        rose_gui_widget_initialize(&root, ROSE_GUI_WIDGET_ROOT, NULL);
        rose_gui_widget_initialize(&dialog, ROSE_GUI_WIDGET_DIALOG,
                                   "EXIT?");
        dialog.flags |= ROSE_GUI_WIDGET_ABSOLUTE;
        dialog.bounds = (struct rose_gui_rectangle){40, 30, 240, 140};
        dialog.state |= ROSE_GUI_STATE_HIDDEN;
        dialog.padding = 32U;
        rose_gui_widget_initialize(&accept, ROSE_GUI_WIDGET_BUTTON, "EXIT");
        rose_gui_widget_initialize(&cancel, ROSE_GUI_WIDGET_BUTTON, "CANCEL");
        rose_gui_widget_set_flex(&accept, 1U);
        rose_gui_widget_set_flex(&cancel, 1U);
        rose_gui_widget_add(&dialog, &accept);
        rose_gui_widget_add(&dialog, &cancel);
        rose_gui_widget_add(&root, &dialog);
        rose_gui_ui_initialize(&ui, &canvas, &theme, &root);
        rose_gui_ui_layout(&ui,
                           (struct rose_gui_rectangle){0, 0, 320, 200});

        assert(accept.bounds.width > 0);
        assert(accept.bounds.height > 0);
        assert(cancel.bounds.width > 0);
        assert(cancel.bounds.height > 0);
        assert(accept.bounds.x >= dialog.bounds.x);
        assert(cancel.bounds.y > accept.bounds.y);
}

static void test_file_resources(void) {
        struct rose_gui_theme theme;
        assert(rose_gui_theme_load(&theme, "/theme"));
        assert(theme.background == UINT32_C(0x00010203));
        assert(theme.accent == UINT32_C(0x00112233));
        assert(theme.padding == 9U);

        struct rose_gui_icon icon;
        assert(rose_gui_icon_load(&icon, "test", "/icons"));
        assert(icon.rows[0] == 1U);
        assert(icon.rows[7] == 128U);

        struct rose_gui_app_catalog catalog;
        assert(rose_gui_app_catalog_load(&catalog, "/apps"));
        assert(catalog.count == 2U);
        const struct rose_gui_app_metadata *files =
            rose_gui_app_find(&catalog, "files");
        assert(files != NULL);
        assert(files->width == 340U);
        assert(files->height == 480U);
}

int main(void) {
        test_drawing();
        test_layout_and_keyboard();
        test_escape_dismisses_menu();
        test_hidden_dialog_children_are_laid_out();
        test_file_resources();
        return 0;
}
