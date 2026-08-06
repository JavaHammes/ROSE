#include <assert.h>
#include <stdint.h>

#include "rose/network.h"

int main(void) {
        uint32_t address = 0U;
        char text[16];

        assert(rose_ipv4_parse("10.0.2.15", &address));
        assert(address == UINT32_C(0x0a00020f));
        rose_ipv4_format(address, text);
        assert(text[0] == '1' && text[1] == '0' && text[2] == '.' &&
               text[3] == '0' && text[4] == '.' && text[5] == '2' &&
               text[6] == '.' && text[7] == '1' && text[8] == '5' &&
               text[9] == '\0');
        assert(rose_ipv4_parse("0.0.0.0", &address) && address == 0U);
        assert(rose_ipv4_parse("255.255.255.255", &address) &&
               address == UINT32_MAX);
        assert(!rose_ipv4_parse("256.1.1.1", &address));
        assert(!rose_ipv4_parse("1.2.3", &address));
        assert(!rose_ipv4_parse("1..2.3", &address));
        assert(!rose_ipv4_parse("1.2.3.4x", &address));

        static const uint8_t checksum_sample[] = {
            0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00, 0x40, 0x01,
            0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7,
        };
        assert(rose_internet_checksum(checksum_sample,
                                      sizeof(checksum_sample)) == 0xb890U);
        return 0;
}
