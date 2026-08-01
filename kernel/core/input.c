#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input.h"

enum { INPUT_EVENT_QUEUE_SIZE = 128 };

static struct user_input_event events[INPUT_EVENT_QUEUE_SIZE];
static volatile uint32_t read_index;
static volatile uint32_t write_index;
static bool console_captured;

static uint32_t next_index(uint32_t index) {
        return (index + 1U) % INPUT_EVENT_QUEUE_SIZE;
}

void input_event_push(const struct user_input_event *event) {
        if (event == NULL) {
                return;
        }

        uint32_t next = next_index(write_index);
        if (next == read_index) {
                read_index = next_index(read_index);
        }
        events[write_index] = *event;
        write_index = next;
}

bool input_event_pop(struct user_input_event *event) {
        if (event == NULL || read_index == write_index) {
                return false;
        }

        *event = events[read_index];
        read_index = next_index(read_index);
        return true;
}

void input_event_clear(void) { read_index = write_index; }

void input_set_console_captured(bool captured) { console_captured = captured; }

bool input_console_captured(void) { return console_captured; }
