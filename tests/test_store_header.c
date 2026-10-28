/* test_store_header -- step 2 of docs/store-plan.md: segment creation, the
 * header check, the per-sensor lock and the per-boot segment rule. Every test
 * gets its own mkdtemp directory. The current boot id is parsed here from
 * /proc, independently of store.c. */
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
        if (unlinkat(dirfd(d), e->d_name, 0) < 0 &&
            unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR) < 0)
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

static long read_file(const char *name, void *buf, size_t cap)
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

static void write_file(const char *name, const void *data, size_t len)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, data, len) != (ssize_t)len || close(fd) < 0) {
        perror(p);
        exit(2);
    }
}

static int exists(const char *name)
{
    char p[128];
    struct stat sb;
    path(p, sizeof p, name);
    return stat(p, &sb) == 0;
}

/* Names in the test dir that start with prefix and end with suffix. */
static int count_names(const char *prefix, const char *suffix)
{
    DIR *d = opendir(g_dir);
    struct dirent *e;
    int n = 0;
    size_t pl = strlen(prefix), sl = strlen(suffix);

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l >= pl + sl && strncmp(e->d_name, prefix, pl) == 0 &&
            strcmp(e->d_name + l - sl, suffix) == 0)
            n++;
    }
    closedir(d);
    return n;
}

struct snap {
    long len;
    unsigned char b[4096];
};

static void take_snap(const char *name, struct snap *s)
{
    memset(s->b, 0, sizeof s->b);
    s->len = read_file(name, s->b, sizeof s->b);
}

static int unchanged(const char *name, const struct snap *s)
{
    struct snap now;
    take_snap(name, &now);
    return s->len >= 0 && now.len == s->len && memcmp(now.b, s->b, (size_t)s->len) == 0;
}

static void reseal(struct store_header *h)
{
    h->crc32 = store_crc32(h, offsetof(struct store_header, crc32));
}

static void make_header(struct store_header *h, uint8_t sensor, uint32_t counter,
                        const uint8_t boot[16])
{
    memset(h, 0, sizeof *h);
    h->magic = STORE_MAGIC;
    h->version = STORE_VERSION;
    h->sensor = sensor;
    h->record_size = sizeof(struct record);
    h->counter = counter;
    memcpy(h->boot_id, boot, 16);
    reseal(h);
}

/* Header plus nrec valid records, as a writer would have left it. */
static void write_segment(const char *name, const struct store_header *h, unsigned nrec)
{
    unsigned char buf[4096];
    size_t len = sizeof *h;

    memcpy(buf, h, sizeof *h);
    for (unsigned i = 0; i < nrec && len + 32 <= sizeof buf; i++) {
        struct record r = {
            .magic = RECORD_MAGIC,
            .sensor = h->sensor,
            .seq = i + 1,
            .t_ns = 1700000000000000000LL + (int64_t)i * 10000000000LL,
            .a = 2150,
            .b = 5500,
        };
        r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
        memcpy(buf + len, &r, sizeof r);
        len += sizeof r;
    }
    write_file(name, buf, len);
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

static int list_fds(int *fds, int max)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int fd = atoi(e->d_name);
        if (fd != dirfd(d) && n < max)
            fds[n++] = fd;
    }
    closedir(d);
    return n;
}

/* ---- tests ---- */

