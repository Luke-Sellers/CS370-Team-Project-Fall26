/* test_store_crc -- pins storage's CRC-32 (IEEE, reflected 0xEDB88320) to
 * published check values. Expected constants were computed independently
 * with zlib.crc32, not with this implementation. */
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "record.h"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

int main(void)
{
    /* C1: the standard CRC-32 check value. */
    CHECK(store_crc32("123456789", 9) == 0xCBF43926u);

    /* C2: empty input; a NULL pointer with length 0 must not be touched. */
    CHECK(store_crc32("", 0) == 0x00000000u);
    CHECK(store_crc32(NULL, 0) == 0x00000000u);

    /* More known vectors: one byte, all-zero and all-one blocks, a sentence. */
    CHECK(store_crc32("a", 1) == 0xE8B7BE43u);
    unsigned char zeros[32], ones[32];
    memset(zeros, 0x00, sizeof zeros);
    memset(ones, 0xFF, sizeof ones);
    CHECK(store_crc32(zeros, sizeof zeros) == 0x190A55ADu); /* C3 */
    CHECK(store_crc32(ones, sizeof ones) == 0xFF6CAB0Bu);
    const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK(store_crc32(fox, strlen(fox)) == 0x414FA339u);

    /* Over exactly the bytes storage covers: record bytes [0, 28). */
    struct record r = {
        .magic = RECORD_MAGIC,
        .sensor = SENSOR_BME280,
        .flags = REC_SYNTHETIC,
        .seq = 7,
        .t_ns = 1700000000123456789LL,
        .a = 2150,
        .b = 5500,
    };
    CHECK(store_crc32(&r, 28) == 0xB25CE650u);

    /* Input is read-only, and alignment does not matter. */
    unsigned char buf[1 + 9];
    memcpy(buf + 1, "123456789", 9);
    CHECK(store_crc32(buf + 1, 9) == 0xCBF43926u);
    CHECK(memcmp(buf + 1, "123456789", 9) == 0);

    /* A single flipped bit changes the result. */
    struct record flipped = r;
    flipped.a ^= 1;
    CHECK(store_crc32(&flipped, 28) != store_crc32(&r, 28));

    if (failures == 0)
        printf("test_store_crc: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
