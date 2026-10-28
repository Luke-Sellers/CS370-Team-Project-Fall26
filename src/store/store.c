/* store.c -- per-sensor segment log; format and rules in docs/store-plan.md.
 *
 * This file currently covers open/close, the per-sensor lock, segment naming
 * and creation, the header check, and the per-boot segment rule. Appending,
 * rotation and tail recovery come in later steps.
 */
#define _DEFAULT_SOURCE /* flock(2); -D_POSIX_C_SOURCE alone hides it */

#include "store.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "log.h"

#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#define NAME_LEN     64

struct store {
    int dirfd;
    int lockfd;
    int fd;               /* live segment, O_APPEND */
    enum sensor_id sensor;
    uint32_t counter;     /* live segment */
    uint32_t max_any;     /* highest counter seen, live or quarantined */
    uint64_t records;     /* records in the live segment */
    uint8_t boot_id[16];  /* this boot; zeros if unknown */
};

enum name_kind { NAME_OTHER, NAME_LIVE, NAME_TMP, NAME_CORRUPT };

static const char *sensor_name(enum sensor_id s)
{
    switch (s) {
    case SENSOR_BME280:
        return "bme280";
    case SENSOR_MLX90614:
        return "mlx90614";
    }
    return NULL;
}

static const char *status_text(int rc)
{
    switch (rc) {
    case STORE_EBADHDR:
        return "bad header";
    case STORE_EVERSION:
        return "unsupported format version";
    case STORE_ESENSOR:
        return "header is for another sensor";
    default:
        return "error";
    }
}

/* Log a failed call with its errno, keep errno for the caller, return status. */
static int fail(int status, const char *what, const char *name)
{
    int e = errno;
    log_msg("%s %s: %s", what, name, strerror(e));
    errno = e;
    return status;
}