/* H1: an empty dir gets segment 1 with a correct header, and the lock file. */
static void test_create_empty(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct store_header h;
    unsigned char buf[4096] = { 0 };

    setup();
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(st != NULL);
    CHECK(info.counter == 1 && info.created == 1 && info.new_boot == 0 && info.records == 0);
    CHECK(read_file("bme280.000001.mwlog", buf, sizeof buf) == 32);
    memcpy(&h, buf, sizeof h);
    CHECK(memcmp(buf, "MWLG", 4) == 0);
    CHECK(h.magic == STORE_MAGIC);
    CHECK(h.version == STORE_VERSION);
    CHECK(h.sensor == SENSOR_BME280);
    CHECK(h.record_size == 32);
    CHECK(h.counter == 1);
    CHECK(memcmp(h.boot_id, g_boot, 16) == 0);
    CHECK(h.crc32 == store_crc32(&h, 28));
    CHECK(exists("bme280.lock"));
    CHECK(count_names("bme280.", ".mwlog") == 1);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* H2: our stale .tmp files go; another sensor's stay; creation leaves none. */
static void test_stale_tmp(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };

    setup();
    write_file("bme280.000003.mwlog.tmp", "junk", 4);
    write_file("mlx90614.000001.mwlog.tmp", "junk", 4);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 1); /* a .tmp never counts as a segment */
    CHECK(!exists("bme280.000003.mwlog.tmp"));
    CHECK(exists("mlx90614.000001.mwlog.tmp"));
    CHECK(count_names("bme280.", ".tmp") == 0);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* H3: reopening in the same boot reuses the segment and changes nothing. */
static void test_reopen_same_boot(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct store_header h;
    struct snap s;

    setup();
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(store_close(st) == STORE_OK);
    st = NULL;

    make_header(&h, SENSOR_BME280, 1, g_boot); /* same bytes the store wrote */
    write_segment("bme280.000001.mwlog", &h, 3);
    take_snap("bme280.000001.mwlog", &s);

    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 1 && info.created == 0 && info.new_boot == 0 && info.records == 3);
    CHECK(unchanged("bme280.000001.mwlog", &s));
    CHECK(count_names("bme280.", ".mwlog") == 1);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* H4: a latest segment from another boot is left sealed; a new one starts. */
static void test_new_boot(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct store_header h;
    uint8_t ob[16];
    unsigned char buf[4096] = { 0 };
    struct snap s;

    setup();
    other_boot(ob);
    make_header(&h, SENSOR_BME280, 1, ob);
    write_segment("bme280.000001.mwlog", &h, 2);
    take_snap("bme280.000001.mwlog", &s);

    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 2 && info.created == 1 && info.new_boot == 1 && info.records == 0);
    CHECK(unchanged("bme280.000001.mwlog", &s));
    CHECK(read_file("bme280.000002.mwlog", buf, sizeof buf) == 32);
    memcpy(&h, buf, sizeof h);
    CHECK(h.counter == 2);
    CHECK(memcmp(h.boot_id, g_boot, 16) == 0);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* H5: the next counter is 1 + the max over live AND quarantined names. */
static void test_counter_counts_quarantined(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct store_header h;
    uint8_t ob[16];
    struct snap s;

    setup();
    other_boot(ob);
    make_header(&h, SENSOR_BME280, 1, ob);
    write_segment("bme280.000001.mwlog", &h, 1);
    write_file("bme280.000004.mwlog.corrupt-1", "garbage", 7);
    take_snap("bme280.000004.mwlog.corrupt-1", &s);

    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 5);
    CHECK(exists("bme280.000005.mwlog"));
    CHECK(unchanged("bme280.000004.mwlog.corrupt-1", &s));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* H6: names that are not ours are ignored; counters compare as numbers. */
static void test_names(void)
{
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct store_header h;
    uint8_t ob[16];
    static const char *const junk[] = {
        "bme280.abc.mwlog",          "foo",
        "bme280.000007.mwlog.bak",   "bme2800.000050.mwlog",
        "bme280.000060.mwlog.corrupt-", "bme280.000070.mwlog.corrupt-x",
        "bme280..mwlog",             "bme280.+00001.mwlog",
    };

    /* Only junk present: nothing counts, so this is segment 1. */
    setup();
    make_header(&h, SENSOR_BME280, 1, g_boot);
    write_segment("bme280.1.mwlog", &h, 1); /* not canonical %06u: not segment 1 */
    make_header(&h, SENSOR_MLX90614, 50, g_boot);
    write_segment("mlx90614.000050.mwlog", &h, 1); /* the other sensor's */
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        write_file(junk[i], "x", 1);

    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 1 && info.created == 1);
    CHECK(exists("bme280.000001.mwlog"));
    CHECK(exists("bme280.1.mwlog") && exists("mlx90614.000050.mwlog"));
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        CHECK(exists(junk[i]));
    CHECK(store_close(st) == STORE_OK);
    teardown();

    /* 1000000 sorts after 999999 numerically (not as a string), so the
     * same-boot segment 1000000 is the latest and is reused. */
    st = NULL;
    setup();
    other_boot(ob);
    make_header(&h, SENSOR_BME280, 999999, ob);
    write_segment("bme280.999999.mwlog", &h, 1);
    make_header(&h, SENSOR_BME280, 1000000, g_boot);
    write_segment("bme280.1000000.mwlog", &h, 1);

    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.counter == 1000000 && info.created == 0 && info.records == 1);
    CHECK(count_names("bme280.", ".mwlog") == 2);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* Write bytes as segment 1, open, expect a refusal, and check nothing moved. */
