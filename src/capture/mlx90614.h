/* mlx90614.h -- Melexis MLX90614: non-contact (infrared) temperature. */
#ifndef MOLDWATCH_MLX90614_H
#define MOLDWATCH_MLX90614_H

#include <stdint.h>

#define MLX90614_ADDR     0x5A
#define MLX90614_REG_TA   0x06 /* the sensor's own temperature */
#define MLX90614_REG_TOBJ 0x07 /* the object it points at: the ceiling */

struct mlx90614_dev {
    uint8_t addr;
};

/* Returns 0, or -1 with errno set and a message on stderr. */
int mlx90614_init(int fd, struct mlx90614_dev *dev, uint8_t addr);

/* One reading: object and ambient temperature, both in 0.01 C.
 * Returns 0, or -1 with errno set. */
int mlx90614_read(int fd, const struct mlx90614_dev *dev, int32_t *obj_centi, int32_t *amb_centi);

#endif
