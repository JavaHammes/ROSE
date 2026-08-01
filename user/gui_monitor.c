#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"

#define MONITOR_BACKGROUND UINT32_C(0x00151d2e)
#define MONITOR_CARD UINT32_C(0x00212d43)
#define MONITOR_TEXT UINT32_C(0x00ecf3ff)
#define MONITOR_MUTED UINT32_C(0x00899ab5)
#define MONITOR_GREEN UINT32_C(0x0047d7a1)
#define MONITOR_BLUE UINT32_C(0x005f7cff)

static void draw_metric(struct rose_gui_context *gui, int32_t y,
                        const char *label, uint64_t value, uint32_t color,
                        uint32_t width) {
        char number[24];
        rose_gui_unsigned(number, sizeof(number), value);
        rose_gui_text(gui, 24, y, label, MONITOR_MUTED, 1U);
        rose_gui_text(gui, (int32_t)gui->width - 150, y, number, MONITOR_TEXT,
                      1U);
        rose_gui_fill(gui, 24, y + 17, (int32_t)gui->width - 48, 8,
                      UINT32_C(0x0034415a));
        rose_gui_fill(gui, 24, y + 17, (int32_t)width, 8, color);
}

static void monitor_render(struct rose_gui_context *gui,
                           const struct user_system_info *info) {
        rose_gui_fill(gui, 0, 0, (int32_t)gui->width, (int32_t)gui->height,
                      MONITOR_BACKGROUND);
        rose_gui_text(gui, 22, 20, "SYSTEM MONITOR", MONITOR_TEXT, 2U);
        rose_gui_text(gui, 24, 48, "LIVE KERNEL TELEMETRY", MONITOR_MUTED,
                      1U);
        rose_gui_fill(gui, 16, 72, (int32_t)gui->width - 32,
                      (int32_t)gui->height - 88, MONITOR_CARD);

        uint32_t available = gui->width > 64U ? gui->width - 64U : 1U;
        uint32_t memory_width = info->total_pages == 0U
                                    ? 0U
                                    : (uint32_t)(info->used_pages * available /
                                                 info->total_pages);
        draw_metric(gui, 94, "MEMORY PAGES", info->used_pages, MONITOR_GREEN,
                    memory_width);
        draw_metric(gui, 146, "PROCESSES", info->process_count, MONITOR_BLUE,
                    info->process_count * 18U % available);
        draw_metric(gui, 198, "CONTEXT SWITCHES", info->context_switches,
                    MONITOR_GREEN,
                    (uint32_t)(info->context_switches % available));
        draw_metric(gui, 250, "PREEMPTIONS", info->scheduler_preemptions,
                    MONITOR_BLUE,
                    (uint32_t)(info->scheduler_preemptions % available));
        draw_metric(gui, 302, "BLOCKS", info->scheduler_blocks, MONITOR_GREEN,
                    (uint32_t)(info->scheduler_blocks % available));
        draw_metric(gui, 354, "COW COPIES", info->copy_on_write_copies,
                    MONITOR_BLUE,
                    (uint32_t)(info->copy_on_write_copies % available));
        rose_gui_present(gui, 0, 0, (int32_t)gui->width,
                         (int32_t)gui->height);
}

int rose_gui_monitor_main(int argc, char **argv) {
        struct rose_gui_context gui;
        if (argc != 2 || !rose_gui_connect(argv[1], &gui)) return 1;
        uint32_t delay = 0U;
        while (gui.surface->close_requested == 0U) {
                if (delay == 0U) {
                        struct user_system_info information;
                        if (rose_system_info(&information) == 0) {
                                monitor_render(&gui, &information);
                        }
                }
                delay = (delay + 1U) % 32U;
                rose_yield();
        }
        rose_gui_disconnect(&gui);
        return 0;
}
