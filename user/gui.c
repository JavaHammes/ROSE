#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"

_Static_assert(sizeof(struct rose_gui_surface) <=
                   ROSE_GUI_SURFACE_PIXEL_OFFSET,
               "GUI control page exceeds its reserved page");

static const uint8_t font[59][ROSE_GUI_FONT_WIDTH] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, {0x24, 0x2a, 0x7f, 0x2a, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50},
    {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1c, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x14, 0x08, 0x3e, 0x08, 0x14},
    {0x08, 0x08, 0x3e, 0x08, 0x08}, {0x00, 0x50, 0x30, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3e, 0x51, 0x49, 0x45, 0x3e},
    {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00},
    {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08},
    {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3e},
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c},
    {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01},
    {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06},
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f},
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

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
        if (context->surface->magic != ROSE_GUI_SURFACE_MAGIC ||
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

void rose_gui_disconnect(struct rose_gui_context *context) {
        if (context == NULL || context->surface == NULL) {
                return;
        }
        context->surface->client_closed = 1U;
        __sync_synchronize();
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
                if (character >= 'a' && character <= 'z') {
                        character = (char)(character - 'a' + 'A');
                }
                if (character < ' ' || character > 'Z') {
                        character = '?';
                }
                const uint8_t *glyph = font[(uint8_t)character - ' '];
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
        context->surface->damage_x = x;
        context->surface->damage_y = y;
        context->surface->damage_width = width;
        context->surface->damage_height = height;
        __sync_synchronize();
        context->surface->damage_sequence++;
}

bool rose_gui_poll_event(struct rose_gui_context *context,
                         struct user_input_event *event) {
        if (context == NULL || context->surface == NULL || event == NULL) {
                return false;
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
