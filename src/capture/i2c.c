#include "i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>

int i2c_open(const char *bus)
{
    /* O_CLOEXEC: a supervisor that execs a new child must not leak this fd. */
    return open(bus, O_RDWR | O_CLOEXEC);
}

int i2c_read_reg(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    struct i2c_msg msgs[2] = {
        { .addr = addr, .flags = 0,        .len = 1,   .buf = &reg },
        { .addr = addr, .flags = I2C_M_RD, .len = len, .buf = buf  },
    };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };

    int n = ioctl(fd, I2C_RDWR, &xfer);
    if (n == 2)
        return 0;
    if (n >= 0)
        errno = EIO; /* the kernel ran only part of the transaction */
    return -1;
}

int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t out[2] = { reg, val };
    struct i2c_msg msg = { .addr = addr, .flags = 0, .len = 2, .buf = out };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = &msg, .nmsgs = 1 };

    int n = ioctl(fd, I2C_RDWR, &xfer);
    if (n == 1)
        return 0;
    if (n >= 0)
        errno = EIO;
    return -1;
}
