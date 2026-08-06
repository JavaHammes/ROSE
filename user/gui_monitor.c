#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/gui.h"
#include "rose/syscall.h"

enum { PROCESS_TEXT_LIMIT = 96, DETAIL_TEXT_LIMIT = 128 };

struct monitor_state {
        struct user_process_info processes[USER_PROCESS_INFO_LIMIT];
        char process_text[USER_PROCESS_INFO_LIMIT][PROCESS_TEXT_LIMIT];
        const char *process_items[USER_PROCESS_INFO_LIMIT];
        size_t process_count;
        char summary_text[DETAIL_TEXT_LIMIT];
        char detail_text[DETAIL_TEXT_LIMIT];
        struct rose_gui_application *app;
        struct rose_gui_widget root;
        struct rose_gui_widget header;
        struct rose_gui_widget body;
        struct rose_gui_widget summary;
        struct rose_gui_widget process_heading;
        struct rose_gui_widget process_list;
        struct rose_gui_widget detail;
        struct rose_gui_widget actions;
        struct rose_gui_widget terminate;
        struct rose_gui_widget kill;
        struct rose_gui_widget status;
};

static size_t string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') length++;
        return length;
}

static void string_copy(char *destination, size_t capacity,
                        const char *source) {
        size_t index = 0U;
        while (index + 1U < capacity && source[index] != '\0') {
                destination[index] = source[index];
                index++;
        }
        destination[index] = '\0';
}

static void string_append(char *destination, size_t capacity,
                          const char *source) {
        size_t output = string_length(destination);
        size_t input = 0U;
        while (output + 1U < capacity && source[input] != '\0')
                destination[output++] = source[input++];
        destination[output] = '\0';
}

static void append_number(char *destination, size_t capacity,
                          uint64_t value) {
        char number[24];
        rose_gui_unsigned(number, sizeof(number), value);
        string_append(destination, capacity, number);
}

static const char *process_state_name(uint32_t state) {
        static const char *const names[] = {"READY", "RUN", "WAIT", "STOP"};
        return state <= USER_PROCESS_STOPPED ? names[state] : "?";
}