static void refuse_case(const void *bytes, size_t len, int expect)
{
    struct store *st = (struct store *)1; /* must be reset to NULL */
    struct snap s;

    setup();
    write_file("bme280.000001.mwlog", bytes, len);
    take_snap("bme280.000001.mwlog", &s);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, NULL) == expect);
    CHECK(st == NULL);
    CHECK(unchanged("bme280.000001.mwlog", &s));
    CHECK(count_names("bme280.", ".mwlog") == 1);
    CHECK(count_names("bme280.", ".tmp") == 0);
    teardown();
}

/* H7: wrong version or sensor is refused untouched. The EBADHDR cases are
 * step 2 behaviour; step 4 turns them into quarantine. */
static void test_refuse(void)
{
    struct store_header h;

    make_header(&h, SENSOR_BME280, 1, g_boot);
    h.version = 2;
    reseal(&h);
    refuse_case(&h, sizeof h, STORE_EVERSION);

    make_header(&h, SENSOR_MLX90614, 1, g_boot);
    refuse_case(&h, sizeof h, STORE_ESENSOR);

    make_header(&h, SENSOR_BME280, 1, g_boot);
    h.crc32 ^= 1;
    refuse_case(&h, sizeof h, STORE_EBADHDR);

    make_header(&h, SENSOR_BME280, 1, g_boot);
    h.magic ^= 1;
    reseal(&h);
    refuse_case(&h, sizeof h, STORE_EBADHDR);

    make_header(&h, SENSOR_BME280, 2, g_boot); /* file name says 1 */
    refuse_case(&h, sizeof h, STORE_EBADHDR);

    make_header(&h, SENSOR_BME280, 1, g_boot);
    refuse_case(&h, 10, STORE_EBADHDR); /* shorter than a header */
}

/* H8: one writer per sensor; the lock goes away with the handle. */
static void test_lock(void)
{
    struct store *a = NULL, *b = (struct store *)1, *c = NULL, *m = NULL;

    setup();
    CHECK(store_open(g_dir, SENSOR_BME280, &a, NULL) == STORE_OK);
    CHECK(store_open(g_dir, SENSOR_BME280, &b, NULL) == STORE_EBUSY);
    CHECK(b == NULL);
    CHECK(store_open(g_dir, SENSOR_MLX90614, &m, NULL) == STORE_OK); /* other sensor */
    CHECK(store_close(m) == STORE_OK);
    CHECK(store_close(a) == STORE_OK);
    CHECK(store_open(g_dir, SENSOR_BME280, &c, NULL) == STORE_OK);
    CHECK(store_close(c) == STORE_OK);
    teardown();
}

/* H9: no fd leaks; every fd the store holds is close-on-exec. */
static void test_fds(void)
{
    struct store *st = NULL;
    int before[256], after[256];
    int n0, n1, n2, bad = 0;

    setup();
    n0 = list_fds(before, 256);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, NULL) == STORE_OK);
    n1 = list_fds(after, 256);
    CHECK(n1 > n0);
    for (int i = 0; i < n1; i++) {
        int seen = 0;
        for (int j = 0; j < n0; j++)
            seen |= (after[i] == before[j]);
        if (!seen)
            CHECK(fcntl(after[i], F_GETFD) & FD_CLOEXEC);
    }
    CHECK(store_close(st) == STORE_OK);
    CHECK(list_fds(after, 256) == n0);

    for (int i = 0; i < 1000; i++) {
        st = NULL;
        if (store_open(g_dir, SENSOR_BME280, &st, NULL) != STORE_OK ||
            store_close(st) != STORE_OK)
            bad++;
    }
    CHECK(bad == 0);
    n2 = list_fds(after, 256);
    CHECK(n2 == n0);
    teardown();
}

