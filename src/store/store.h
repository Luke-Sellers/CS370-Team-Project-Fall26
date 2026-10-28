/* store.h -- per-sensor, append-only segment log of struct record.
 * Format and rules: docs/store-plan.md.
 *
 * On disk, in one directory:
 *   <sensor>.lock                 held (flock) by the one writer per sensor
 *   <sensor>.<counter>.mwlog      segment: 32-byte header, then records
 *   <sensor>.<counter>.mwlog.corrupt-<n>   quarantined segment, bytes kept
 * The counter is printed %06u and compared as a number. Byte order is the
 * host's, as in record.h.
 */
#ifndef MOLDWATCH_STORE_H
#define MOLDWATCH_STORE_H

#include <stdint.h>

#include "record.h"

#define STORE_MAGIC   0x474C574Du /* "MWLG" on disk (little-endian) */
#define STORE_VERSION 1u
#define STORE_SEG_MAX 8640u       /* records per segment, then the next counter */

/* The first 32 bytes of every segment. */
struct store_header {
    uint32_t magic;       /* STORE_MAGIC */
    uint16_t version;     /* STORE_VERSION */
    uint8_t  sensor;      /* enum sensor_id */
    uint8_t  record_size; /* sizeof(struct record) */
    uint32_t counter;     /* must equal the counter in the file name */
    uint8_t  boot_id[16]; /* /proc/sys/kernel/random/boot_id; zeros if unknown */
    uint32_t crc32;       /* store_crc32 over bytes [0, 28) */
};

_Static_assert(sizeof(struct store_header) == 32, "struct store_header must stay 32 bytes");

enum store_status {
    STORE_OK       = 0,
    STORE_EIO      = -1, /* a syscall failed; it was logged and errno is kept */
    STORE_EBADHDR  = -2, /* bad magic, CRC, record_size or counter, or too short */
    STORE_EVERSION = -3, /* another format version: refused, left untouched */
    STORE_ESENSOR  = -4, /* header is for the other sensor: refused, untouched */
    /* -5 is reserved for STORE_ECORRUPT (recovery). */
    STORE_EBUSY    = -6, /* another process holds <sensor>.lock */
    STORE_EINVAL   = -7, /* bad argument, or a record append refused */
};

struct store_open_info {
    uint32_t counter;  /* the segment appends go to */
    int      created;  /* 1 = this open created that segment */
    int      new_boot; /* 1 = the latest segment belonged to another boot */
    uint64_t records;  /* records already in that segment */
};

/* Check a segment header read from the file for counter. Version is judged
 * before the CRC, so a newer format is refused rather than called corrupt. */
int store_header_check(const struct store_header *h, enum sensor_id sensor, uint32_t counter);

struct store;

/* Open the log for one sensor in dir, which must already exist.
 * Takes <sensor>.lock for the life of the handle, removes this sensor's stale
 * .tmp files, then picks the segment: the latest one if it was written in this
 * boot and holds fewer than STORE_SEG_MAX records, otherwise a new one
 * numbered 1 + the highest counter seen, live or quarantined. On error *out
 * is NULL and no existing segment was changed. info may be NULL. */
int store_open(const char *dir, enum sensor_id sensor, struct store **out,
               struct store_open_info *info);

/* Append one record and fdatasync it; durable when this returns STORE_OK.
 * r is copied; storage fills crc32 over bytes [0, 28) and keeps every other
 * bit. STORE_EINVAL (nothing written) if magic, sensor or reserved is wrong.
 * When the live segment holds STORE_SEG_MAX records, a new segment is fully
 * created first and the record goes there.
 * On STORE_EIO the segment is rolled back to its last whole record and the
 * handle refuses all further appends: the caller must close and exit, since
 * after a failed fsync the page cache can no longer be trusted. */
int store_append(struct store *st, const struct record *r);

/* Counter of the live segment: the one opened, or the last one appended to. */
uint32_t store_counter(const struct store *st);

/* Close every fd and free. NULL is fine. STORE_EIO if a close failed. */
int store_close(struct store *st);

#endif
