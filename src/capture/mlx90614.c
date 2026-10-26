#include "mlx90614.h"

#include <errno.h>

int mlx90614_init(int fd, struct mlx90614_dev *dev, uint8_t addr)
{
    (void)fd;
    dev->addr = addr;
    /* TODO(next step): read MLX90614_REG_TA once to confirm the sensor
     * answers, and check the PEC byte. */
    return 0;
}

int mlx90614_read(int fd, const struct mlx90614_dev *dev, int32_t *obj_centi, int32_t *amb_centi)
{
    (void)fd;
    (void)dev;
    (void)obj_centi;
    (void)amb_centi;
    /* TODO(next step): read 3 bytes (LSB, MSB, PEC) from 0x07 and 0x06,
     * verify PEC, convert raw * 0.02 K to 0.01 C as (raw * 2 - 27315). */
    errno = ENOSYS;
    return -1;
}
