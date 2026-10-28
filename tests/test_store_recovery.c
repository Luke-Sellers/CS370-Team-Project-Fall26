/* test_store_recovery -- step 4 of docs/store-plan.md: at open, the latest
 * segment's torn tail (a partial record, or one bad last record) is
 * truncated; a bad header or a bad record before the last is corruption, and
 * the segment is renamed <seg>.corrupt-<n> with every byte kept while a fresh
 * segment starts. Segments are crafted byte by byte, then damaged. */
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "log.h"
#include "store.h"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define HDR 32L
#define REC 32L
#define SEG1 "bme280.000001.mwlog"
#define SEG2 "bme280.000002.mwlog"
#define Q1   "bme280.000001.mwlog.corrupt-1"
#define Q2   "bme280.000001.mwlog.corrupt-2"
#define T0   1700000000000000000LL
#define DT   10000000000LL

static char g_dir[64];
static uint8_t g_boot[16];

/* ---- fixtures ---- */

static void setup(void)
{
    strcpy(g_dir, "/tmp/mwstore.XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
}

static void teardown(void)
{
    DIR *d = opendir(g_dir);
    struct dirent *e;

    if (d == NULL) {
        perror("opendir");
        exit(2);
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (unlinkat(dirfd(d), e->d_name, 0) < 0)
            perror(e->d_name);
    }
    closedir(d);
    if (rmdir(g_dir) < 0)
        perror("rmdir");
}

static void path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_dir, name);
}

static int exists(const char *name)
{
    char p[128];
    struct stat sb;
    path(p, sizeof p, name);
    return stat(p, &sb) == 0;
}

static long file_size(const char *name)
{
    char p[128];
    struct stat sb;
    path(p, sizeof p, name);
    return stat(p, &sb) == 0 ? (long)sb.st_size : -1;
}

/* A file image: what was written, so it can be compared byte for byte. */
struct image {
    long len;
    unsigned char b[HDR + 64 * REC];
};

static void write_image(const char *name, const struct image *im)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, im->b, (size_t)im->len) != (ssize_t)im->len || close(fd) < 0) {
        perror(p);
        exit(2);
    }
}

static long read_into(const char *name, unsigned char *buf, size_t cap)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap);
    close(fd);
    return (long)n;
}

/* The file holds exactly the first len bytes of im. */
static int holds_prefix(const char *name, const struct image *im, long len)
{
    unsigned char buf[sizeof im->b + 64];
    long n = read_into(name, buf, sizeof buf);
    return n == len && memcmp(buf, im->b, (size_t)len) == 0;
}

static int holds(const char *name, const struct image *im)
{
    return holds_prefix(name, im, im->len);
}

static struct record mk(uint32_t seq)
{
    struct record r = {
        .magic = RECORD_MAGIC,
        .sensor = SENSOR_BME280,
        .seq = seq,
        .t_ns = T0 + (int64_t)(seq - 1) * DT,
        .a = 2150,
        .b = 5500,
    };
    r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
    return r;
}

/* Header for `counter` with boot id `boot`, then nrec valid records 1..nrec. */
static void build(struct image *im, uint32_t counter, const uint8_t boot[16], int nrec)
{
    struct store_header h;

    memset(im, 0, sizeof *im);
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.sensor = SENSOR_BME280;
    h.record_size = sizeof(struct record);
    h.counter = counter;
    memcpy(h.boot_id, boot, 16);
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    memcpy(im->b, &h, sizeof h);
    for (int i = 0; i < nrec; i++) {
        struct record r = mk((uint32_t)i + 1);
        memcpy(im->b + HDR + i * REC, &r, sizeof r);
    }
    im->len = HDR + nrec * REC;
}

/* Damage record i in place: flip a value bit without fixing the CRC. */
static void spoil(struct image *im, int i)
{
    im->b[HDR + i * REC + offsetof(struct record, a)] ^= 0x01;
}

/* Append n junk bytes: a write that was cut off. */
static void tear(struct image *im, int n)
{
    memset(im->b + im->len, 0xAB, (size_t)n);
    im->len += n;
}

static void other_boot(uint8_t out[16])
{
    memcpy(out, g_boot, 16);
    out[0] ^= 0xFF;
}

static void load_boot_id(void)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    char s[64];
    int n = 0;

    if (f == NULL || fgets(s, sizeof s, f) == NULL) {
        perror("boot_id");
        exit(2);
    }
    fclose(f);
    for (const char *c = s; *c != '\0' && *c != '\n'; c++) {
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
            fprintf(stderr, "unparseable boot_id: %s", s);
            exit(2);
        }
        if (n % 2 == 0)
            g_boot[n / 2] = (uint8_t)(v << 4);
        else
            g_boot[n / 2] |= (uint8_t)v;
        n++;
    }
    if (n != 32) {
        fprintf(stderr, "short boot_id: %s", s);
        exit(2);
    }
}

