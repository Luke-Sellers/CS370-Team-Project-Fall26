#include "bme280.h"
#include "i2c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int bme280_init(int fd, struct bme280_dev *dev, uint8_t addr)
{
    uint8_t id = 0;

    dev->addr = addr;
    if (i2c_read_reg(fd, addr, BME280_REG_CHIP_ID, &id, 1) < 0) {
        fprintf(stderr, "bme280: no response at 0x%02x: %s\n", addr, strerror(errno));
        return -1;
    }
    if (id != BME280_CHIP_ID) {
        fprintf(stderr, "bme280: chip ID 0x%02x, expected 0x%02x%s\n", id, BME280_CHIP_ID,
                id == 0x58 ? " (this is a BMP280: no humidity sensor)" : "");
        errno = ENODEV;
        return -1;
    }

    /* TODO(next step): read calibration 0x88..0xA1 and 0xE1..0xE7,
     * write ctrl_hum (0xF2) BEFORE ctrl_meas (0xF4). */
    return 0;
}

int bme280_read(int fd, const struct bme280_dev *dev, int32_t *temp_centi, int32_t *rh_centi)
{
    (void)fd;
    (void)dev;
    (void)temp_centi;
    (void)rh_centi;
    /* TODO(next step): trigger a measurement, read raw 0xF7..0xFE,
     * apply the Bosch integer compensation formulas. */
    errno = ENOSYS;
    return -1;
}