/* H10: bad directories and bad arguments fail cleanly and create nothing. */
static void test_bad_dirs(void)
{
    struct store *st = (struct store *)1;
    char p[128];

    setup();
    CHECK(store_open(NULL, SENSOR_BME280, &st, NULL) == STORE_EINVAL);
    CHECK(st == NULL);
    CHECK(store_open(g_dir, (enum sensor_id)0, &st, NULL) == STORE_EINVAL);
    CHECK(store_open(g_dir, (enum sensor_id)3, &st, NULL) == STORE_EINVAL);
    CHECK(store_open(g_dir, SENSOR_BME280, NULL, NULL) == STORE_EINVAL);
    CHECK(!exists("bme280.lock")); /* arguments are checked before any I/O */

    path(p, sizeof p, "nope");
    CHECK(store_open(p, SENSOR_BME280, &st, NULL) == STORE_EIO);
    CHECK(st == NULL);
    CHECK(!exists("nope"));

    write_file("afile", "x", 1);
    path(p, sizeof p, "afile");
    CHECK(store_open(p, SENSOR_BME280, &st, NULL) == STORE_EIO);

    /* The lock path is taken by a directory: no lock, so no segment. This
     * stands in for an unwritable dir, since the container runs as root. */
    path(p, sizeof p, "bme280.lock");
    CHECK(mkdir(p, 0755) == 0);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, NULL) == STORE_EIO);
    CHECK(st == NULL);
    CHECK(count_names("bme280.", ".mwlog") == 0);

    CHECK(store_close(NULL) == STORE_OK);
    teardown();
}

/* H11: store_header_check case by case, and the on-disk layout. */
static void test_header_check(void)
{
    struct store_header ok, h;

    CHECK(offsetof(struct store_header, magic) == 0);
    CHECK(offsetof(struct store_header, version) == 4);
    CHECK(offsetof(struct store_header, sensor) == 6);
    CHECK(offsetof(struct store_header, record_size) == 7);
    CHECK(offsetof(struct store_header, counter) == 8);
    CHECK(offsetof(struct store_header, boot_id) == 12);
    CHECK(offsetof(struct store_header, crc32) == 28);

    make_header(&ok, SENSOR_BME280, 7, g_boot);
    CHECK(store_header_check(&ok, SENSOR_BME280, 7) == STORE_OK);
    CHECK(store_header_check(&ok, SENSOR_BME280, 8) == STORE_EBADHDR);
    CHECK(store_header_check(&ok, SENSOR_MLX90614, 7) == STORE_ESENSOR);

    h = ok;
    h.magic ^= 1;
    reseal(&h);
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EBADHDR);

    h = ok;
    h.crc32 ^= 1;
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EBADHDR);

    h = ok;
    h.boot_id[5] ^= 0x10; /* any covered byte, not resealed */
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EBADHDR);

    h = ok;
    h.record_size = 16;
    reseal(&h);
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EBADHDR);

    h = ok;
    h.version = 2;
    reseal(&h);
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EVERSION);

    h = ok;
    h.version = 2; /* not resealed: version is judged before the CRC */
    CHECK(store_header_check(&h, SENSOR_BME280, 7) == STORE_EVERSION);
}

int main(void)
{
    log_init("test_store_header", "store");
    load_boot_id();

    test_create_empty();
    test_stale_tmp();
    test_reopen_same_boot();
    test_new_boot();
    test_counter_counts_quarantined();
    test_names();
    test_refuse();
    test_lock();
    test_fds();
    test_bad_dirs();
    test_header_check();

    if (failures == 0)
        printf("test_store_header: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
