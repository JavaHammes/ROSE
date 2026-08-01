#ifndef GRAPHICS_CONSOLE_H
#define GRAPHICS_CONSOLE_H

#include <stdbool.h>

void graphics_console_init(void);
void graphics_console_suspend(void);
bool graphics_console_available(void);
void graphics_console_putc(char character);

#endif
