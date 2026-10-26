/* test_record -- pins down the record layout that capture and storage
 * share, and checks that fake data is always labeled synthetic. */
#include <stddef.h>
#include <stdio.h>

#include "fake.h"
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
    /* Layout: storage depends on these exact offsets. */
    CHECK(sizeof(struct record) == 32);
    CHECK(offsetof(struct record, magic) == 0);
    CHECK(offsetof(struct record, sensor) == 2);
    CHECK(offsetof(struct record, flags) == 3);
    CHECK(offsetof(struct record, seq) == 4);
    CHECK(offsetof(struct record, t_ns) == 8);
    CHECK(offsetof(struct record, a) == 16);
    CHECK(offsetof(struct record, b) == 20);
    CHECK(offsetof(struct record, reserved) == 24);
    CHECK(offsetof(struct record, crc32) == 28);

    /* Every fake record is flagged, for both sensors, across a full cycle. */
    for (uint32_t seq = 1; seq <= 80; seq++) {
        struct record bme = { .sensor = SENSOR_BME280, .seq = seq };
        struct record mlx = { .sensor = SENSOR_MLX90614, .seq = seq };
        fake_fill(&bme);
        fake_fill(&mlx);
        CHECK(bme.flags & REC_SYNTHETIC);
        CHECK(mlx.flags & REC_SYNTHETIC);
        CHECK(bme.a >= 2150 && bme.a <= 2250);
        CHECK(bme.b >= 5500 && bme.b <= 8500);
        CHECK(mlx.a >= 1900 && mlx.a <= 1960);
    }

    if (failures == 0)
        printf("test_record: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