/* Open, record the info, and keep the handle for the caller. */
static struct store *open_bme(struct store_open_info *info)
{
    struct store *st = NULL;
    memset(info, 0, sizeof *info);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, info) == STORE_OK);
    if (st == NULL) {
        fprintf(stderr, "cannot continue without a store handle\n");
        exit(1);
    }
    return st;
}

/* Every record in a segment file is valid and seq runs 1..n. */
static int all_valid(const char *name, long n)
{
    unsigned char buf[HDR + 64 * REC];
    long len = read_into(name, buf, sizeof buf);
    if (len != HDR + n * REC)
        return 0;
    for (long i = 0; i < n; i++) {
        struct record r;
        memcpy(&r, buf + HDR + i * REC, sizeof r);
        if (store_record_check(&r, SENSOR_BME280) != STORE_OK || r.seq != (uint32_t)i + 1)
            return 0;
    }
    return 1;
}

/* ---- tests ---- */

/* R1: a partial record of every possible length (1..31 bytes) is cut away;
 * all whole records stay, and nothing is quarantined. */
static void test_partial_tail(void)
{
    int bad = 0;

    for (int k = 1; k < REC; k++) {
        struct image im;
        struct store_open_info info;
        struct store *st;

        setup();
        build(&im, 1, g_boot, 10);
        tear(&im, k);
        write_image(SEG1, &im);
        st = open_bme(&info);
        if (info.records != 10 || info.truncated_bytes != (uint64_t)k || info.quarantined ||
            info.counter != 1 || info.created || !holds_prefix(SEG1, &im, HDR + 10 * REC))
            bad++;
        CHECK(store_close(st) == STORE_OK);
        teardown();
    }
    CHECK(bad == 0);
}

/* R2 / R3 / R4: one bad last record (CRC wrong, or never written: zeros),
 * alone or followed by a partial record, is a torn final write. */
