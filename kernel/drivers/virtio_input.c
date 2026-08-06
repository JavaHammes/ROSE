/* VirtIO keyboard and absolute tablet support. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "plic.h"
#include "uart.h"
#include "user_abi.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "virtio_mmio.h"

enum {
        INPUT_DEVICE_LIMIT = 2,
        INPUT_QUEUE_SIZE = 32,
        INPUT_CONFIG_SELECT = 0,
        INPUT_CONFIG_SUBSELECT = 1,
        INPUT_CONFIG_SIZE = 2,
        INPUT_CONFIG_DATA = 8,
        INPUT_CONFIG_EVENT_BITS = 0x11,
        INPUT_CONFIG_ABSOLUTE_INFO = 0x12,
        EVENT_SYNCHRONIZE = 0,
        EVENT_KEY = 1,
        EVENT_ABSOLUTE = 3,
        SYNCHRONIZE_REPORT = 0,
        ABSOLUTE_X = 0,
        ABSOLUTE_Y = 1,
        KEY_ESCAPE = 1,
        KEY_LEFT_CONTROL = 29,
        KEY_LEFT_SHIFT = 42,
        KEY_RIGHT_SHIFT = 54,
        KEY_CAPS_LOCK = 58,
        KEY_RIGHT_CONTROL = 97,
        KEY_UP = 103,
        KEY_LEFT = 105,
        KEY_RIGHT = 106,
        KEY_DOWN = 108,
        BUTTON_LEFT = 0x110,
        BUTTON_RIGHT = 0x111,
        BUTTON_MIDDLE = 0x112,
};

struct virtio_input_event {
        uint16_t type;
        uint16_t code;
        uint32_t value;
};

struct absolute_information {
        uint32_t minimum;
        uint32_t maximum;
        uint32_t fuzz;
        uint32_t flat;
        uint32_t resolution;
};

struct input_device {
        struct virtio_mmio_device transport;
        struct virtio_queue event_queue;
        struct virtio_input_event events[INPUT_QUEUE_SIZE];
        bool keyboard;
        bool pointer;
        bool shift;
        bool control;
        bool caps_lock;
        struct absolute_information x_information;
        struct absolute_information y_information;
        uint32_t raw_x;
        uint32_t raw_y;
        uint32_t x;
        uint32_t y;
        uint32_t buttons;
};

static struct input_device devices[INPUT_DEVICE_LIMIT];
static size_t device_count;

static uint8_t config_size(struct input_device *device, uint8_t select,
                           uint8_t subselect) {
        virtio_mmio_config_write8(&device->transport, INPUT_CONFIG_SELECT,
                                  select);
        virtio_mmio_config_write8(&device->transport, INPUT_CONFIG_SUBSELECT,
                                  subselect);
        return virtio_mmio_config_read8(&device->transport, INPUT_CONFIG_SIZE);
}

static struct absolute_information
read_absolute_information(struct input_device *device, uint8_t axis) {
        struct absolute_information information = {0};

        if (config_size(device, INPUT_CONFIG_ABSOLUTE_INFO, axis) <
            sizeof(information)) {
                return information;
        }
        information.minimum =
            virtio_mmio_config_read32(&device->transport, INPUT_CONFIG_DATA);
        information.maximum = virtio_mmio_config_read32(&device->transport,
                                                        INPUT_CONFIG_DATA + 4U);
        information.fuzz = virtio_mmio_config_read32(&device->transport,
                                                     INPUT_CONFIG_DATA + 8U);
        information.flat = virtio_mmio_config_read32(&device->transport,
                                                     INPUT_CONFIG_DATA + 12U);
        information.resolution = virtio_mmio_config_read32(
            &device->transport, INPUT_CONFIG_DATA + 16U);
        return information;
}

static char map_key(uint16_t code, bool shifted) {
        static const char number_keys[] = "1234567890";
        static const char shifted_number_keys[] = "!@#$%^&*()";
        static const char top_row[] = "qwertyuiop";
        static const char home_row[] = "asdfghjkl";
        static const char bottom_row[] = "zxcvbnm";
        char character = 0;

        if (code >= 2U && code <= 11U) {
                size_t index = code - 2U;
                return shifted ? shifted_number_keys[index]
                               : number_keys[index];
        }
        if (code >= 16U && code <= 25U) {
                character = top_row[code - 16U];
        } else if (code >= 30U && code <= 38U) {
                character = home_row[code - 30U];
        } else if (code >= 44U && code <= 50U) {
                character = bottom_row[code - 44U];
        }
        if (character != 0) {
                return shifted ? (char)(character - 'a' + 'A') : character;
        }

        switch (code) {
        case KEY_ESCAPE:
                return 27;
        case 12:
                return shifted ? '_' : '-';
        case 13:
                return shifted ? '+' : '=';
        case 14:
                return '\b';
        case 15:
                return '\t';
        case 26:
                return shifted ? '{' : '[';
        case 27:
                return shifted ? '}' : ']';
        case 28:
                return '\n';
        case 39:
                return shifted ? ':' : ';';
        case 40:
                return shifted ? '"' : '\'';
        case 41:
                return shifted ? '~' : '`';
        case 43:
                return shifted ? '|' : '\\';
        case 51:
                return shifted ? '<' : ',';
        case 52:
                return shifted ? '>' : '.';
        case 53:
                return shifted ? '?' : '/';
        case 57:
                return ' ';
        default:
                return 0;
        }
}

static void submit_terminal_sequence(const char *sequence) {
        while (*sequence != '\0') {
                uart_receive_character(*sequence++);
        }
}

static void handle_keyboard_event(struct input_device *device,
                                  const struct virtio_input_event *event) {
        struct user_input_event user_event = {
            .type = USER_INPUT_EVENT_KEY,
            .code = event->code,
            .value = (int32_t)event->value,
        };
        input_event_push(&user_event);

        bool pressed = event->value != 0U;
        if (event->code == KEY_LEFT_SHIFT || event->code == KEY_RIGHT_SHIFT) {
                device->shift = pressed;
                return;
        }
        if (event->code == KEY_LEFT_CONTROL ||
            event->code == KEY_RIGHT_CONTROL) {
                device->control = pressed;
                return;
        }
        if (event->code == KEY_CAPS_LOCK && event->value == 1U) {
                device->caps_lock = !device->caps_lock;
                return;
        }
        if (!pressed) {
                return;
        }

        if (input_console_captured()) {
                return;
        }
        if (event->code == KEY_UP) {
                submit_terminal_sequence("\033[A");
                return;
        }
        if (event->code == KEY_DOWN) {
                submit_terminal_sequence("\033[B");
                return;
        }
        if (event->code == KEY_RIGHT) {
                submit_terminal_sequence("\033[C");
                return;
        }
        if (event->code == KEY_LEFT) {
                submit_terminal_sequence("\033[D");
                return;
        }

        char character = map_key(event->code, device->shift);
        if (device->caps_lock && character >= 'a' && character <= 'z') {
                character = (char)(character - 'a' + 'A');
        } else if (device->caps_lock && character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
        }
        if (device->control && character >= 'a' && character <= 'z') {
                character = (char)(character - 'a' + 1);
        } else if (device->control && character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 1);
        }
        if (character != 0) {
                uart_receive_character(character);
        }
}

static uint32_t scale_axis(uint32_t raw,
                           const struct absolute_information *information,
                           uint32_t extent) {
        if (extent == 0U || information->maximum <= information->minimum) {
                return 0U;
        }
        if (raw <= information->minimum) {
                return 0U;
        }
        if (raw >= information->maximum) {
                return extent - 1U;
        }
        return (
            uint32_t)(((uint64_t)(raw - information->minimum) * (extent - 1U)) /
                      (information->maximum - information->minimum));
}

static void handle_pointer_event(struct input_device *device,
                                 const struct virtio_input_event *event) {
        if (event->type == EVENT_ABSOLUTE) {
                if (event->code == ABSOLUTE_X) {
                        device->raw_x = event->value;
                } else if (event->code == ABSOLUTE_Y) {
                        device->raw_y = event->value;
                }
                return;
        }
        if (event->type == EVENT_KEY) {
                uint32_t mask = 0U;
                if (event->code == BUTTON_LEFT) {
                        mask = USER_POINTER_BUTTON_LEFT;
                } else if (event->code == BUTTON_RIGHT) {
                        mask = USER_POINTER_BUTTON_RIGHT;
                } else if (event->code == BUTTON_MIDDLE) {
                        mask = USER_POINTER_BUTTON_MIDDLE;
                }
                if (event->value != 0U) {
                        device->buttons |= mask;
                } else {
                        device->buttons &= ~mask;
                }
                return;
        }
        if (event->type != EVENT_SYNCHRONIZE ||
            event->code != SYNCHRONIZE_REPORT) {
                return;
        }

        uint32_t width =
            virtio_gpu_available() ? virtio_gpu_width() : VIRTIO_GPU_MAX_WIDTH;
        uint32_t height = virtio_gpu_available()
                              ? virtio_gpu_height()
                              : VIRTIO_GPU_MAX_HEIGHT;
        device->x = scale_axis(device->raw_x, &device->x_information, width);
        device->y = scale_axis(device->raw_y, &device->y_information, height);
        struct user_input_event user_event = {
            .type = USER_INPUT_EVENT_POINTER,
            .x = (int32_t)device->x,
            .y = (int32_t)device->y,
            .buttons = device->buttons,
        };
        input_event_push(&user_event);
}

static void handle_device_interrupt(struct input_device *device) {
        (void)virtio_mmio_ack_interrupt(&device->transport);
        uint32_t identifier;
        uint32_t length;

        while (
            virtio_queue_pop_used(&device->event_queue, &identifier, &length)) {
                if (identifier >= INPUT_QUEUE_SIZE ||
                    length < sizeof(struct virtio_input_event)) {
                        continue;
                }
                struct virtio_input_event *event = &device->events[identifier];
                if (device->keyboard && event->type == EVENT_KEY) {
                        handle_keyboard_event(device, event);
                }
                if (device->pointer) {
                        handle_pointer_event(device, event);
                }
                virtio_queue_submit(&device->transport, &device->event_queue,
                                    (uint16_t)identifier);
        }
}

static void handle_device_zero_interrupt(void) {
        handle_device_interrupt(&devices[0]);
}

static void handle_device_one_interrupt(void) {
        handle_device_interrupt(&devices[1]);
}

static bool initialize_device(size_t instance) {
        struct input_device *device = &devices[device_count];

        if (!virtio_mmio_begin(&device->transport, VIRTIO_DEVICE_INPUT,
                               instance, 0U, 0U)) {
                return false;
        }
        uint8_t absolute_size =
            config_size(device, INPUT_CONFIG_EVENT_BITS, EVENT_ABSOLUTE);
        uint8_t key_size =
            config_size(device, INPUT_CONFIG_EVENT_BITS, EVENT_KEY);
        device->pointer = absolute_size != 0U;
        device->keyboard = !device->pointer && key_size != 0U;
        if (!device->pointer && !device->keyboard) {
                virtio_mmio_fail(&device->transport);
                return false;
        }
        if (device->pointer) {
                device->x_information =
                    read_absolute_information(device, ABSOLUTE_X);
                device->y_information =
                    read_absolute_information(device, ABSOLUTE_Y);
        }
        if (!virtio_mmio_queue_init(&device->transport, &device->event_queue,
                                    0U, INPUT_QUEUE_SIZE)) {
                virtio_mmio_fail(&device->transport);
                return false;
        }

        plic_interrupt_handler handler = device_count == 0U
                                             ? handle_device_zero_interrupt
                                             : handle_device_one_interrupt;
        if (!plic_register_handler(device->transport.interrupt, handler)) {
                virtio_mmio_fail(&device->transport);
                return false;
        }

        for (size_t index = 0U; index < INPUT_QUEUE_SIZE; index++) {
                virtio_queue_set_descriptor(&device->event_queue,
                                            (uint16_t)index,
                                            (uintptr_t)&device->events[index],
                                            sizeof(struct virtio_input_event),
                                            VIRTIO_DESCRIPTOR_WRITE, 0U);
        }
        virtio_mmio_finish(&device->transport);
        for (size_t index = 0U; index < INPUT_QUEUE_SIZE; index++) {
                virtio_queue_submit(&device->transport, &device->event_queue,
                                    (uint16_t)index);
        }
        device_count++;
        return true;
}

bool virtio_input_init(void) {
        for (size_t instance = 0U; instance < INPUT_DEVICE_LIMIT; instance++) {
                if (!initialize_device(instance)) {
                        break;
                }
        }
        return device_count != 0U;
}