static void update_detail(struct monitor_state *state) {
        if (state->process_list.selected_index >= state->process_count) {
                string_copy(state->detail_text, sizeof(state->detail_text),
                            "NO PROCESS SELECTED");
                return;
        }
        const struct user_process_info *process =
            &state->processes[state->process_list.selected_index];
        string_copy(state->detail_text, sizeof(state->detail_text), "PID ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->pid);
        string_append(state->detail_text, sizeof(state->detail_text),
                      " | PPID ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->parent_pid);
        string_append(state->detail_text, sizeof(state->detail_text),
                      " | PGID ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->process_group);
        string_append(state->detail_text, sizeof(state->detail_text),
                      " | SID ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->session_id);
        string_append(state->detail_text, sizeof(state->detail_text),
                      " | PAGES ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->resident_pages);
        string_append(state->detail_text, sizeof(state->detail_text),
                      " | FDS ");
        append_number(state->detail_text, sizeof(state->detail_text),
                      process->open_descriptors);
}

static bool monitor_update(struct rose_gui_application *app, uint64_t now,
                           void *user_data) {
        (void)app;
        (void)now;
        struct monitor_state *state = user_data;
        uint64_t selected_pid = 0U;
        if (state->process_list.selected_index < state->process_count) {
                selected_pid =
                    state->processes[state->process_list.selected_index].pid;
        }
        struct user_system_info system;
        long count = rose_process_list(state->processes,
                                       USER_PROCESS_INFO_LIMIT);
        if (count < 0 || rose_system_info(&system) != 0) return false;
        state->process_count = (size_t)count;
        size_t selected = 0U;
        for (size_t index = 0U; index < state->process_count; index++) {
                const struct user_process_info *process =
                    &state->processes[index];
                char *text = state->process_text[index];
                text[0] = '\0';
                append_number(text, PROCESS_TEXT_LIMIT, process->pid);
                string_append(text, PROCESS_TEXT_LIMIT, "  ");
                string_append(text, PROCESS_TEXT_LIMIT,
                              process_state_name(process->state));
                string_append(text, PROCESS_TEXT_LIMIT, "  ");
                append_number(text, PROCESS_TEXT_LIMIT,
                              process->resident_pages);
                string_append(text, PROCESS_TEXT_LIMIT, "P  ");
                string_append(text, PROCESS_TEXT_LIMIT, process->name);
                state->process_items[index] = text;
                if (process->pid == selected_pid) selected = index;
        }
        state->process_list.items = state->process_items;
        state->process_list.item_count = state->process_count;
        state->process_list.selected_index = selected;
        string_copy(state->summary_text, sizeof(state->summary_text),
                    "MEMORY ");
        append_number(state->summary_text, sizeof(state->summary_text),
                      system.used_pages);
        string_append(state->summary_text, sizeof(state->summary_text), "/");
        append_number(state->summary_text, sizeof(state->summary_text),
                      system.total_pages);
        string_append(state->summary_text, sizeof(state->summary_text),
                      " PAGES | CONTEXT SWITCHES ");
        append_number(state->summary_text, sizeof(state->summary_text),
                      system.context_switches);
        string_append(state->summary_text, sizeof(state->summary_text),
                      " | COW ");
        append_number(state->summary_text, sizeof(state->summary_text),
                      system.copy_on_write_copies);
        update_detail(state);
        return true;
}

static void process_action(struct rose_gui_widget *widget,
                           enum rose_gui_widget_action action,
                           void *user_data) {
        struct monitor_state *state = user_data;
        if (action == ROSE_GUI_ACTION_CHANGE ||
            action == ROSE_GUI_ACTION_SELECT) {
                update_detail(state);
                if (state->app != NULL)
                        rose_gui_ui_invalidate(&state->app->ui);
        }
        (void)widget;
}

static void signal_action(struct rose_gui_widget *widget,
                          enum rose_gui_widget_action action,
                          void *user_data) {
        if (action != ROSE_GUI_ACTION_ACTIVATE) return;
        struct monitor_state *state = user_data;
        if (state->process_list.selected_index >= state->process_count) return;
        const struct user_process_info *process =
            &state->processes[state->process_list.selected_index];
        if (process->pid == (uint64_t)rose_getpid()) {
                string_copy(state->detail_text, sizeof(state->detail_text),
                            "SYSTEM MONITOR WILL NOT SIGNAL ITSELF");
        } else {
                int signal = widget == &state->kill ? USER_SIGNAL_KILL
                                                    : USER_SIGNAL_TERMINATE;
                string_copy(state->detail_text, sizeof(state->detail_text),
                            rose_kill((int64_t)process->pid, signal) == 0
                                ? "SIGNAL SENT"
                                : "SIGNAL FAILED");
        }
        rose_gui_ui_invalidate(&state->app->ui);
}

static void monitor_build_ui(struct monitor_state *state) {
        rose_gui_widget_initialize(&state->root, ROSE_GUI_WIDGET_ROOT, NULL);
        state->root.padding = 0U;
        state->root.gap = 0U;
        rose_gui_widget_initialize(&state->header, ROSE_GUI_WIDGET_STATUS_BAR,
                                   "SYSTEM MONITOR");
        rose_gui_widget_set_minimum(&state->header, 0, 38);
        rose_gui_widget_initialize(&state->body, ROSE_GUI_WIDGET_COLUMN, NULL);
        state->body.padding = 12U;
        state->body.gap = 6U;
        rose_gui_widget_set_flex(&state->body, 1U);
        rose_gui_widget_initialize(&state->summary,
                                   ROSE_GUI_WIDGET_STATUS_BAR,
                                   state->summary_text);
        rose_gui_widget_set_minimum(&state->summary, 0, 24);
        rose_gui_widget_initialize(&state->process_heading,
                                   ROSE_GUI_WIDGET_LABEL,
                                   "PID  STATE  MEMORY  COMMAND");
        rose_gui_widget_set_minimum(&state->process_heading, 0, 16);
        rose_gui_items_initialize(&state->process_list, ROSE_GUI_WIDGET_LIST,
                                  state->process_items, 0U);
        rose_gui_widget_set_flex(&state->process_list, 1U);
        state->process_list.callback = process_action;
        state->process_list.user_data = state;
        rose_gui_widget_initialize(&state->detail,
                                   ROSE_GUI_WIDGET_STATUS_BAR,
                                   state->detail_text);
        rose_gui_widget_set_minimum(&state->detail, 0, 24);
        rose_gui_widget_initialize(&state->actions, ROSE_GUI_WIDGET_ROW, NULL);
        state->actions.padding = 0U;
        state->actions.gap = 7U;
        rose_gui_widget_set_minimum(&state->actions, 0, 30);
        rose_gui_widget_initialize(&state->terminate,
                                   ROSE_GUI_WIDGET_BUTTON, "TERMINATE");
        rose_gui_widget_initialize(&state->kill, ROSE_GUI_WIDGET_BUTTON,
                                   "KILL NOW");
        rose_gui_widget_set_flex(&state->terminate, 1U);
        rose_gui_widget_set_flex(&state->kill, 1U);
        state->terminate.callback = state->kill.callback = signal_action;
        state->terminate.user_data = state->kill.user_data = state;
        rose_gui_widget_add(&state->actions, &state->terminate);
        rose_gui_widget_add(&state->actions, &state->kill);
        rose_gui_widget_initialize(&state->status,
                                   ROSE_GUI_WIDGET_STATUS_BAR,
                                   "LIVE UPDATE EVERY 500 MS");
        rose_gui_widget_set_minimum(&state->status, 0, 22);
        rose_gui_widget_add(&state->root, &state->header);
        rose_gui_widget_add(&state->root, &state->body);
        rose_gui_widget_add(&state->body, &state->summary);
        rose_gui_widget_add(&state->body, &state->process_heading);
        rose_gui_widget_add(&state->body, &state->process_list);
        rose_gui_widget_add(&state->body, &state->detail);
        rose_gui_widget_add(&state->body, &state->actions);
        rose_gui_widget_add(&state->body, &state->status);
}

int rose_gui_monitor_main(int argc, char **argv) {
        static struct monitor_state state;
        static struct rose_gui_application app;
        if (argc != 2) return 1;
        monitor_build_ui(&state);
        if (!rose_gui_application_initialize(&app, argv[1], &state.root))
                return 1;
        state.app = &app;
        app.update_interval = UINT64_C(500000000);
        app.update = monitor_update;
        app.user_data = &state;
        rose_gui_ui_focus(&app.ui, &state.process_list);
        return rose_gui_application_run(&app);
}
