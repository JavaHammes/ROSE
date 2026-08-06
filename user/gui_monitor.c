#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"

enum { MONITOR_METRIC_COUNT = 6 };

struct monitor_metric {
        struct rose_gui_widget widget;
        const char *label;
        char value_text[24];
        uint64_t value;
        uint64_t maximum;
        bool alternate;
};

struct monitor_state {
        struct rose_gui_widget root;
        struct rose_gui_widget header;
        struct rose_gui_widget body;
        struct rose_gui_widget subtitle;
        struct monitor_metric metrics[MONITOR_METRIC_COUNT];
        struct rose_gui_widget status;
};

static void metric_draw(struct rose_gui_widget *widget,
                        struct rose_gui_canvas *canvas,
                        const struct rose_gui_theme *theme, void *user_data) {
        struct monitor_metric *metric = user_data;
        int32_t text_y = widget->bounds.y + 5;
        rose_gui_canvas_text(canvas, widget->bounds.x + 8, text_y,
                             metric->label, theme->muted, 1U);
        int32_t value_width = rose_gui_text_width(metric->value_text, 1U);
        rose_gui_canvas_text(canvas,
                             widget->bounds.x + widget->bounds.width -
                                 value_width - 8,
                             text_y, metric->value_text, theme->text, 1U);
        int32_t bar_x = widget->bounds.x + 8;
        int32_t bar_y = widget->bounds.y + widget->bounds.height - 13;
        int32_t bar_width = widget->bounds.width - 16;
        rose_gui_canvas_rounded_rectangle(
            canvas, (struct rose_gui_rectangle){bar_x, bar_y, bar_width, 7},
            3, theme->surface_alternate);
        uint64_t maximum = metric->maximum == 0U ? 1U : metric->maximum;
        int32_t used = (int32_t)(metric->value * (uint64_t)bar_width / maximum);
        if (used < 0) used = 0;
        if (used > bar_width) used = bar_width;
        if (used != 0) {
                rose_gui_canvas_rounded_rectangle(
                    canvas,
                    (struct rose_gui_rectangle){bar_x, bar_y, used, 7}, 3,
                    metric->alternate ? theme->accent : theme->success);
        }
}

static void set_metric(struct monitor_metric *metric, uint64_t value,
                       uint64_t maximum) {
        metric->value = value;
        if (maximum != 0U) {
                metric->maximum = maximum;
        } else if (value >= metric->maximum) {
                metric->maximum = value + value / 4U + 1U;
        }
        rose_gui_unsigned(metric->value_text, sizeof(metric->value_text),
                          value);
}

static bool monitor_update(struct rose_gui_application *app, uint64_t now,
                           void *user_data) {
        (void)app;
        (void)now;
        struct monitor_state *state = user_data;
        struct user_system_info information;
        if (rose_system_info(&information) != 0) return false;
        set_metric(&state->metrics[0], information.used_pages,
                   information.total_pages);
        set_metric(&state->metrics[1], information.process_count,
                   USER_PROCESS_INFO_LIMIT);
        set_metric(&state->metrics[2], information.context_switches, 0U);
        set_metric(&state->metrics[3], information.scheduler_preemptions, 0U);
        set_metric(&state->metrics[4], information.scheduler_blocks, 0U);
        set_metric(&state->metrics[5], information.copy_on_write_copies, 0U);
        return true;
}

static void monitor_build_ui(struct monitor_state *state) {
        static const char *const labels[MONITOR_METRIC_COUNT] = {
            "MEMORY PAGES", "PROCESSES", "CONTEXT SWITCHES", "PREEMPTIONS",
            "SCHEDULER BLOCKS", "COPY-ON-WRITE COPIES"};
        rose_gui_widget_initialize(&state->root, ROSE_GUI_WIDGET_ROOT, NULL);
        state->root.padding = 0U;
        state->root.gap = 0U;

        rose_gui_widget_initialize(&state->header, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "SYSTEM MONITOR");
        rose_gui_widget_set_minimum(&state->header, 0, 42);

        rose_gui_widget_initialize(&state->body, ROSE_GUI_WIDGET_COLUMN, NULL);
        state->body.padding = 16U;
        state->body.gap = 7U;
        state->body.flags |= ROSE_GUI_WIDGET_SURFACE;
        rose_gui_widget_set_flex(&state->body, 1U);

        rose_gui_widget_initialize(&state->subtitle, ROSE_GUI_WIDGET_LABEL,
                                   "LIVE KERNEL TELEMETRY");
        rose_gui_widget_set_minimum(&state->subtitle, 0, 18);
        rose_gui_widget_add(&state->root, &state->header);
        rose_gui_widget_add(&state->root, &state->body);
        rose_gui_widget_add(&state->body, &state->subtitle);

        for (size_t index = 0U; index < MONITOR_METRIC_COUNT; index++) {
                struct monitor_metric *metric = &state->metrics[index];
                metric->label = labels[index];
                metric->alternate = (index & 1U) != 0U;
                metric->maximum = 1U;
                metric->value_text[0] = '0';
                metric->value_text[1] = '\0';
                rose_gui_widget_initialize(&metric->widget,
                                           ROSE_GUI_WIDGET_CUSTOM, NULL);
                rose_gui_widget_set_minimum(&metric->widget, 0, 36);
                rose_gui_widget_set_flex(&metric->widget, 1U);
                metric->widget.custom_draw = metric_draw;
                metric->widget.user_data = metric;
                rose_gui_widget_add(&state->body, &metric->widget);
        }

        rose_gui_widget_initialize(&state->status, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "UPDATES EVERY 500 MS");
        rose_gui_widget_set_minimum(&state->status, 0, 24);
        rose_gui_widget_add(&state->body, &state->status);
}

int rose_gui_monitor_main(int argc, char **argv) {
        static struct monitor_state state;
        static struct rose_gui_application app;
        if (argc != 2) return 1;
        monitor_build_ui(&state);
        if (!rose_gui_application_initialize(&app, argv[1], &state.root))
                return 1;
        app.update_interval = UINT64_C(500000000);
        app.update = monitor_update;
        app.user_data = &state;
        return rose_gui_application_run(&app);
}
