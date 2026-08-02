#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "rose/gui.h"
#include "rose/syscall.h"

_Static_assert(sizeof(struct rose_gui_surface) <=
                   ROSE_GUI_SURFACE_PIXEL_OFFSET,
               "GUI control page exceeds its reserved page");

static void bytes_zero(void *destination, size_t size) {
        uint8_t *bytes = destination;
        for (size_t index = 0U; index < size; index++) {
                bytes[index] = 0U;
        }
}

static bool parse_identifier(const char *text, uint32_t *identifier) {
        uint32_t value = 0U;
        if (text == NULL || *text == '\0') {
                return false;
        }
        while (*text != '\0') {
                if (*text < '0' || *text > '9') {
                        return false;
                }
                uint32_t digit = (uint32_t)(*text - '0');
                if (value > (UINT32_MAX - digit) / 10U) {
                        return false;
                }
                value = value * 10U + digit;
                text++;
        }
        *identifier = value;
        return value != 0U;
}

bool rose_gui_connect(const char *identifier_text,
                      struct rose_gui_context *context) {
        uint32_t identifier;
        if (context == NULL ||
            !parse_identifier(identifier_text, &identifier)) {
                return false;
        }
        bytes_zero(context, sizeof(*context));
        if (rose_shared_memory_map(identifier, &context->mapping) != 0 ||
            context->mapping.size < ROSE_GUI_SURFACE_PIXEL_OFFSET) {
                return false;
        }
        context->identifier = identifier;
        context->surface =
            (struct rose_gui_surface *)context->mapping.address;
        if (!rose_font_load() ||
            context->surface->magic != ROSE_GUI_SURFACE_MAGIC ||
            context->surface->version != ROSE_GUI_SURFACE_VERSION ||
            context->surface->stride < context->surface->width * 4U ||
            (uint64_t)ROSE_GUI_SURFACE_PIXEL_OFFSET +
                    (uint64_t)context->surface->stride *
                        context->surface->height >
                context->mapping.size) {
                (void)rose_shared_memory_unmap(identifier);
                bytes_zero(context, sizeof(*context));
                return false;
        }
        context->width = context->surface->width;
        context->height = context->surface->height;
        context->pixel_stride = context->surface->stride / sizeof(uint32_t);
        context->pixels = (uint32_t *)(context->mapping.address +
                                      ROSE_GUI_SURFACE_PIXEL_OFFSET);
        context->surface->client_ready = 1U;
        rose_gui_present(context, 0, 0, (int32_t)context->width,
                         (int32_t)context->height);
        return true;
}

bool rose_gui_refresh_size(struct rose_gui_context *context) {
        if (context == NULL || context->surface == NULL ||
            context->surface->width == 0U ||
            context->surface->height == 0U ||
            context->surface->stride < context->surface->width * 4U ||
            (uint64_t)ROSE_GUI_SURFACE_PIXEL_OFFSET +
                    (uint64_t)context->surface->stride *
                        context->surface->height >
                context->mapping.size) {
                return false;
        }
        context->width = context->surface->width;
        context->height = context->surface->height;
        context->pixel_stride = context->surface->stride / sizeof(uint32_t);
        return true;
}

void rose_gui_disconnect(struct rose_gui_context *context) {
        if (context == NULL || context->surface == NULL) {
                return;
        }
        context->surface->client_closed = 1U;
        __sync_synchronize();
        (void)rose_event_notify(&context->surface->client_closed);
        (void)rose_shared_memory_unmap(context->identifier);
        bytes_zero(context, sizeof(*context));
}

