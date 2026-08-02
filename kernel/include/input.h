#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

#include "user_abi.h"

void input_event_push(const struct user_input_event *event);
bool input_event_pop(struct user_input_event *event);
bool input_event_available(void);
void input_event_clear(void);
void input_set_console_captured(bool captured);
bool input_console_captured(void);

#endif
