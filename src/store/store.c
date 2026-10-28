/* store.c -- per-sensor segment log; format and rules in docs/store-plan.md.
 *
 * This file covers open/close, the per-sensor lock, segment naming and
 * creation, the header check, recovery of the latest segment (torn tail
 * truncated, corruption quarantined), the per-boot segment rule, and
 * appending with rollback and rotation.
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

#ifndef BOOT_ID_PATH /* tests point this at a file they control */
#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#define NAME_LEN     64

struct store {
    int dirfd;
    int lockfd;
    int fd;               /* live segment, O_APPEND */
    enum sensor_id sensor;
    uint32_t counter;     /* live segment */
    uint32_t max_any;     /* highest counter seen, live or quarantined */
    uint64_t records;     /* records in the live segment */
    off_t size;           /* live segment's size after the last good append */
    int failed;           /* an append failed: refuse all further appends */
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

/* A whole string of digits that fits in 32 bits. */
static int parse_u32(const char *p, uint32_t *out)
{
    unsigned long long v = 0;

    if (*p == '\0')
        return 0;
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        v = v * 10 + (unsigned long long)(*p - '0');
        if (v > UINT32_MAX)
            return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* Classify "<sensor>.<counter>.mwlog[.tmp|.corrupt-<n>]". The counter must be
 * in canonical %06u form, so "bme280.1.mwlog" is not segment 1. For a
 * quarantined name, *qnum (if not NULL) gets n. */
static enum name_kind parse_name(const char *name, const char *sensor, uint32_t *counter,
                                 uint32_t *qnum)
{
    uint32_t q;

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
    if (strncmp(end, ".corrupt-", 9) == 0 && parse_u32(end + 9, &q)) {
        if (qnum != NULL)
            *qnum = q;
        return NAME_CORRUPT;
    }
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

int store_record_check(const struct record *r, enum sensor_id sensor)
{
    if (r->magic != RECORD_MAGIC || r->sensor != (uint8_t)sensor)
        return STORE_ECORRUPT;
    if (r->crc32 != store_crc32(r, offsetof(struct record, crc32)))
        return STORE_ECORRUPT;
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
        if (ferror(f))
            fail(0, "read", BOOT_ID_PATH);
        else
            log_msg("%s: empty; boot id unknown", BOOT_ID_PATH);
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
        switch (parse_name(e->d_name, sname, &c, NULL)) {
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
    st->size = (off_t)sizeof h;
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

/* Check the n whole records after the header, in 4 KiB reads. With an
 * fdatasync after every append, at most one write is ever in flight, so the
 * only damage a crash can leave is a partial record or one bad *last* record.
 * STORE_OK: *keep is n, or n-1 when only the last record is bad.
 * STORE_ECORRUPT: an earlier record is bad; *keep is its index.
 * *last is the last valid record before *keep (zeroed if none). */
static int scan_records(int fd, enum sensor_id sensor, uint64_t n, uint64_t *keep,
                        struct record *last)
{
    struct record buf[128];
    uint64_t i = 0;

    memset(last, 0, sizeof *last);
    while (i < n) {
        size_t k = (n - i < 128) ? (size_t)(n - i) : 128;
        off_t off = (off_t)(sizeof(struct store_header) + i * sizeof(struct record));
        if (pread_all(fd, buf, k * sizeof buf[0], off) < 0)
            return STORE_EIO;
        for (size_t j = 0; j < k; j++, i++) {
            if (store_record_check(&buf[j], sensor) != STORE_OK) {
                *keep = i;
                return i == n - 1 ? STORE_OK : STORE_ECORRUPT;
            }
            *last = buf[j];
        }
    }
    *keep = n;
    return STORE_OK;
}

/* Rename segment `counter` to <seg>.corrupt-<k>, the smallest free k, and
 * make the rename durable. Every byte is kept; nothing is truncated. The
 * per-sensor lock makes check-then-rename race-free. */
static int quarantine(struct store *st, uint32_t counter, const char *why, uint64_t off)
{
    char seg[NAME_LEN], dst[NAME_LEN + 32];
    struct stat sb;

    if (seg_name(seg, sizeof seg, st->sensor, counter, "") < 0)
        return fail(STORE_EIO, "name", sensor_name(st->sensor));
    for (unsigned k = 1; k <= 1000; k++) {
        snprintf(dst, sizeof dst, "%s.corrupt-%u", seg, k);
        if (fstatat(st->dirfd, dst, &sb, AT_SYMLINK_NOFOLLOW) == 0)
            continue;
        if (errno != ENOENT)
            return fail(STORE_EIO, "stat", dst);
        if (renameat(st->dirfd, seg, st->dirfd, dst) < 0)
            return fail(STORE_EIO, "rename", seg);
        if (fsync(st->dirfd) < 0)
            return fail(STORE_EIO, "fsync dir for", dst);
        log_msg("QUARANTINED %s as %s: %s at offset %" PRIu64 "; every byte kept",
                seg, dst, why, off);
        return STORE_OK;
    }
    errno = EEXIST;
    return fail(STORE_EIO, "no free quarantine name for", seg);
}

/* Recover the latest segment, `counter`. Torn tail: truncated. Bad header or
 * a bad record before the last: quarantined. Another version or sensor:
 * refused, untouched. Sets *reuse when appends should continue in it (this
 * boot, not full); otherwise the caller starts a new segment. */
static int recover_latest(struct store *st, uint32_t counter, struct store_open_info *info,
                          int *reuse)
{
    char name[NAME_LEN];
    struct store_header h;
    struct record last;
    struct stat sb;
    uint64_t n, keep = 0, bad = 0;
    off_t keep_size;
    const char *why;
    int fd = -1, rc = STORE_EIO, e;

    *reuse = 0;
    if (seg_name(name, sizeof name, st->sensor, counter, "") < 0)
        return fail(STORE_EIO, "name", sensor_name(st->sensor));
    fd = openat(st->dirfd, name, O_RDWR | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return fail(STORE_EIO, "open", name);
    if (fstat(fd, &sb) < 0) {
        fail(0, "fstat", name);
        goto out;
    }

    if (sb.st_size < (off_t)sizeof h) {
        why = "shorter than a header";
        goto corrupt;
    }
    if (pread_all(fd, &h, sizeof h, 0) < 0) {
        fail(0, "read header", name);
        goto out;
    }
    rc = store_header_check(&h, st->sensor, counter);
    if (rc == STORE_EVERSION || rc == STORE_ESENSOR) {
        log_msg("refusing %s: %s", name, status_text(rc));
        goto out;
    }
    if (rc == STORE_EBADHDR) {
        why = "bad header";
        goto corrupt;
    }
    rc = STORE_EIO;

    n = (uint64_t)(sb.st_size - (off_t)sizeof h) / sizeof(struct record);
    rc = scan_records(fd, st->sensor, n, &keep, &last);
    info->last_seq = last.seq;
    info->last_t_ns = last.t_ns;
    if (rc == STORE_EIO) {
        fail(0, "read records", name);
        goto out;
    }
    if (rc == STORE_ECORRUPT) {
        why = "bad record";
        bad = sizeof h + keep * sizeof(struct record);
        goto corrupt;
    }
    rc = STORE_EIO;

    keep_size = (off_t)(sizeof h + keep * sizeof(struct record));
    if (keep_size != sb.st_size) {
        if (ftruncate(fd, keep_size) < 0) {
            fail(0, "truncate torn tail of", name);
            goto out;
        }
        if (fdatasync(fd) < 0) {
            fail(0, "fdatasync after truncating", name);
            goto out;
        }
        info->truncated_bytes = (uint64_t)(sb.st_size - keep_size);
        log_msg("%s: removed a torn tail of %" PRIu64 " bytes; %" PRIu64 " records kept",
                name, info->truncated_bytes, keep);
    }

    if (boot_known(st->boot_id) && memcmp(h.boot_id, st->boot_id, 16) == 0) {
        if (keep < STORE_SEG_MAX) {
            st->fd = fd;
            fd = -1;
            st->counter = counter;
            st->records = keep;
            st->size = keep_size;
            *reuse = 1;
        } else {
            log_msg("%s is full (%" PRIu64 " records); starting the next segment", name, keep);
        }
    } else {
        info->new_boot = 1; /* that segment stays sealed as it is */
    }
    rc = STORE_OK;
    goto out;

corrupt:
    if (close(fd) < 0) {
        fd = -1;
        rc = fail(STORE_EIO, "close", name);
        goto out;
    }
    fd = -1;
    rc = quarantine(st, counter, why, bad);
    if (rc == STORE_OK) {
        info->quarantined = 1;
        info->quarantined_counter = counter;
        info->bad_offset = bad;
    }

out:
    e = errno;
    if (fd >= 0 && close(fd) < 0)
        fail(0, "close", name);
    errno = e;
    return rc;
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
    uint32_t max_live = 0, next;
    int have_live = 0, rc = STORE_EIO, e;

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
        int reuse = 0;
        rc = recover_latest(st, max_live, info, &reuse);
        if (rc != STORE_OK)
            goto out;
        rc = STORE_EIO;
        if (reuse)
            goto done;
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
    release(st);
    errno = e;
    return rc;
}

/* Start counter+1 because the live segment is full. The new segment is
 * complete and durable before the old fd is let go; on failure the handle
 * still points at the old, untouched segment. */
static int rotate(struct store *st)
{
    int old = st->fd;
    uint32_t next;
    int rc;

    rc = next_counter(st, &next);
    if (rc != STORE_OK)
        return rc;
    rc = create_segment(st, next); /* replaces st->fd only on success */
    if (rc != STORE_OK)
        return rc;
    if (close(old) < 0)
        fail(0, "close", "full segment"); /* already fdatasync'd: log, carry on */
    log_msg("rotated to segment %06" PRIu32, next);
    return STORE_OK;
}

int store_append(struct store *st, const struct record *r)
{
    struct record rec;
    ssize_t w;

    if (st == NULL || r == NULL) {
        errno = EINVAL;
        return STORE_EINVAL;
    }
    if (r->magic != RECORD_MAGIC || r->sensor != (uint8_t)st->sensor || r->reserved != 0) {
        log_msg("refusing record #%" PRIu32 ": magic=0x%04x sensor=%u reserved=%" PRIu32,
                r->seq, (unsigned)r->magic, (unsigned)r->sensor, r->reserved);
        errno = EINVAL;
        return STORE_EINVAL;
    }
    if (st->failed) {
        errno = EIO;
        return STORE_EIO;
    }
    if (st->records >= STORE_SEG_MAX && rotate(st) != STORE_OK) {
        st->failed = 1;
        return STORE_EIO;
    }

    rec = *r;
    rec.crc32 = store_crc32(&rec, offsetof(struct record, crc32));
    do {
        w = write(st->fd, &rec, sizeof rec);
    } while (w < 0 && errno == EINTR);
    if (w != (ssize_t)sizeof rec) {
        if (w >= 0) {
            log_msg("short write: %zd of %zu bytes", w, sizeof rec);
            errno = EIO;
        } else {
            fail(0, "write", "segment");
        }
        goto rollback;
    }
    if (fdatasync(st->fd) < 0) {
        fail(0, "fdatasync", "segment");
        goto rollback;
    }
    st->records++;
    st->size += (off_t)sizeof rec;
    return STORE_OK;

rollback:
    /* Cut back to the last whole record so the file stays aligned. The
     * handle is done either way: after a failed write or fsync we cannot
     * know what the page cache holds, so the caller must close and exit. */
    {
        int e = errno;
        if (ftruncate(st->fd, st->size) < 0)
            fail(0, "rollback ftruncate", "segment");
        else if (fdatasync(st->fd) < 0)
            fail(0, "rollback fdatasync", "segment");
        st->failed = 1;
        errno = e;
    }
    return STORE_EIO;
}

uint32_t store_counter(const struct store *st)
{
    return st != NULL ? st->counter : 0;
}

int store_close(struct store *st)
{
    return release(st);
}

/* ---- Reader ---- */

#define READ_CHUNK 128 /* records per pread: 4 KiB */

struct seg_entry {
    uint32_t counter;
    uint32_t qnum; /* 0 = live/sealed file, n = .corrupt-<n> */
};

struct store_reader {
    int dirfd;
    enum sensor_id sensor;
    struct seg_entry *list; /* snapshot taken at open, in walk order */
    size_t count, next_file;
    int fd;                 /* current file, -1 between files */
    char name[NAME_LEN + 32];
    struct store_rec_src src;
    uint64_t n, idx;        /* whole records in the current file; next to read */
    struct record buf[READ_CHUNK];
    size_t buf_len, buf_pos;
    int failed;
    struct store_reader_stats stats;
};

static int seg_entry_cmp(const void *a, const void *b)
{
    const struct seg_entry *x = a, *y = b;

    if (x->counter != y->counter)
        return x->counter < y->counter ? -1 : 1;
    if (x->qnum != y->qnum)
        return x->qnum < y->qnum ? -1 : 1;
    return 0;
}

/* Snapshot this sensor's live and quarantined files, sorted for the walk. */
static int list_segments(struct store_reader *rd)
{
    const char *sname = sensor_name(rd->sensor);
    size_t cap = 0;
    struct dirent *e;
    DIR *d;
    int fd, rc = STORE_OK;

    fd = openat(rd->dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return fail(STORE_EIO, "open", "segment dir");
    d = fdopendir(fd);
    if (d == NULL) {
        fail(0, "fdopendir", "segment dir");
        if (close(fd) < 0)
            fail(0, "close", "segment dir");
        return STORE_EIO;
    }
    for (;;) {
        struct seg_entry s = { 0, 0 };
        enum name_kind kind;

        errno = 0;
        e = readdir(d);
        if (e == NULL) {
            if (errno != 0)
                rc = fail(STORE_EIO, "readdir", "segment dir");
            break;
        }
        kind = parse_name(e->d_name, sname, &s.counter, &s.qnum);
        if (kind != NAME_LIVE && kind != NAME_CORRUPT)
            continue;
        if (kind == NAME_LIVE)
            s.qnum = 0;
        else if (s.qnum == 0)
            continue; /* ".corrupt-0" is never written; not ours */
        if (rd->count == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            struct seg_entry *grown = realloc(rd->list, ncap * sizeof *grown);
            if (grown == NULL) {
                rc = fail(STORE_EIO, "realloc for", "segment list");
                break;
            }
            rd->list = grown;
            cap = ncap;
        }
        rd->list[rd->count++] = s;
    }
    if (closedir(d) < 0)
        rc = fail(STORE_EIO, "closedir", "segment dir");
    if (rc == STORE_OK && rd->count > 1)
        qsort(rd->list, rd->count, sizeof rd->list[0], seg_entry_cmp);
    return rc;
}

static void reader_close_file(struct store_reader *rd)
{
    if (rd->fd >= 0 && close(rd->fd) < 0)
        fail(0, "close", rd->name);
    rd->fd = -1;
}

/* Open the next listed file and check its header. STORE_OK with rd->fd set
 * means records follow; STORE_OK with rd->fd == -1 means the file was
 * skipped (gone, or a bad header); STORE_EIO is a real I/O failure. */
static int reader_open_next(struct store_reader *rd)
{
    const struct seg_entry *s = &rd->list[rd->next_file++];
    struct store_header h;
    struct stat sb;
    int rc;

    if (seg_name(rd->name, sizeof rd->name, rd->sensor, s->counter, "") < 0)
        return fail(STORE_EIO, "name", sensor_name(rd->sensor));
    if (s->qnum != 0) {
        size_t l = strlen(rd->name);
        snprintf(rd->name + l, sizeof rd->name - l, ".corrupt-%" PRIu32, s->qnum);
    }

    rd->fd = openat(rd->dirfd, rd->name, O_RDONLY | O_CLOEXEC);
    if (rd->fd < 0) {
        if (errno == ENOENT) {
            /* Renamed since the snapshot, e.g. quarantined by the writer. */
            rd->stats.vanished++;
            log_msg("reader: %s is gone since the listing; skipped", rd->name);
            return STORE_OK;
        }
        return fail(STORE_EIO, "open", rd->name);
    }
    if (s->qnum != 0)
        rd->stats.quarantined_files++;
    else
        rd->stats.segments++;

    if (fstat(rd->fd, &sb) < 0) {
        fail(0, "fstat", rd->name);
        reader_close_file(rd);
        return STORE_EIO;
    }
    if (sb.st_size < (off_t)sizeof h) {
        rd->stats.bad_header++;
        log_msg("reader: skipping %s: shorter than a header", rd->name);
        reader_close_file(rd);
        return STORE_OK;
    }
    if (pread_all(rd->fd, &h, sizeof h, 0) < 0) {
        fail(0, "read header", rd->name);
        reader_close_file(rd);
        return STORE_EIO;
    }
    rc = store_header_check(&h, rd->sensor, s->counter);
    if (rc != STORE_OK) {
        rd->stats.bad_header++;
        log_msg("reader: skipping %s: %s", rd->name, status_text(rc));
        reader_close_file(rd);
        return STORE_OK;
    }

    /* Whole records as of now; a write still in flight is left for next time. */
    rd->n = (uint64_t)(sb.st_size - (off_t)sizeof h) / sizeof(struct record);
    rd->idx = 0;
    rd->buf_len = rd->buf_pos = 0;
    rd->src.counter = s->counter;
    memcpy(rd->src.boot_id, h.boot_id, sizeof rd->src.boot_id);
    rd->src.quarantined = (s->qnum != 0);
    return STORE_OK;
}

int store_reader_open(const char *dir, enum sensor_id sensor, struct store_reader **out)
{
    struct store_reader *rd;
    int rc, e;

    if (out != NULL)
        *out = NULL;
    if (dir == NULL || out == NULL || sensor_name(sensor) == NULL) {
        errno = EINVAL;
        return STORE_EINVAL;
    }
    rd = calloc(1, sizeof *rd);
    if (rd == NULL)
        return fail(STORE_EIO, "calloc for", "reader");
    rd->fd = -1;
    rd->sensor = sensor;
    rd->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rd->dirfd < 0) {
        rc = fail(STORE_EIO, "open dir", dir);
        goto out;
    }
    rc = list_segments(rd);
    if (rc != STORE_OK)
        goto out;
    *out = rd;
    return STORE_OK;

out:
    e = errno;
    store_reader_close(rd);
    errno = e;
    return rc;
}

int store_reader_next(struct store_reader *rd, struct record *out, struct store_rec_src *src)
{
    if (rd == NULL || out == NULL) {
        errno = EINVAL;
        return STORE_EINVAL;
    }
    for (;;) {
        struct record r;

        if (rd->failed) {
            errno = EIO;
            return STORE_EIO;
        }
        if (rd->fd < 0) {
            if (rd->next_file >= rd->count)
                return 0;
            if (reader_open_next(rd) != STORE_OK) {
                rd->failed = 1;
                return STORE_EIO;
            }
            continue;
        }
        if (rd->idx >= rd->n) {
            reader_close_file(rd);
            continue;
        }
        if (rd->buf_pos == rd->buf_len) {
            uint64_t left = rd->n - rd->idx;
            size_t k = left < READ_CHUNK ? (size_t)left : READ_CHUNK;
            off_t off = (off_t)(sizeof(struct store_header) + rd->idx * sizeof(struct record));
            if (pread_all(rd->fd, rd->buf, k * sizeof rd->buf[0], off) < 0) {
                fail(0, "read records", rd->name);
                reader_close_file(rd);
                rd->failed = 1;
                return STORE_EIO;
            }
            rd->buf_len = k;
            rd->buf_pos = 0;
        }

        r = rd->buf[rd->buf_pos];
        if (store_record_check(&r, rd->sensor) != STORE_OK) {
            /* No resync: whatever follows a bad record in this file is not
             * returned. A bad last record of a live or sealed segment is a
             * write in flight or a torn one, not corruption. */
            if (!rd->src.quarantined && rd->idx != rd->n - 1) {
                rd->stats.corrupt_stops++;
                log_msg("reader: %s: bad record at offset %" PRIu64 "; rest of segment skipped",
                        rd->name,
                        (uint64_t)(sizeof(struct store_header) + rd->idx * sizeof(struct record)));
            }
            reader_close_file(rd);
            continue;
        }
        rd->buf_pos++;
        rd->idx++;
        if (rd->src.quarantined)
            rd->stats.salvaged_records++;
        *out = r;
        if (src != NULL)
            *src = rd->src;
        return 1;
    }
}

void store_reader_stats(const struct store_reader *rd, struct store_reader_stats *out)
{
    if (rd == NULL)
        memset(out, 0, sizeof *out);
    else
        *out = rd->stats;
}

int store_reader_close(struct store_reader *rd)
{
    int rc = STORE_OK;

    if (rd == NULL)
        return STORE_OK;
    if (rd->fd >= 0 && close(rd->fd) < 0)
        rc = fail(STORE_EIO, "close", rd->name);
    if (rd->dirfd >= 0 && close(rd->dirfd) < 0)
        rc = fail(STORE_EIO, "close", "segment dir");
    free(rd->list);
    free(rd);
    return rc;
}
