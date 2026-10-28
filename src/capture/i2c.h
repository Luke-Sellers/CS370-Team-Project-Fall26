/* i2c.h -- the only code in capture that touches hardware. */
#ifndef MOLDWATCH_I2C_H
#define MOLDWATCH_I2C_H

#include <stdint.h>

/* Open an I2C bus such as "/dev/i2c-1". Returns an fd, or -1 with errno set. */
int i2c_open(const char *bus);

/* Read len bytes starting at register reg of the device at addr.
 * Sent as one combined transaction (repeated start), which the MLX90614
 * requires. Returns 0, or -1 with errno set. */
int i2c_read_reg(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);

/* Write one byte to register reg. Returns 0, or -1 with errno set. */
int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val);

#endif
