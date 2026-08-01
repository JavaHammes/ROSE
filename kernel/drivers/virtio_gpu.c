/* VirtIO-GPU 2D scanout. Pixels use little-endian B8G8R8X8 (0x00RRGGBB). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "page_allocator.h"
#include "plic.h"
#include "virtio_gpu.h"
#include "virtio_mmio.h"

enum {
        GPU_QUEUE_SIZE = 16,
        GPU_RESOURCE_ID = 1,
        GPU_SCANOUT_ID = 0,
        GPU_FORMAT_B8G8R8X8_UNORM = 2,
        GPU_COMMAND_GET_DISPLAY_INFO = 0x0100,
        GPU_COMMAND_RESOURCE_CREATE_2D = 0x0101,
        GPU_COMMAND_SET_SCANOUT = 0x0103,
        GPU_COMMAND_RESOURCE_FLUSH = 0x0104,
        GPU_COMMAND_TRANSFER_TO_HOST_2D = 0x0105,
        GPU_COMMAND_RESOURCE_ATTACH_BACKING = 0x0106,
        GPU_RESPONSE_OK_NODATA = 0x1100,
        GPU_RESPONSE_OK_DISPLAY_INFO = 0x1101,
        GPU_DISPLAY_COUNT = 16,
};

struct gpu_control_header {
        uint32_t type;
        uint32_t flags;
        uint64_t fence_id;
        uint32_t context_id;
        uint8_t ring_index;
        uint8_t padding[3];
};

struct gpu_rectangle {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
};

struct gpu_display_mode {
        struct gpu_rectangle rectangle;
        uint32_t enabled;
        uint32_t flags;
};

struct gpu_display_info_response {
        struct gpu_control_header header;
        struct gpu_display_mode modes[GPU_DISPLAY_COUNT];
};

struct gpu_resource_create_2d {
        struct gpu_control_header header;
        uint32_t resource_id;
        uint32_t format;
        uint32_t width;
        uint32_t height;
};

struct gpu_resource_attach_backing {
        struct gpu_control_header header;
        uint32_t resource_id;
        uint32_t entry_count;
};

struct gpu_memory_entry {
        uint64_t address;
        uint32_t length;
        uint32_t padding;
};

struct gpu_set_scanout {
        struct gpu_control_header header;
        struct gpu_rectangle rectangle;
        uint32_t scanout_id;
        uint32_t resource_id;
};

struct gpu_transfer_to_host_2d {
        struct gpu_control_header header;
        struct gpu_rectangle rectangle;
        uint64_t offset;
        uint32_t resource_id;
        uint32_t padding;
};

struct gpu_resource_flush {
        struct gpu_control_header header;
        struct gpu_rectangle rectangle;
        uint32_t resource_id;
        uint32_t padding;
};

static struct virtio_mmio_device transport;
static struct virtio_queue control_queue;
static uint32_t framebuffer[VIRTIO_GPU_MAX_WIDTH * VIRTIO_GPU_MAX_HEIGHT]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t scanout_width;
static uint32_t scanout_height;
static bool ready;

static void zero_bytes(void *pointer, size_t length) {
        uint8_t *bytes = pointer;

        for (size_t index = 0U; index < length; index++) {
                bytes[index] = 0U;
        }
}

static bool submit_command(const void *request, uint32_t request_length,
                           const void *extra, uint32_t extra_length,
                           void *response, uint32_t response_length,
                           uint32_t expected_response) {
        uint16_t response_descriptor = extra == NULL ? 1U : 2U;

        zero_bytes(response, response_length);
        virtio_queue_set_descriptor(&control_queue, 0U, (uintptr_t)request,
                                    request_length, VIRTIO_DESCRIPTOR_NEXT, 1U);
        if (extra != NULL) {
                virtio_queue_set_descriptor(&control_queue, 1U,
                                            (uintptr_t)extra, extra_length,
                                            VIRTIO_DESCRIPTOR_NEXT, 2U);
        }
        virtio_queue_set_descriptor(&control_queue, response_descriptor,
                                    (uintptr_t)response, response_length,
                                    VIRTIO_DESCRIPTOR_WRITE, 0U);
        virtio_queue_submit(&transport, &control_queue, 0U);
        if (!virtio_queue_wait_used(&transport, &control_queue, 0U, NULL)) {
                return false;
        }

        return ((struct gpu_control_header *)response)->type ==
               expected_response;
}

static bool command_without_data(const void *request, uint32_t length) {
        struct gpu_control_header response;

        return submit_command(request, length, NULL, 0U, &response,
                              sizeof(response), GPU_RESPONSE_OK_NODATA);
}

static struct gpu_rectangle full_rectangle(void) {
        return (struct gpu_rectangle){
            .x = 0U,
            .y = 0U,
            .width = scanout_width,
            .height = scanout_height,
        };
}

static bool initialize_resource(void) {
        struct gpu_resource_create_2d create = {
            .header = {.type = GPU_COMMAND_RESOURCE_CREATE_2D},
            .resource_id = GPU_RESOURCE_ID,
            .format = GPU_FORMAT_B8G8R8X8_UNORM,
            .width = scanout_width,
            .height = scanout_height,
        };
        if (!command_without_data(&create, sizeof(create))) {
                return false;
        }

        struct gpu_resource_attach_backing attach = {
            .header = {.type = GPU_COMMAND_RESOURCE_ATTACH_BACKING},
            .resource_id = GPU_RESOURCE_ID,
            .entry_count = 1U,
        };
        struct gpu_memory_entry entry = {
            .address = (uintptr_t)framebuffer,
            .length = (uint32_t)virtio_gpu_framebuffer_size(),
        };
        struct gpu_control_header response;
        if (!submit_command(&attach, sizeof(attach), &entry, sizeof(entry),
                            &response, sizeof(response),
                            GPU_RESPONSE_OK_NODATA)) {
                return false;
        }

        struct gpu_set_scanout scanout = {
            .header = {.type = GPU_COMMAND_SET_SCANOUT},
            .rectangle = full_rectangle(),
            .scanout_id = GPU_SCANOUT_ID,
            .resource_id = GPU_RESOURCE_ID,
        };
        return command_without_data(&scanout, sizeof(scanout));
}

static bool select_display_mode(void) {
        struct gpu_control_header request = {
            .type = GPU_COMMAND_GET_DISPLAY_INFO,
        };
        struct gpu_display_info_response response;

        if (!submit_command(&request, sizeof(request), NULL, 0U, &response,
                            sizeof(response), GPU_RESPONSE_OK_DISPLAY_INFO)) {
                return false;
        }

        for (size_t index = 0U; index < GPU_DISPLAY_COUNT; index++) {
                if (response.modes[index].enabled == 0U ||
                    response.modes[index].rectangle.width == 0U ||
                    response.modes[index].rectangle.height == 0U) {
                        continue;
                }

                scanout_width = response.modes[index].rectangle.width;
                scanout_height = response.modes[index].rectangle.height;
                if (scanout_width > VIRTIO_GPU_MAX_WIDTH) {
                        scanout_width = VIRTIO_GPU_MAX_WIDTH;
                }
                if (scanout_height > VIRTIO_GPU_MAX_HEIGHT) {
                        scanout_height = VIRTIO_GPU_MAX_HEIGHT;
                }
                return true;
        }
        return false;
}

static bool flush_rectangle(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height) {
        struct gpu_rectangle rectangle = {
            .x = x,
            .y = y,
            .width = width,
            .height = height,
        };
        struct gpu_transfer_to_host_2d transfer = {
            .header = {.type = GPU_COMMAND_TRANSFER_TO_HOST_2D},
            .rectangle = rectangle,
            .offset = ((uint64_t)y * scanout_width + x) * sizeof(uint32_t),
            .resource_id = GPU_RESOURCE_ID,
        };
        if (!command_without_data(&transfer, sizeof(transfer))) {
                return false;
        }

        struct gpu_resource_flush flush = {
            .header = {.type = GPU_COMMAND_RESOURCE_FLUSH},
            .rectangle = rectangle,
            .resource_id = GPU_RESOURCE_ID,
        };
        return command_without_data(&flush, sizeof(flush));
}

static void handle_interrupt(void) {
        (void)virtio_mmio_ack_interrupt(&transport);
}

bool virtio_gpu_init(void) {
        if (!virtio_mmio_begin(&transport, VIRTIO_DEVICE_GPU, 0U, 0U, 0U) ||
            !virtio_mmio_queue_init(&transport, &control_queue, 0U,
                                    GPU_QUEUE_SIZE)) {
                virtio_mmio_fail(&transport);
                return false;
        }
        virtio_mmio_finish(&transport);

        if (!select_display_mode() || !initialize_resource() ||
            !plic_register_handler(transport.interrupt, handle_interrupt)) {
                virtio_mmio_fail(&transport);
                return false;
        }

        for (size_t index = 0U; index < (size_t)scanout_width * scanout_height;
             index++) {
                framebuffer[index] = UINT32_C(0x0010182a);
        }
        ready = flush_rectangle(0U, 0U, scanout_width, scanout_height);
        return ready;
}

bool virtio_gpu_available(void) { return ready; }

uint32_t virtio_gpu_width(void) { return scanout_width; }

uint32_t virtio_gpu_height(void) { return scanout_height; }

uint32_t virtio_gpu_stride(void) { return scanout_width * sizeof(uint32_t); }

uint32_t *virtio_gpu_framebuffer(void) { return framebuffer; }

size_t virtio_gpu_framebuffer_size(void) {
        return (size_t)VIRTIO_GPU_MAX_WIDTH * VIRTIO_GPU_MAX_HEIGHT *
               sizeof(uint32_t);
}

bool virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        if (!ready || width == 0U || height == 0U || x >= scanout_width ||
            y >= scanout_height || width > scanout_width - x ||
            height > scanout_height - y) {
                return false;
        }
        return flush_rectangle(x, y, width, height);
}