static int seg_name(char *buf, size_t n, enum sensor_id s, uint32_t counter, const char *suffix)
{
    int len = snprintf(buf, n, "%s.%06" PRIu32 ".mwlog%s", sensor_name(s), counter, suffix);
    if (len < 0 || (size_t)len >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int all_digits(const char *p)
{
    if (*p == '\0')
        return 0;
    for (; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

/* Classify "<sensor>.<counter>.mwlog[.tmp|.corrupt-<n>]". The counter must be
 * in canonical %06u form, so "bme280.1.mwlog" is not segment 1. */
static enum name_kind parse_name(const char *name, const char *sensor, uint32_t *counter)
{
    size_t sl = strlen(sensor);
    const char *p, *end;
    unsigned long long v = 0;
    char canon[24];
    int n;

    if (strncmp(name, sensor, sl) != 0 || name[sl] != '.')
        return NAME_OTHER;
    p = name + sl + 1;
    for (end = p; *end >= '0' && *end <= '9'; end++) {
        v = v * 10 + (unsigned long long)(*end - '0');
        if (v > UINT32_MAX)
            return NAME_OTHER;
    }
    if (end == p)
        return NAME_OTHER;
    n = snprintf(canon, sizeof canon, "%06llu", v);
    if (n < 0 || (size_t)n != (size_t)(end - p) || strncmp(canon, p, (size_t)n) != 0)
        return NAME_OTHER;
    if (strncmp(end, ".mwlog", 6) != 0)
        return NAME_OTHER;
    end += 6;
    *counter = (uint32_t)v;
    if (*end == '\0')
        return NAME_LIVE;
    if (strcmp(end, ".tmp") == 0)
        return NAME_TMP;
    if (strncmp(end, ".corrupt-", 9) == 0 && all_digits(end + 9))
        return NAME_CORRUPT;
    return NAME_OTHER;
}

static void header_fill(struct store_header *h, enum sensor_id s, uint32_t counter,
                        const uint8_t boot_id[16])
{
    memset(h, 0, sizeof *h);
    h->magic = STORE_MAGIC;
    h->version = STORE_VERSION;
    h->sensor = (uint8_t)s;
    h->record_size = (uint8_t)sizeof(struct record);
    h->counter = counter;
    memcpy(h->boot_id, boot_id, sizeof h->boot_id);
    h->crc32 = store_crc32(h, offsetof(struct store_header, crc32));
}

int store_header_check(const struct store_header *h, enum sensor_id sensor, uint32_t counter)
{
    if (h->magic != STORE_MAGIC)
        return STORE_EBADHDR;
    /* Before the CRC: a newer format may lay its header out differently and
     * must be refused untouched, not judged corrupt by version-1 rules. */
    if (h->version != STORE_VERSION)
        return STORE_EVERSION;
    if (h->crc32 != store_crc32(h, offsetof(struct store_header, crc32)))
        return STORE_EBADHDR;
    if (h->record_size != sizeof(struct record))
        return STORE_EBADHDR;
    if (h->sensor != (uint8_t)sensor)
        return STORE_ESENSOR;
    if (h->counter != counter)
        return STORE_EBADHDR;
    return STORE_OK;
}

static int boot_known(const uint8_t id[16])
{
    for (int i = 0; i < 16; i++)
        if (id[i] != 0)
            return 1;
    return 0;
}

/* The kernel's per-boot UUID as 16 bytes; zeros (logged) if unavailable, which
 * makes every open start a new segment rather than guess. */
static void read_boot_id(uint8_t out[16])
{
    char text[64];
    int n = 0;
    FILE *f = fopen(BOOT_ID_PATH, "re");

    memset(out, 0, 16);
    if (f == NULL) {
        fail(0, "open", BOOT_ID_PATH);
        return;
    }
    if (fgets(text, sizeof text, f) == NULL) {
        fail(0, "read", BOOT_ID_PATH);
        goto out;
    }
    for (const char *c = text; *c != '\0' && *c != '\n'; c++) {
        int v;
        if (*c == '-')
            continue;
        if (*c >= '0' && *c <= '9')
            v = *c - '0';
        else if (*c >= 'a' && *c <= 'f')
            v = *c - 'a' + 10;
        else
            v = -1;
        if (v < 0 || n >= 32) {
            n = -1;
            break;
        }
        out[n / 2] = (uint8_t)(n % 2 == 0 ? v << 4 : out[n / 2] | v);
        n++;
    }
    if (n != 32) {
        memset(out, 0, 16);
        log_msg("%s: unparseable; boot id unknown", BOOT_ID_PATH);
    }
out:
    if (fclose(f) != 0)
        fail(0, "close", BOOT_ID_PATH);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Read exactly len bytes at off; a short file is EIO. */
static int pread_all(int fd, void *buf, size_t len, off_t off)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t r = pread(fd, p, len, off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {
            errno = EIO;
            return -1;
        }
        p += r;
        off += r;
        len -= (size_t)r;
    }
    return 0;
}

/* Find this sensor's highest live counter and highest counter of any kind,
 * and remove its stale .tmp files (a crash during create; we hold the lock,
 * so none can be in progress). */
static int scan_dir(struct store *st, uint32_t *max_live, int *have_live)
{
    const char *sname = sensor_name(st->sensor);
    struct dirent *e;
    DIR *d;
    int fd, rc = STORE_OK;

    fd = openat(st->dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return fail(STORE_EIO, "open", "segment dir");
    d = fdopendir(fd);
    if (d == NULL) {
        fail(STORE_EIO, "fdopendir", "segment dir");
        if (close(fd) < 0)
            fail(0, "close", "segment dir");
        return STORE_EIO;
    }

    *have_live = 0;
    *max_live = 0;
    st->max_any = 0;
    for (;;) {
        uint32_t c;
        errno = 0;
        e = readdir(d);
        if (e == NULL) {
            if (errno != 0)
                rc = fail(STORE_EIO, "readdir", "segment dir");
            break;
        }
        switch (parse_name(e->d_name, sname, &c)) {
        case NAME_LIVE:
            if (!*have_live || c > *max_live)
                *max_live = c;
            *have_live = 1;
            if (c > st->max_any)
                st->max_any = c;
            break;
        case NAME_CORRUPT:
            if (c > st->max_any)
                st->max_any = c;
            break;
        case NAME_TMP:
            if (unlinkat(st->dirfd, e->d_name, 0) < 0)
                fail(0, "unlink stale", e->d_name);
            else
                log_msg("removed stale %s", e->d_name);
            break;
        case NAME_OTHER:
            break;
        }
    }
    if (closedir(d) < 0)
        rc = fail(STORE_EIO, "closedir", "segment dir");
    return rc;
}

/* Create segment `counter` with a complete header, durably:
 * write <seg>.tmp, fdatasync, rename into place, fsync the directory.
 * A crash leaves either no segment or a whole one, never a torn header.
 * On success st->fd is the new segment, open for appending. */
static int create_segment(struct store *st, uint32_t counter)
{
    char seg[NAME_LEN], tmp[NAME_LEN];
    struct store_header h;
    int fd = -1, made_tmp = 0, rc = STORE_EIO, e = 0;

    if (seg_name(seg, sizeof seg, st->sensor, counter, "") < 0 ||
        seg_name(tmp, sizeof tmp, st->sensor, counter, ".tmp") < 0)
        return fail(STORE_EIO, "name", sensor_name(st->sensor));
    header_fill(&h, st->sensor, counter, st->boot_id);

    fd = openat(st->dirfd, tmp, O_RDWR | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail(0, "create", tmp);
        goto out;
    }
    made_tmp = 1;
    if (write_all(fd, &h, sizeof h) < 0) {
        fail(0, "write", tmp);
        goto out;
    }
    if (fdatasync(fd) < 0) {
        fail(0, "fdatasync", tmp);
        goto out;
    }
    if (renameat(st->dirfd, tmp, st->dirfd, seg) < 0) {
        fail(0, "rename", tmp);
        goto out;
    }
    made_tmp = 0;
    if (fsync(st->dirfd) < 0) {
        fail(0, "fsync dir for", seg);
        goto out;
    }

    st->fd = fd;
    fd = -1;
    st->counter = counter;
    st->records = 0;
    if (counter > st->max_any)
        st->max_any = counter;
    log_msg("created %s", seg);
    rc = STORE_OK;
out:
    e = errno;
    if (fd >= 0 && close(fd) < 0)
        fail(0, "close", tmp);
    if (made_tmp && unlinkat(st->dirfd, tmp, 0) < 0)
        fail(0, "unlink", tmp);
    errno = e;
    return rc;
}

static int next_counter(const struct store *st, uint32_t *out)
{
    if (st->max_any == UINT32_MAX) {
        errno = EOVERFLOW;
        return fail(STORE_EIO, "next counter for", sensor_name(st->sensor));
    }
    *out = st->max_any + 1;
    return STORE_OK;
}

/* Close what is open and free. Returns STORE_EIO if any close failed. */
static int release(struct store *st)
{
    int rc = STORE_OK;

    if (st == NULL)
        return STORE_OK;
    if (st->fd >= 0 && close(st->fd) < 0)
        rc = fail(STORE_EIO, "close", "segment");
    if (st->lockfd >= 0 && close(st->lockfd) < 0)
        rc = fail(STORE_EIO, "close", "lock");
    if (st->dirfd >= 0 && close(st->dirfd) < 0)
        rc = fail(STORE_EIO, "close", "segment dir");
    free(st);
    return rc;
}

int store_open(const char *dir, enum sensor_id sensor, struct store **out,
               struct store_open_info *info)
{
    struct store_open_info scratch;
    struct store *st = NULL;
    const char *sname = sensor_name(sensor);
    char name[NAME_LEN];
    struct store_header h;
    struct stat sb;
    uint32_t max_live = 0, next;
    int have_live = 0, fd = -1, rc = STORE_EIO, e;

    if (out != NULL)
        *out = NULL;
    if (info == NULL)
        info = &scratch;
    memset(info, 0, sizeof *info);
    if (dir == NULL || out == NULL || sname == NULL) {
        errno = EINVAL;
        return STORE_EINVAL;
    }

    st = calloc(1, sizeof *st);
    if (st == NULL)
        return fail(STORE_EIO, "calloc for", sname);
    st->dirfd = st->lockfd = st->fd = -1;
    st->sensor = sensor;

    st->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (st->dirfd < 0) {
        fail(0, "open dir", dir);
        goto out;
    }

    /* One writer per sensor. flock is tied to this open file, so it goes
     * away with the process: a crashed writer never leaves it held. */
    snprintf(name, sizeof name, "%s.lock", sname);
    st->lockfd = openat(st->dirfd, name, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (st->lockfd < 0) {
        fail(0, "open", name);
        goto out;
    }
    if (flock(st->lockfd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK)
            rc = STORE_EBUSY;
        fail(0, "lock", name);
        goto out;
    }

    read_boot_id(st->boot_id);
    rc = scan_dir(st, &max_live, &have_live);
    if (rc != STORE_OK)
        goto out;
    rc = STORE_EIO;

    if (have_live) {
        if (seg_name(name, sizeof name, sensor, max_live, "") < 0) {
            fail(0, "name", sname);
            goto out;
        }
        fd = openat(st->dirfd, name, O_RDWR | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            fail(0, "open", name);
            goto out;
        }
        if (fstat(fd, &sb) < 0) {
            fail(0, "fstat", name);
            goto out;
        }
        if (sb.st_size < (off_t)sizeof h) {
            rc = STORE_EBADHDR;
            log_msg("refusing %s: %lld bytes, shorter than a header", name,
                    (long long)sb.st_size);
            goto out;
        }
        if (pread_all(fd, &h, sizeof h, 0) < 0) {
            fail(0, "read header", name);
            goto out;
        }
        rc = store_header_check(&h, sensor, max_live);
        if (rc != STORE_OK) {
            log_msg("refusing %s: %s", name, status_text(rc));
            goto out;
        }
        rc = STORE_EIO;

        if (boot_known(st->boot_id) && memcmp(h.boot_id, st->boot_id, 16) == 0) {
            /* Same boot: keep appending here. The record count trusts the
             * file size until tail recovery (step 4) checks the records. */
            st->fd = fd;
            fd = -1;
            st->counter = max_live;
            st->records = (uint64_t)(sb.st_size - (off_t)sizeof h) / sizeof(struct record);
            goto done;
        }
        /* Another boot: that segment stays sealed as it is. */
        info->new_boot = 1;
        if (close(fd) < 0) {
            fd = -1;
            fail(0, "close", name);
            goto out;
        }
        fd = -1;
    }

    rc = next_counter(st, &next);
    if (rc != STORE_OK)
        goto out;
    rc = create_segment(st, next);
    if (rc != STORE_OK)
        goto out;
    info->created = 1;

done:
    info->counter = st->counter;
    info->records = st->records;
    *out = st;
    return STORE_OK;

out:
    e = errno;
    if (fd >= 0 && close(fd) < 0)
        fail(0, "close", name);
    release(st);
    errno = e;
    return rc;
}

int store_close(struct store *st)
{
    return release(st);
}