void rose_gui_fill(struct rose_gui_context *context, int32_t x, int32_t y,
                   int32_t width, int32_t height, uint32_t color) {
        if (context == NULL || context->pixels == NULL) {
                return;
        }
        if (x < 0) {
                width += x;
                x = 0;
        }
        if (y < 0) {
                height += y;
                y = 0;
        }
        if (x + width > (int32_t)context->width) {
                width = (int32_t)context->width - x;
        }
        if (y + height > (int32_t)context->height) {
                height = (int32_t)context->height - y;
        }
        if (width <= 0 || height <= 0) {
                return;
        }
        for (int32_t row = y; row < y + height; row++) {
                for (int32_t column = x; column < x + width; column++) {
                        context->pixels[(size_t)row * context->pixel_stride +
                                        (size_t)column] = color;
                }
        }
}

void rose_gui_text(struct rose_gui_context *context, int32_t x, int32_t y,
                   const char *text, uint32_t color, uint32_t scale) {
        if (scale == 0U) {
                return;
        }
        while (text != NULL && *text != '\0') {
                char character = *text++;
                const uint8_t *glyph = rose_font_glyph(character);
                if (glyph == NULL) return;
                for (int32_t glyph_x = 0; glyph_x < ROSE_GUI_FONT_WIDTH;
                     glyph_x++) {
                        for (int32_t glyph_y = 0; glyph_y < ROSE_GUI_FONT_HEIGHT;
                             glyph_y++) {
                                if ((glyph[glyph_x] & (1U << glyph_y)) != 0U) {
                                        rose_gui_fill(
                                            context,
                                            x + glyph_x * (int32_t)scale,
                                            y + glyph_y * (int32_t)scale,
                                            (int32_t)scale, (int32_t)scale,
                                            color);
                                }
                        }
                }
                x += (ROSE_GUI_FONT_WIDTH + 1) * (int32_t)scale;
        }
}

void rose_gui_present(struct rose_gui_context *context, int32_t x, int32_t y,
                      int32_t width, int32_t height) {
        if (context == NULL || context->surface == NULL || width <= 0 ||
            height <= 0) {
                return;
        }
        if (context->surface->damage_sequence !=
            context->surface->damage_consumed) {
                int32_t old_right = context->surface->damage_x +
                                    context->surface->damage_width;
                int32_t old_bottom = context->surface->damage_y +
                                     context->surface->damage_height;
                int32_t right = x + width;
                int32_t bottom = y + height;
                if (context->surface->damage_x < x)
                        x = context->surface->damage_x;
                if (context->surface->damage_y < y)
                        y = context->surface->damage_y;
                if (old_right > right) right = old_right;
                if (old_bottom > bottom) bottom = old_bottom;
                width = right - x;
                height = bottom - y;
        }
        context->surface->damage_x = x;
        context->surface->damage_y = y;
        context->surface->damage_width = width;
        context->surface->damage_height = height;
        __sync_synchronize();
        context->surface->damage_sequence++;
        (void)rose_event_notify(&context->surface->damage_sequence);
}

bool rose_gui_poll_event(struct rose_gui_context *context,
                         struct user_input_event *event) {
        if (context == NULL || context->surface == NULL || event == NULL) {
                return false;
        }
        /* A modifier release may be routed to a newly focused window. Never
         * leave an inactive client with a latched Shift or Control state. */
        if (context->surface->focused == 0U) {
                context->shift = false;
                context->control = false;
        }
        uint32_t read = context->surface->input_read;
        if (read == context->surface->input_write) {
                return false;
        }
        *event = context->surface->input_events[read];
        __sync_synchronize();
        context->surface->input_read =
            (read + 1U) % ROSE_GUI_EVENT_CAPACITY;
        return true;
}

