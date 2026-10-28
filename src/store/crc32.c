#include "crc32.h"

uint32_t store_crc32(const void *buf, size_t len)
{
    /* Bitwise, no table: shift each input bit through the register, XORing
     * in the reflected polynomial whenever a 1 falls off the low end.
     * 8 iterations per byte is plenty for 32-byte records. */
    const unsigned char *p = buf;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}
