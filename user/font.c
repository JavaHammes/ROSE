#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/font.h"
#include "rose/syscall.h"
#include "user_abi.h"

static uint8_t glyphs[ROSE_FONT_GLYPH_COUNT][ROSE_FONT_WIDTH];
static bool loaded;

static int hex_value(char character) {
        if (character >= '0' && character <= '9') {
                return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
        }
        return -1;
}

bool rose_font_load(void) {
        if (loaded) return true;

        long descriptor = rose_open("/share/font5x7.hex", USER_OPEN_READ);
        if (descriptor < 0) return false;

        size_t byte = 0U;
        int high_nibble = -1;
        char input[128];
        while (byte < (size_t)ROSE_FONT_GLYPH_COUNT * ROSE_FONT_WIDTH) {
                long count = rose_read((int)descriptor, input, sizeof(input));
                if (count <= 0) break;
                for (long index = 0; index < count; index++) {
                        int value = hex_value(input[index]);
                        if (value < 0) continue;
                        if (high_nibble < 0) {
                                high_nibble = value;
                        } else {
                                glyphs[byte / ROSE_FONT_WIDTH]
                                      [byte % ROSE_FONT_WIDTH] =
                                    (uint8_t)((high_nibble << 4) | value);
                                high_nibble = -1;
                                byte++;
                                if (byte == (size_t)ROSE_FONT_GLYPH_COUNT *
                                                ROSE_FONT_WIDTH) {
                                        break;
                                }
                        }
                }
        }
        (void)rose_close((int)descriptor);
        loaded = byte == (size_t)ROSE_FONT_GLYPH_COUNT * ROSE_FONT_WIDTH &&
                 high_nibble < 0;
        return loaded;
}

const uint8_t *rose_font_glyph(char character) {
        if (!loaded && !rose_font_load()) return NULL;
        if (character < ROSE_FONT_FIRST_CHARACTER ||
            character > ROSE_FONT_LAST_CHARACTER) {
                character = '?';
        }
        return glyphs[(uint8_t)character - ROSE_FONT_FIRST_CHARACTER];
}