long rose_gui_wait(struct rose_gui_context *context,
                   int64_t timeout_nanoseconds) {
        if (context == NULL || context->surface == NULL) {
                return -USER_ERROR_INVALID_ARGUMENT;
        }
        struct user_wait_item items[4] = {
            {.type = USER_WAIT_OBJECT_SHARED_WORD,
             .events = USER_WAIT_EVENT_CHANGED,
             .identifier =
                 (int64_t)(uintptr_t)&context->surface->input_write,
             .value = context->surface->input_write},
            {.type = USER_WAIT_OBJECT_SHARED_WORD,
             .events = USER_WAIT_EVENT_CHANGED,
             .identifier =
                 (int64_t)(uintptr_t)&context->surface->close_requested,
             .value = context->surface->close_requested},
            {.type = USER_WAIT_OBJECT_SHARED_WORD,
             .events = USER_WAIT_EVENT_CHANGED,
             .identifier = (int64_t)(uintptr_t)&context->surface->focused,
             .value = context->surface->focused},
            {.type = USER_WAIT_OBJECT_SHARED_WORD,
             .events = USER_WAIT_EVENT_CHANGED,
             .identifier =
                 (int64_t)(uintptr_t)&context->surface->resize_sequence,
             .value = context->surface->resize_sequence},
        };
        return rose_wait_events(items, 4U, timeout_nanoseconds);
}

char rose_gui_key_character(struct rose_gui_context *context,
                            const struct user_input_event *event) {
        if (context == NULL || event == NULL ||
            event->type != USER_INPUT_EVENT_KEY) {
                return 0;
        }
        bool pressed = event->value != 0;
        if (event->code == 42U || event->code == 54U) {
                context->shift = pressed;
                return 0;
        }
        if (event->code == 29U || event->code == 97U) {
                context->control = pressed;
                return 0;
        }
        if (event->code == 58U && event->value == 1) {
                context->caps_lock = !context->caps_lock;
                return 0;
        }
        if (!pressed) {
                return 0;
        }

        static const char number_keys[] = "1234567890";
        static const char shifted_number_keys[] = "!@#$%^&*()";
        static const char top_row[] = "qwertyuiop";
        static const char home_row[] = "asdfghjkl";
        static const char bottom_row[] = "zxcvbnm";
        char character = 0;
        if (event->code >= 2U && event->code <= 11U) {
                size_t index = event->code - 2U;
                return context->shift ? shifted_number_keys[index]
                                      : number_keys[index];
        }
        if (event->code >= 16U && event->code <= 25U) {
                character = top_row[event->code - 16U];
        } else if (event->code >= 30U && event->code <= 38U) {
                character = home_row[event->code - 30U];
        } else if (event->code >= 44U && event->code <= 50U) {
                character = bottom_row[event->code - 44U];
        }
        if (character != 0) {
                bool uppercase = context->shift != context->caps_lock;
                if (uppercase) {
                        character = (char)(character - 'a' + 'A');
                }
                if (context->control) {
                        character = (char)((character | 0x20) - 'a' + 1);
                }
                return character;
        }
        switch (event->code) {
        case 1: return 27;
        case 12: return context->shift ? '_' : '-';
        case 13: return context->shift ? '+' : '=';
        case 14: return '\b';
        case 15: return '\t';
        case 26: return context->shift ? '{' : '[';
        case 27: return context->shift ? '}' : ']';
        case 28: return '\n';
        case 39: return context->shift ? ':' : ';';
        case 40: return context->shift ? '"' : '\'';
        case 41: return context->shift ? '~' : '`';
        case 43: return context->shift ? '|' : '\\';
        case 51: return context->shift ? '<' : ',';
        case 52: return context->shift ? '>' : '.';
        case 53: return context->shift ? '?' : '/';
        case 57: return ' ';
        default: return 0;
        }
}

void rose_gui_unsigned(char *buffer, size_t size, uint64_t value) {
        if (buffer == NULL || size == 0U) {
                return;
        }
        char reversed[21];
        size_t count = 0U;
        do {
                reversed[count++] = (char)('0' + value % 10U);
                value /= 10U;
        } while (value != 0U && count < sizeof(reversed));
        size_t written = 0U;
        while (count != 0U && written + 1U < size) {
                buffer[written++] = reversed[--count];
        }
        buffer[written] = '\0';
}
