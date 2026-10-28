/* crc32.h -- CRC-32 for storage records and segment headers.
 * IEEE 802.3 polynomial, reflected (0xEDB88320), init and final XOR
 * 0xFFFFFFFF: the same CRC as zlib's crc32(), so values can be checked
 * independently. Check value: store_crc32("123456789", 9) == 0xCBF43926. */
#ifndef MOLDWATCH_CRC32_H
#define MOLDWATCH_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 of len bytes at buf. buf may be NULL when len is 0. */
uint32_t store_crc32(const void *buf, size_t len);

#endif
