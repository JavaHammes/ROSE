#ifndef ROSE_USER_FONT_H
#define ROSE_USER_FONT_H

#include <stdbool.h>
#include <stdint.h>

enum {
        ROSE_FONT_FIRST_CHARACTER = 32,
        ROSE_FONT_LAST_CHARACTER = 126,
        ROSE_FONT_GLYPH_COUNT = 95,
        ROSE_FONT_WIDTH = 5,
        ROSE_FONT_HEIGHT = 7,
};

/* Load the shared printable-ASCII bitmap resource installed in userspace. */
bool rose_font_load(void);
const uint8_t *rose_font_glyph(char character);

#endif
