/* record.h -- the one shared contract between capture (Ricky) and
 * storage (Luke). Change it only when both of you agree.
 *
 * One sensor reading, fixed at 32 bytes so storage can recover a log
 * by record boundaries after a power cut. Values are scaled integers,
 * so nothing on disk is a float. Byte order is the host's (little-endian
 * on both the Pi and x86).
 */
#ifndef MOLDWATCH_RECORD_H
#define MOLDWATCH_RECORD_H

#include <stdint.h>

#define RECORD_MAGIC 0x4D57u /* "MW" */

enum sensor_id {
    SENSOR_BME280   = 1,
    SENSOR_MLX90614 = 2,
};

enum record_flags {
    REC_SYNTHETIC = 1u << 0, /* fake or replayed data; must stay labeled everywhere */
    REC_READ_ERR  = 1u << 1, /* the sensor read failed; a and b are not valid */
};

struct record {
    uint16_t magic;    /* RECORD_MAGIC */
    uint8_t  sensor;   /* enum sensor_id */
    uint8_t  flags;    /* enum record_flags, OR'd together */
    uint32_t seq;      /* per-process sequence number, starts at 1 */
    int64_t  t_ns;     /* CLOCK_REALTIME, nanoseconds since the epoch */
    int32_t  a;        /* BME280: air temp, 0.01 C   | MLX90614: object (ceiling) temp, 0.01 C */
    int32_t  b;        /* BME280: humidity, 0.01 %RH | MLX90614: sensor's own temp, 0.01 C */
    uint32_t reserved; /* always 0 */
    uint32_t crc32;    /* storage fills this over bytes [0, 28); capture leaves it 0 */
};

_Static_assert(sizeof(struct record) == 32, "struct record must stay 32 bytes");

#endif /* MOLDWATCH_RECORD_H */
