/* bme280.h -- Bosch BME280: air temperature and humidity. */
#ifndef MOLDWATCH_BME280_H
#define MOLDWATCH_BME280_H

#include <stdint.h>

#define BME280_ADDR        0x76 /* 0x77 if your board's SDO pin is pulled high */
#define BME280_REG_CHIP_ID 0xD0
#define BME280_CHIP_ID     0x60 /* 0x58 means it is really a BMP280: no humidity */

struct bme280_dev {
    uint8_t addr;
    /* TODO(next step): calibration constants dig_T1..T3 and dig_H1..H6. */
};

/* Check the chip ID, then (next step) read calibration and configure the
 * sensor. Returns 0, or -1 with errno set and a message on stderr. */
int bme280_init(int fd, struct bme280_dev *dev, uint8_t addr);

/* One reading: temperature in 0.01 C, humidity in 0.01 %RH.
 * Returns 0, or -1 with errno set. */
int bme280_read(int fd, const struct bme280_dev *dev, int32_t *temp_centi, int32_t *rh_centi);

#endif
