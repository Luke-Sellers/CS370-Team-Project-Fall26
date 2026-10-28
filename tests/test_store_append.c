/* test_store_append -- step 3 of docs/store-plan.md: appends are validated,
 * stored bit for bit with storage's CRC, and rolled back on failure; a segment
 * holds at most STORE_SEG_MAX records and the next append goes to counter+1.
 * Large segments are built directly (no fsync per record) to keep this fast. */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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

/* Whole file into a malloc'd buffer; *len = -1 if unreadable. */
static unsigned char *read_all(const char *name, long *len)
{
    char p[128];
    long n = file_size(name);
    unsigned char *buf;
    int fd;

    *len = -1;
    if (n < 0)
        return NULL;
    buf = malloc((size_t)n + 1);
    path(p, sizeof p, name);
    fd = open(p, O_RDONLY);
    if (buf == NULL || fd < 0 || read(fd, buf, (size_t)n) != (ssize_t)n) {
        perror(p);
        exit(2);
    }
    close(fd);
    *len = n;
    return buf;
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

/* Fingerprint of a whole file: length and CRC. */
struct print {
    long len;
    uint32_t crc;
};

static struct print fingerprint(const char *name)
{
    struct print f = { -1, 0 };
    unsigned char *b = read_all(name, &f.len);
    if (b != NULL)
        f.crc = store_crc32(b, (size_t)f.len);
    free(b);
    return f;
}

static int same_print(const char *name, struct print want)
{
    struct print now = fingerprint(name);
    return want.len >= 0 && now.len == want.len && now.crc == want.crc;
}

static struct record mk(uint32_t seq, int64_t t_ns, int32_t a, int32_t b, uint8_t flags)
{
    struct record r = {
        .magic = RECORD_MAGIC,
        .sensor = SENSOR_BME280,
        .flags = flags,
        .seq = seq,
        .t_ns = t_ns,
        .a = a,
        .b = b,
    };
    return r;
}

static struct record rec_at(const unsigned char *buf, long i)
{
    struct record r;
    memcpy(&r, buf + HDR + i * REC, sizeof r);
    return r;
}

static int crc_ok(const struct record *r)
{
    return r->crc32 == store_crc32(r, offsetof(struct record, crc32));
}

/* A segment as a writer would have left it: header plus nrec valid records,
 * seq 1..nrec. */
static void craft_segment(const char *name, uint32_t counter, uint32_t nrec)
{
    size_t len = (size_t)(HDR + (long)nrec * REC);
    unsigned char *buf = malloc(len);
    struct store_header h;

    if (buf == NULL) {
        perror("malloc");
        exit(2);
    }
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.sensor = SENSOR_BME280;
    h.record_size = sizeof(struct record);
    h.counter = counter;
    memcpy(h.boot_id, g_boot, 16);
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    memcpy(buf, &h, sizeof h);
    for (uint32_t i = 0; i < nrec; i++) {
        struct record r = mk(i + 1, 1700000000000000000LL + (int64_t)i * 10000000000LL,
                             2150, 5500, 0);
        r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
        memcpy(buf + HDR + (long)i * REC, &r, sizeof r);
    }
    write_file(name, buf, len);
    free(buf);
}

static struct store_header header_of(const char *name)
{
    struct store_header h;
    long len;
    unsigned char *b = read_all(name, &len);

    memset(&h, 0xEE, sizeof h);
    if (b != NULL && len >= HDR)
        memcpy(&h, b, sizeof h);
    free(b);
    return h;
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

static struct store *open_bme(struct store_open_info *info)
{
    struct store *st = NULL;
    struct store_open_info scratch;
    CHECK(store_open(g_dir, SENSOR_BME280, &st, info ? info : &scratch) == STORE_OK);
    if (st == NULL) {
        fprintf(stderr, "cannot continue without a store handle\n");
        exit(1);
    }
    return st;
}

/* ---- tests ---- */

/* A1: 100 appends survive close and reopen, bit for bit, each with a valid CRC. */
static void test_roundtrip(void)
{
    struct record in[100];
    struct store_open_info info;
    struct store *st;
    unsigned char *buf;
    long len;
    int bad = 0;

    setup();
    st = open_bme(NULL);
    for (int i = 0; i < 100; i++) {
        in[i] = mk((uint32_t)i + 1, 1700000000000000000LL + i * 10000000000LL, 2000 + i,
                   5000 + 3 * i, REC_SYNTHETIC);
        CHECK(store_append(st, &in[i]) == STORE_OK);
    }
    CHECK(store_close(st) == STORE_OK);

    st = open_bme(&info);
    CHECK(info.counter == 1 && info.created == 0 && info.records == 100);
    CHECK(store_close(st) == STORE_OK);

    buf = read_all(SEG1, &len);
    CHECK(len == HDR + 100 * REC);
    for (int i = 0; buf != NULL && i < 100 && len == HDR + 100 * REC; i++) {
        struct record got = rec_at(buf, i);
        if (!crc_ok(&got))
            bad++;
        got.crc32 = in[i].crc32; /* the only field storage may change */
        if (memcmp(&got, &in[i], sizeof got) != 0)
            bad++;
    }
    CHECK(bad == 0);
    free(buf);
    teardown();
}

/* A2: the caller's record is untouched, and its crc32 field is ignored. */
static void test_caller_record_const(void)
{
    struct record r = mk(1, 1700000000000000000LL, 2150, 5500, 0), copy;
    struct store *st;
    unsigned char *buf;
    long len;

    setup();
    st = open_bme(NULL);
    r.crc32 = 0xDEADBEEFu;
    copy = r;
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(memcmp(&r, &copy, sizeof r) == 0);
    CHECK(store_close(st) == STORE_OK);

    buf = read_all(SEG1, &len);
    CHECK(len == HDR + REC);
    if (len == HDR + REC) {
        struct record got = rec_at(buf, 0);
        CHECK(crc_ok(&got));
        CHECK(got.crc32 != 0xDEADBEEFu);
    }
    free(buf);
    teardown();
}

/* A3: malformed records are refused with nothing written, and the handle
 * stays usable (a refused record is not an I/O failure). */
static void test_refused_records(void)
{
    struct record good = mk(1, 1700000000000000000LL, 2150, 5500, 0), r;
    struct store *st;

    setup();
    st = open_bme(NULL);

    r = good;
    r.magic ^= 1;
    CHECK(store_append(st, &r) == STORE_EINVAL);
    r = good;
    r.sensor = SENSOR_MLX90614;
    CHECK(store_append(st, &r) == STORE_EINVAL);
    r = good;
    r.reserved = 1;
    CHECK(store_append(st, &r) == STORE_EINVAL);
    CHECK(store_append(st, NULL) == STORE_EINVAL);
    CHECK(store_append(NULL, &good) == STORE_EINVAL);
    CHECK(file_size(SEG1) == HDR);

    CHECK(store_append(st, &good) == STORE_OK);
    CHECK(file_size(SEG1) == HDR + REC);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* A4: flags (known and unknown bits) and zeroed READ_ERR values are kept exactly. */
static void test_flags_preserved(void)
{
    struct record r = mk(9, 1700000000000000000LL, 0, 0,
                         REC_SYNTHETIC | REC_READ_ERR | 0x80u);
    struct store *st;
    unsigned char *buf;
    long len;

    setup();
    st = open_bme(NULL);
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_close(st) == STORE_OK);
    buf = read_all(SEG1, &len);
    CHECK(len == HDR + REC);
    if (len == HDR + REC)
        CHECK(memcmp(buf + HDR, &r, offsetof(struct record, crc32)) == 0);
    free(buf);
    teardown();
}

/* A5: seq resets and a wall clock that is zero or runs backwards are stored as
 * received, in arrival order. Storage does not judge time. */
static void test_arrival_order(void)
{
    static const uint32_t seq[] = { 1, 2, 3, 4, 5, 1, 2, 3 };
    static const int64_t t[] = { 1700000000000000000LL, 1700000010000000000LL, 0,
                                 1700000020000000000LL, 1600000000000000000LL,
                                 1700000030000000000LL, 1700000030000000000LL, -1 };
    struct store *st;
    unsigned char *buf;
    long len;
    int bad = 0;

    setup();
    st = open_bme(NULL);
    for (int i = 0; i < 8; i++) {
        struct record r = mk(seq[i], t[i], 2150, 5500, 0);
        CHECK(store_append(st, &r) == STORE_OK);
    }
    CHECK(store_close(st) == STORE_OK);
    buf = read_all(SEG1, &len);
    CHECK(len == HDR + 8 * REC);
    for (int i = 0; buf != NULL && len == HDR + 8 * REC && i < 8; i++) {
        struct record got = rec_at(buf, i);
        if (got.seq != seq[i] || got.t_ns != t[i])
            bad++;
    }
    CHECK(bad == 0);
    free(buf);
    teardown();
}

/* A6: a write cut short by the file-size limit is rolled back to the last
 * whole record, the handle refuses further appends, and a reopen is clean.
 * RLIMIT_FSIZE stands in for a full disk without needing root. */
static void test_short_write_rollback(void)
{
    struct record r = mk(1, 1700000000000000000LL, 2150, 5500, 0);
    struct store_open_info info;
    struct rlimit old, lim;
    struct sigaction ign = { .sa_handler = SIG_IGN }, old_sa;
    struct store *st;

    setup();
    st = open_bme(NULL);
    sigemptyset(&ign.sa_mask);
    if (getrlimit(RLIMIT_FSIZE, &old) < 0 || sigaction(SIGXFSZ, &ign, &old_sa) < 0) {
        perror("rlimit setup");
        exit(2);
    }
    lim = old;
    lim.rlim_cur = (rlim_t)(HDR + 2 * REC + REC / 2); /* room for 2.5 records */
    if (setrlimit(RLIMIT_FSIZE, &lim) < 0) {
        perror("setrlimit");
        exit(2);
    }

    CHECK(store_append(st, &r) == STORE_OK);
    r.seq = 2;
    CHECK(store_append(st, &r) == STORE_OK);
    r.seq = 3;
    CHECK(store_append(st, &r) == STORE_EIO); /* only 16 of 32 bytes fit */

    if (setrlimit(RLIMIT_FSIZE, &old) < 0 || sigaction(SIGXFSZ, &old_sa, NULL) < 0) {
        perror("rlimit restore");
        exit(2);
    }
    CHECK(file_size(SEG1) == HDR + 2 * REC);
    r.seq = 4;
    CHECK(store_append(st, &r) == STORE_EIO); /* failed handle: no retry */
    CHECK(file_size(SEG1) == HDR + 2 * REC);
    CHECK(store_close(st) == STORE_OK);

    st = open_bme(&info);
    CHECK(info.counter == 1 && info.created == 0 && info.records == 2);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* A7 + A11: the record that fills a segment stays in it; the next one starts
 * counter+1 with this boot's id; nothing is lost or duplicated across. */
static void test_rotation(void)
{
    struct store_open_info info;
    struct store_header h;
    struct store *st;
    unsigned char *b1, *b2;
    long l1, l2;
    int bad = 0;
    uint32_t want = 1;

    CHECK(STORE_SEG_MAX == 8640u);
    setup();
    craft_segment(SEG1, 1, STORE_SEG_MAX - 1);
    st = open_bme(&info);
    CHECK(info.counter == 1 && info.created == 0 && info.records == STORE_SEG_MAX - 1);

    struct record r = mk(STORE_SEG_MAX, 1800000000000000000LL, 2150, 5500, 0);
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_counter(st) == 1);
    CHECK(file_size(SEG1) == HDR + (long)STORE_SEG_MAX * REC);
    CHECK(!exists(SEG2));

    r.seq = STORE_SEG_MAX + 1;
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_counter(st) == 2);
    CHECK(file_size(SEG1) == HDR + (long)STORE_SEG_MAX * REC);
    CHECK(file_size(SEG2) == HDR + REC);
    h = header_of(SEG2);
    CHECK(h.counter == 2 && h.sensor == SENSOR_BME280);
    CHECK(memcmp(h.boot_id, g_boot, 16) == 0);
    CHECK(!exists("bme280.000002.mwlog.tmp"));
    CHECK(store_close(st) == STORE_OK);

    /* A11: walk both segments; seq must run 1..8641 with every CRC valid. */
    b1 = read_all(SEG1, &l1);
    b2 = read_all(SEG2, &l2);
    for (long i = 0; b1 != NULL && i < (l1 - HDR) / REC; i++, want++) {
        struct record g = rec_at(b1, i);
        bad += !crc_ok(&g) || g.seq != want;
    }
    for (long i = 0; b2 != NULL && i < (l2 - HDR) / REC; i++, want++) {
        struct record g = rec_at(b2, i);
        bad += !crc_ok(&g) || g.seq != want;
    }
    CHECK(bad == 0);
    CHECK(want == STORE_SEG_MAX + 2);
    free(b1);
    free(b2);

    /* Reopen in the same boot: segment 2 is live and not full. */
    st = open_bme(&info);
    CHECK(info.counter == 2 && info.created == 0 && info.records == 1);
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* A8: a same-boot segment that is already full at open is left as it is, and
 * the first append lands in the next segment. */
static void test_full_at_open(void)
{
    struct store_open_info info;
    struct store *st;
    struct print p;
    struct record r = mk(STORE_SEG_MAX + 1, 1800000000000000000LL, 2150, 5500, 0);

    setup();
    craft_segment(SEG1, 1, STORE_SEG_MAX);
    p = fingerprint(SEG1);
    st = open_bme(&info);
    CHECK(info.counter == 2 && info.created == 1 && info.new_boot == 0 && info.records == 0);
    CHECK(store_append(st, &r) == STORE_OK);
    CHECK(store_counter(st) == 2);
    CHECK(file_size(SEG2) == HDR + REC);
    CHECK(same_print(SEG1, p));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* A9: rotation takes 1 + the highest counter on disk, so it never reuses a
 * quarantined segment's number. */
static void test_rotation_skips_quarantined(void)
{
    struct store_open_info info;
    struct store *st;
    struct record r = mk(1, 1800000000000000000LL, 2150, 5500, 0);

    setup();
    craft_segment("bme280.000003.mwlog", 3, STORE_SEG_MAX - 1);
    write_file("bme280.000004.mwlog.corrupt-1", "garbage", 7);
    st = open_bme(&info);
    CHECK(info.counter == 3 && info.created == 0);
    CHECK(store_append(st, &r) == STORE_OK); /* fills 3 */
    CHECK(store_counter(st) == 3);
    CHECK(store_append(st, &r) == STORE_OK); /* rotates */
    CHECK(store_counter(st) == 5);
    CHECK(exists("bme280.000005.mwlog"));
    CHECK(!exists("bme280.000004.mwlog"));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* A10: if the next segment cannot be created, nothing is appended anywhere,
 * the full segment is untouched, and the handle refuses further appends. */
static void test_rotation_failure(void)
{
    struct store *st;
    struct print p;
    char p_tmp[128];
    struct record r = mk(STORE_SEG_MAX, 1800000000000000000LL, 2150, 5500, 0);

    setup();
    craft_segment(SEG1, 1, STORE_SEG_MAX - 1);
    st = open_bme(NULL);
    CHECK(store_append(st, &r) == STORE_OK); /* segment 1 now full */
    p = fingerprint(SEG1);

    /* A directory where the new segment's .tmp must go makes creation fail.
     * This stands in for an unwritable dir, since the container runs as root. */
    path(p_tmp, sizeof p_tmp, "bme280.000002.mwlog.tmp");
    CHECK(mkdir(p_tmp, 0755) == 0);

    r.seq = STORE_SEG_MAX + 1;
    CHECK(store_append(st, &r) == STORE_EIO);
    CHECK(same_print(SEG1, p));
    CHECK(!exists(SEG2));
    CHECK(store_counter(st) == 1);
    CHECK(store_append(st, &r) == STORE_EIO); /* failed handle: no retry */
    CHECK(same_print(SEG1, p));
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

int main(void)
{
    log_init("test_store_append", "store");
    load_boot_id();

    test_roundtrip();
    test_caller_record_const();
    test_refused_records();
    test_flags_preserved();
    test_arrival_order();
    test_short_write_rollback();
    test_rotation();
    test_full_at_open();
    test_rotation_skips_quarantined();
    test_rotation_failure();

    if (failures == 0)
        printf("test_store_append: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
