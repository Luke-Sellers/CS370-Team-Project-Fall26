#include "fake.h"

void fake_fill(struct record *r)
{
    /* A slow triangle wave, 40 samples per cycle: 0, 1 .. 20 .. 1, 0. */
    uint32_t p = r->seq % 40u;
    int32_t tri = (int32_t)(p < 20u ? p : 40u - p);

    r->flags |= REC_SYNTHETIC;
    switch (r->sensor) {
    case SENSOR_BME280:
        r->a = 2150 + tri * 5;   /* 21.50 .. 22.50 C   */
        r->b = 5500 + tri * 150; /* 55.00 .. 85.00 %RH */
        break;
    case SENSOR_MLX90614:
        r->a = 1900 + tri * 3;   /* ceiling 19.00 .. 19.60 C */
        r->b = 2200;             /* sensor body 22.00 C      */
        break;
    default:
        break;
    }
}