static void torn_last_case(int zeros, int partial)
{
    struct image im;
    struct store_open_info info;
    struct store *st;

    setup();
    build(&im, 1, g_boot, 10);
    if (zeros)
        memset(im.b + HDR + 9 * REC, 0, REC);
    else
        spoil(&im, 9);
    tear(&im, partial);
    write_image(SEG1, &im);

    st = open_bme(&info);
    CHECK(info.records == 9);
    CHECK(info.truncated_bytes == (uint64_t)(REC + partial));
    CHECK(info.quarantined == 0 && info.counter == 1 && info.created == 0);
    CHECK(info.last_seq == 9 && info.last_t_ns == T0 + 8 * DT);
    CHECK(holds_prefix(SEG1, &im, HDR + 9 * REC));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

static void test_torn_last(void)
{
    torn_last_case(0, 0);  /* R2: bad CRC */
    torn_last_case(1, 0);  /* R3: all zeros */
    torn_last_case(0, 7);  /* R4: bad last + partial */
    torn_last_case(1, 31); /* R4: zeros + longest partial */
}

/* R5 / R6 / R7: a bad record anywhere before the last is corruption. The
 * segment is renamed .corrupt-1 untouched and segment 2 starts. */
static void corrupt_case(const int *bad_idx, int nbad, long want_offset)
{
    struct image im;
    struct store_open_info info;
    struct store *st;

    setup();
    build(&im, 1, g_boot, 10);
    for (int i = 0; i < nbad; i++)
        spoil(&im, bad_idx[i]);
    write_image(SEG1, &im);

    st = open_bme(&info);
    CHECK(info.quarantined == 1 && info.quarantined_counter == 1);
    CHECK(info.bad_offset == (uint64_t)want_offset);
    CHECK(info.truncated_bytes == 0);
    CHECK(info.counter == 2 && info.created == 1 && info.records == 0);
    CHECK(!exists(SEG1));
    CHECK(holds(Q1, &im));
    CHECK(file_size(SEG2) == HDR);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

static void test_corrupt(void)
{
    static const int mid[] = { 4 };
    static const int first[] = { 0 };
    static const int last_two[] = { 8, 9 };
    static const int second_last[] = { 8 };

    corrupt_case(mid, 1, HDR + 4 * REC);         /* R5 */
    corrupt_case(first, 1, HDR);                 /* R6 */
    corrupt_case(last_two, 2, HDR + 8 * REC);    /* R7: only one write is ever in flight */
    corrupt_case(second_last, 1, HDR + 8 * REC); /* bad, then a good last record */
}

/* R8: a bad header in front of valid records is still corruption (offset 0).
 * Opens directly rather than through open_bme, so a refusal here is reported
 * as FAIL and the remaining tests still run. */
static void test_bad_header_with_records(void)
{
    struct image im;
    struct store_open_info info = { 0 };
    struct store *st = NULL;

    setup();
    build(&im, 1, g_boot, 5);
    im.b[offsetof(struct store_header, boot_id)] ^= 0x01; /* header CRC now wrong */
    write_image(SEG1, &im);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(st != NULL);
    CHECK(info.quarantined == 1 && info.bad_offset == 0);
    CHECK(info.counter == 2 && info.created == 1);
    CHECK(holds(Q1, &im));
    if (st != NULL)
        CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* R9: quarantining never overwrites an earlier quarantine of the same name. */
static void test_requarantine(void)
{
    struct image im, old;
    struct store_open_info info;
    struct store *st;

    setup();
    memset(&old, 0, sizeof old);
    memcpy(old.b, "an earlier quarantined file", 27);
    old.len = 27;
    write_image(Q1, &old);
    build(&im, 1, g_boot, 10);
    spoil(&im, 3);
    write_image(SEG1, &im);

    st = open_bme(&info);
    CHECK(info.quarantined == 1 && info.counter == 2);
    CHECK(holds(Q1, &old));
    CHECK(holds(Q2, &im));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* R10: a torn tail on a segment from another boot is cleaned too, and that
 * segment then stays sealed while a new one starts. */
static void test_old_boot_tail(void)
{
    struct image im;
    struct store_open_info info;
    struct store *st;
    uint8_t ob[16];

    setup();
    other_boot(ob);
    build(&im, 1, ob, 10);
    tear(&im, 5);
    write_image(SEG1, &im);
    st = open_bme(&info);
    CHECK(info.truncated_bytes == 5 && info.quarantined == 0);
    CHECK(info.new_boot == 1 && info.counter == 2 && info.created == 1);
    CHECK(info.last_seq == 10);
    CHECK(holds_prefix(SEG1, &im, HDR + 10 * REC));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* R11 + R12: recovery is idempotent, and appending afterwards works and
 * leaves a clean segment, both after truncation and after quarantine. */
static void test_after_recovery(void)
{
    struct image im;
    struct store_open_info info;
    struct store *st;
    struct record r;

    /* After truncation. */
    setup();
    build(&im, 1, g_boot, 10);
    spoil(&im, 9);
    tear(&im, 3);
    write_image(SEG1, &im);
    st = open_bme(&info);
    CHECK(info.truncated_bytes == REC + 3 && info.records == 9);
    r = mk(10);
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_close(st) == STORE_OK);
    st = open_bme(&info);
    CHECK(info.truncated_bytes == 0 && info.quarantined == 0);
    CHECK(info.counter == 1 && info.created == 0 && info.records == 10);
    CHECK(store_close(st) == STORE_OK);
    CHECK(all_valid(SEG1, 10));
    teardown();

    /* After quarantine. */
    setup();
    build(&im, 1, g_boot, 10);
    spoil(&im, 2);
    write_image(SEG1, &im);
    st = open_bme(&info);
    CHECK(info.quarantined == 1 && info.counter == 2);
    r = mk(1);
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_close(st) == STORE_OK);
    st = open_bme(&info);
    CHECK(info.quarantined == 0 && info.truncated_bytes == 0);
    CHECK(info.counter == 2 && info.created == 0 && info.records == 1);
    CHECK(store_close(st) == STORE_OK);
    CHECK(all_valid(SEG2, 1));
    CHECK(holds(Q1, &im));
    teardown();
}

/* R13: last_seq / last_t_ns come from the last valid record; 0 when empty. */
static void test_last_seen(void)
{
    struct image im;
    struct store_open_info info;
    struct store *st;

    setup();
    build(&im, 1, g_boot, 10);
    write_image(SEG1, &im);
    st = open_bme(&info);
    CHECK(info.last_seq == 10 && info.last_t_ns == T0 + 9 * DT);
    CHECK(store_close(st) == STORE_OK);
    teardown();

    setup();
    build(&im, 1, g_boot, 0);
    write_image(SEG1, &im);
    st = open_bme(&info);
    CHECK(info.last_seq == 0 && info.last_t_ns == 0 && info.records == 0);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* R14: store_record_check, case by case. A record that is well formed but
 * belongs to the other sensor is not valid in this sensor's log. */
static void test_record_check(void)
{
    struct record ok = mk(1), r;

    CHECK(store_record_check(&ok, SENSOR_BME280) == STORE_OK);
    CHECK(store_record_check(&ok, SENSOR_MLX90614) == STORE_ECORRUPT);

    r = ok;
    r.crc32 ^= 1;
    CHECK(store_record_check(&r, SENSOR_BME280) == STORE_ECORRUPT);

    r = ok;
    r.magic ^= 1;
    r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
    CHECK(store_record_check(&r, SENSOR_BME280) == STORE_ECORRUPT);

    r = ok;
    r.sensor = SENSOR_MLX90614;
    r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
    CHECK(store_record_check(&r, SENSOR_BME280) == STORE_ECORRUPT);

    memset(&r, 0, sizeof r);
    CHECK(store_record_check(&r, SENSOR_BME280) == STORE_ECORRUPT);
}

int main(void)
{
    log_init("test_store_recovery", "store");
    load_boot_id();

    test_partial_tail();
    test_torn_last();
    test_corrupt();
    test_bad_header_with_records();
    test_requarantine();
    test_old_boot_tail();
    test_after_recovery();
    test_last_seen();
    test_record_check();

    if (failures == 0)
        printf("test_store_recovery: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
