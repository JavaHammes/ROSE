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
        bool shift;
        bool control;
        bool caps_lock;
};

bool rose_gui_connect(const char *identifier, struct rose_gui_context *context);
bool rose_gui_refresh_size(struct rose_gui_context *context);
void rose_gui_disconnect(struct rose_gui_context *context);
void rose_gui_fill(struct rose_gui_context *context, int32_t x, int32_t y,
                   int32_t width, int32_t height, uint32_t color);
void rose_gui_text(struct rose_gui_context *context, int32_t x, int32_t y,
                   const char *text, uint32_t color, uint32_t scale);
void rose_gui_present(struct rose_gui_context *context, int32_t x, int32_t y,
                      int32_t width, int32_t height);
bool rose_gui_poll_event(struct rose_gui_context *context,
                         struct user_input_event *event);
long rose_gui_wait(struct rose_gui_context *context,
                   int64_t timeout_nanoseconds);
char rose_gui_key_character(struct rose_gui_context *context,
                            const struct user_input_event *event);
void rose_gui_unsigned(char *buffer, size_t size, uint64_t value);

#endif
