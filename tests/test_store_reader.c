/* test_store_reader -- step 5 of docs/store-plan.md: the multi-segment reader.
 * It walks a sensor's segments in counter order (live, then .corrupt-1, -2...
 * for one counter), salvages a quarantined file up to its first bad record,
 * never resyncs past a bad record, and never changes a file. Segments are
 * crafted byte by byte. */
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
#define T0  1700000000000000000LL
#define DT  10000000000LL

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

static void append_bytes(const char *name, const void *data, size_t len)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_WRONLY | O_APPEND);
    if (fd < 0 || write(fd, data, len) != (ssize_t)len || close(fd) < 0) {
        perror(p);
        exit(2);
    }
}

/* Length + CRC of a whole file, to prove the reader changed nothing. */
static uint32_t print_of(const char *name, long *len)
{
    unsigned char buf[HDR + 128 * REC];
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_RDONLY);
    *len = -1;
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    *len = (long)n;
    return n < 0 ? 0 : store_crc32(buf, (size_t)n);
}

static struct record mk(uint8_t sensor, uint32_t seq)
{
    struct record r = {
        .magic = RECORD_MAGIC,
        .sensor = sensor,
        .seq = seq,
        .t_ns = T0 + (int64_t)seq * DT,
        .a = 2150,
        .b = 5500,
    };
    r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
    return r;
}

/* Header for `counter`/`boot`, then nrec valid records with seq seq0, seq0+1... */
static void build(struct image *im, uint8_t sensor, uint32_t counter, const uint8_t boot[16],
                  int nrec, uint32_t seq0)
{
    struct store_header h;

    memset(im, 0, sizeof *im);
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.sensor = sensor;
    h.record_size = sizeof(struct record);
    h.counter = counter;
    memcpy(h.boot_id, boot, 16);
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    memcpy(im->b, &h, sizeof h);
    for (int i = 0; i < nrec; i++) {
        struct record r = mk(sensor, seq0 + (uint32_t)i);
        memcpy(im->b + HDR + i * REC, &r, sizeof r);
    }
    im->len = HDR + nrec * REC;
}

static void spoil(struct image *im, int i)
{
    im->b[HDR + i * REC + offsetof(struct record, a)] ^= 0x01;
}

static void tear(struct image *im, int n)
{
    memset(im->b + im->len, 0xAB, (size_t)n);
    im->len += n;
}

static void boot_n(uint8_t out[16], uint8_t tag)
{
    memcpy(out, g_boot, 16);
    out[15] ^= tag; /* tag 0 = this boot */
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

/* What a reader walk returned, in order. */
struct walk {
    int n;
    uint32_t seq[256];
    struct store_rec_src src[256];
    int end_rc;    /* 0 when the walk ended cleanly */
    int again_rc;  /* a further next() after the end */
    struct store_reader_stats st;
};

/* Open a reader on the test dir and drain it. Reports FAIL rather than
 * aborting if the reader cannot be opened. */
static void walk_all(struct walk *w)
{
    struct store_reader *rd = NULL;
    struct record r;
    struct store_rec_src s;
    int rc;

    memset(w, 0, sizeof *w);
    w->end_rc = -100;
    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    if (rd == NULL)
        return;
    while ((rc = store_reader_next(rd, &r, &s)) == 1) {
        if (w->n < 256) {
            w->seq[w->n] = r.seq;
            w->src[w->n] = s;
        }
        w->n++;
        if (w->n > 1000)
            break; /* a resync bug could loop; never hang the suite */
    }
    w->end_rc = rc;
    w->again_rc = store_reader_next(rd, &r, &s);
    store_reader_stats(rd, &w->st);
    CHECK(store_reader_close(rd) == STORE_OK);
}

/* seq values of w[from .. from+n) are first, first+1, ... and all came from
 * `counter` with the given quarantine flag. */
static int run_is(const struct walk *w, int from, int n, uint32_t first, uint32_t counter,
                  int quarantined)
{
    if (from + n > w->n)
        return 0;
    for (int i = 0; i < n; i++) {
        if (w->seq[from + i] != first + (uint32_t)i || w->src[from + i].counter != counter ||
            w->src[from + i].quarantined != quarantined)
            return 0;
    }
    return 1;
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

/* D1: segments 1, 2, 10 come back in counter order whatever order they were
 * created in, each record tagged with its segment's counter and boot id;
 * other sensors' files are not read. */
static void test_order_and_source(void)
{
    struct image im;
    struct walk w;
    uint8_t b1[16], b2[16], b3[16];

    setup();
    boot_n(b1, 1);
    boot_n(b2, 2);
    boot_n(b3, 0);
    build(&im, SENSOR_BME280, 10, b3, 4, 6);
    write_image("bme280.000010.mwlog", &im);
    build(&im, SENSOR_BME280, 1, b1, 3, 1);
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 2, b2, 2, 4);
    write_image("bme280.000002.mwlog", &im);
    build(&im, SENSOR_MLX90614, 5, b3, 7, 500);
    write_image("mlx90614.000005.mwlog", &im);

    walk_all(&w);
    CHECK(w.n == 9);
    CHECK(run_is(&w, 0, 3, 1, 1, 0));
    CHECK(run_is(&w, 3, 2, 4, 2, 0));
    CHECK(run_is(&w, 5, 4, 6, 10, 0));
    CHECK(memcmp(w.src[0].boot_id, b1, 16) == 0);
    CHECK(memcmp(w.src[3].boot_id, b2, 16) == 0);
    CHECK(memcmp(w.src[5].boot_id, b3, 16) == 0);
    CHECK(w.end_rc == 0 && w.again_rc == 0);
    CHECK(w.st.segments == 3 && w.st.quarantined_files == 0);
    CHECK(w.st.bad_header == 0 && w.st.corrupt_stops == 0 && w.st.salvaged_records == 0);
    teardown();
}

/* D2: a partial record or a bad last record just ends that segment (a write
 * in progress, or a torn one); the walk continues and no file changes. */
static void test_tail_ends_segment(void)
{
    struct image im;
    struct walk w;
    long l1, l2, l1b, l2b;
    uint32_t p1, p2;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 5, 1);
    tear(&im, 7);
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 5, 6);
    spoil(&im, 4);
    write_image("bme280.000002.mwlog", &im);
    build(&im, SENSOR_BME280, 3, g_boot, 2, 10);
    write_image("bme280.000003.mwlog", &im);
    p1 = print_of("bme280.000001.mwlog", &l1);
    p2 = print_of("bme280.000002.mwlog", &l2);

    walk_all(&w);
    CHECK(w.n == 11);
    CHECK(run_is(&w, 0, 5, 1, 1, 0));
    CHECK(run_is(&w, 5, 4, 6, 2, 0));
    CHECK(run_is(&w, 9, 2, 10, 3, 0));
    CHECK(w.end_rc == 0);
    CHECK(w.st.corrupt_stops == 0);
    CHECK(print_of("bme280.000001.mwlog", &l1b) == p1 && l1b == l1);
    CHECK(print_of("bme280.000002.mwlog", &l2b) == p2 && l2b == l2);
    teardown();
}

/* D3: a sealed segment with a bad record mid-file gives the records before it,
 * skips the rest (no resync), counts the stop, and the walk moves on. */
static void test_mid_segment_stop(void)
{
    struct image im;
    struct walk w;
    long l, lb;
    uint32_t p;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 10, 1);
    spoil(&im, 4);
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 3, 100);
    write_image("bme280.000002.mwlog", &im);
    p = print_of("bme280.000001.mwlog", &l);

    walk_all(&w);
    CHECK(w.n == 7);
    CHECK(run_is(&w, 0, 4, 1, 1, 0));
    CHECK(run_is(&w, 4, 3, 100, 2, 0));
    CHECK(w.st.corrupt_stops == 1);
    CHECK(print_of("bme280.000001.mwlog", &lb) == p && lb == l);
    teardown();
}

/* D4: segments with a bad header (CRC, counter mismatch, too short, another
 * version) are skipped and counted; the rest are read. */
static void test_bad_headers(void)
{
    struct image im;
    struct walk w;
    struct store_header h;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 3, 1);
    im.b[offsetof(struct store_header, boot_id)] ^= 1;
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 99, g_boot, 3, 10); /* header says 99 */
    write_image("bme280.000002.mwlog", &im);
    build(&im, SENSOR_BME280, 3, g_boot, 0, 0);
    im.len = 10;
    write_image("bme280.000003.mwlog", &im);
    build(&im, SENSOR_BME280, 4, g_boot, 3, 20);
    memcpy(&h, im.b, sizeof h);
    h.version = 2;
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    memcpy(im.b, &h, sizeof h);
    write_image("bme280.000004.mwlog", &im);
    build(&im, SENSOR_BME280, 5, g_boot, 2, 30);
    write_image("bme280.000005.mwlog", &im);

    walk_all(&w);
    CHECK(w.n == 2);
    CHECK(run_is(&w, 0, 2, 30, 5, 0));
    CHECK(w.st.bad_header == 4 && w.st.segments == 5);
    CHECK(w.end_rc == 0);
    teardown();
}

/* D5: a quarantined file is salvaged up to its first bad record, tagged as
 * quarantined; records after the bad one are NOT returned; nothing changes. */
static void test_salvage(void)
{
    struct image im;
    struct walk w;
    long l, lb;
    uint32_t p;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 3, 1);
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 16, 100); /* 10 good, bad 11th, 5 good */
    spoil(&im, 10);
    write_image("bme280.000002.mwlog.corrupt-1", &im);
    build(&im, SENSOR_BME280, 3, g_boot, 2, 200);
    write_image("bme280.000003.mwlog", &im);
    p = print_of("bme280.000002.mwlog.corrupt-1", &l);

    walk_all(&w);
    CHECK(w.n == 15);
    CHECK(run_is(&w, 0, 3, 1, 1, 0));
    CHECK(run_is(&w, 3, 10, 100, 2, 1));
    CHECK(run_is(&w, 13, 2, 200, 3, 0));
    for (int i = 0; i < w.n && i < 256; i++)
        CHECK(w.seq[i] < 111 || w.seq[i] >= 200); /* 111..115 never surface */
    CHECK(w.st.salvaged_records == 10 && w.st.quarantined_files == 1);
    CHECK(w.st.corrupt_stops == 0); /* that count is for live/sealed segments */
    CHECK(print_of("bme280.000002.mwlog.corrupt-1", &lb) == p && lb == l);
    teardown();
}

/* D5a: a quarantined file whose only damage is a partial tail gives up every
 * whole record. */
static void test_salvage_partial_only(void)
{
    struct image im;
    struct walk w;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 6, 1);
    tear(&im, 19);
    write_image("bme280.000001.mwlog.corrupt-1", &im);
    walk_all(&w);
    CHECK(w.n == 6 && run_is(&w, 0, 6, 1, 1, 1));
    CHECK(w.st.salvaged_records == 6);
    teardown();
}

/* D5b: a quarantined file with a bad header, or shorter than a header, gives
 * up nothing; it is counted and the walk continues. */
static void test_salvage_bad_header(void)
{
    struct image im;
    struct walk w;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 4, 1);
    im.b[0] ^= 1; /* magic */
    write_image("bme280.000001.mwlog.corrupt-1", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 0, 0);
    im.len = 5;
    write_image("bme280.000002.mwlog.corrupt-1", &im);
    build(&im, SENSOR_BME280, 3, g_boot, 2, 50);
    write_image("bme280.000003.mwlog", &im);

    walk_all(&w);
    CHECK(w.n == 2 && run_is(&w, 0, 2, 50, 3, 0));
    CHECK(w.st.salvaged_records == 0 && w.st.quarantined_files == 2 && w.st.bad_header == 2);
    teardown();
}

/* D5c: within one counter: live first, then .corrupt-1, -2, ..., -10 by number. */
static void test_order_within_counter(void)
{
    struct image im;
    struct walk w;

    setup();
    build(&im, SENSOR_BME280, 2, g_boot, 1, 40);
    write_image("bme280.000002.mwlog.corrupt-10", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 1, 30);
    write_image("bme280.000002.mwlog.corrupt-2", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 1, 20);
    write_image("bme280.000002.mwlog.corrupt-1", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 1, 10);
    write_image("bme280.000002.mwlog", &im);
    build(&im, SENSOR_BME280, 1, g_boot, 1, 5);
    write_image("bme280.000001.mwlog.corrupt-1", &im);

    walk_all(&w);
    CHECK(w.n == 5);
    CHECK(w.n == 5 && w.seq[0] == 5 && w.seq[1] == 10 && w.seq[2] == 20 && w.seq[3] == 30 &&
          w.seq[4] == 40);
    CHECK(w.n == 5 && w.src[1].quarantined == 0 && w.src[2].quarantined == 1);
    teardown();
}

/* D5d: end to end with the writer. store_open quarantines a segment; the
 * reader salvages exactly the records before the reported bad offset, then
 * reads what was appended to the fresh segment. */
static void test_writer_quarantine_then_read(void)
{
    struct image im;
    struct walk w;
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct record r;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 10, 1);
    spoil(&im, 6);
    write_image("bme280.000001.mwlog", &im);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.quarantined == 1 && info.counter == 2);
    r = mk(SENSOR_BME280, 11);
    if (st != NULL) {
        CHECK(store_append(st, &r) == STORE_OK);
        r = mk(SENSOR_BME280, 12);
        CHECK(store_append(st, &r) == STORE_OK);
        CHECK(store_close(st) == STORE_OK);
    }

    walk_all(&w);
    CHECK(w.st.salvaged_records == (info.bad_offset - HDR) / REC);
    CHECK(w.n == 8);
    CHECK(run_is(&w, 0, 6, 1, 1, 1));
    CHECK(run_is(&w, 6, 2, 11, 2, 0));
    teardown();
}

/* D6: empty dir is an empty walk; bad arguments and a missing dir fail cleanly. */
static void test_empty_and_errors(void)
{
    struct store_reader *rd = (struct store_reader *)1;
    struct walk w;
    char p[128];

    setup();
    walk_all(&w);
    CHECK(w.n == 0 && w.end_rc == 0 && w.again_rc == 0);
    CHECK(w.st.segments == 0 && w.st.quarantined_files == 0);

    CHECK(store_reader_open(NULL, SENSOR_BME280, &rd) == STORE_EINVAL);
    CHECK(rd == NULL);
    CHECK(store_reader_open(g_dir, (enum sensor_id)7, &rd) == STORE_EINVAL);
    CHECK(store_reader_open(g_dir, SENSOR_BME280, NULL) == STORE_EINVAL);
    path(p, sizeof p, "nope");
    rd = (struct store_reader *)1;
    CHECK(store_reader_open(p, SENSOR_BME280, &rd) == STORE_EIO);
    CHECK(rd == NULL);
    CHECK(store_reader_close(NULL) == STORE_OK);
    teardown();
}

/* D7: the reader works beside a live writer. It takes no lock (the writer is
 * open throughout), and a write in flight at the end of the live segment
 * (a half record) is not an error and not a corrupt stop. */
static void test_beside_writer(void)
{
    struct store *st = NULL;
    struct store_reader *rd = NULL;
    struct record r, got;
    struct store_rec_src s;
    struct store_reader_stats stats;
    unsigned char half[16];
    int n = 0, rc, bad = 0;

    setup();
    CHECK(store_open(g_dir, SENSOR_BME280, &st, NULL) == STORE_OK);
    for (uint32_t i = 1; st != NULL && i <= 5; i++) {
        r = mk(SENSOR_BME280, i);
        CHECK(store_append(st, &r) == STORE_OK);
    }
    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    memset(half, 0x5A, sizeof half);
    append_bytes("bme280.000001.mwlog", half, sizeof half); /* write in flight */
    while (rd != NULL && (rc = store_reader_next(rd, &got, &s)) == 1) {
        bad += store_record_check(&got, SENSOR_BME280) != STORE_OK || got.seq != (uint32_t)n + 1;
        n++;
    }
    CHECK(n == 5 && bad == 0);
    if (rd != NULL) {
        store_reader_stats(rd, &stats);
        CHECK(stats.corrupt_stops == 0);
        CHECK(store_reader_close(rd) == STORE_OK);
    }
    CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* D7b: a file renamed between the snapshot and the walk reaching it is
 * counted as vanished, not an error. */
static void test_vanished(void)
{
    struct image im;
    struct store_reader *rd = NULL;
    struct record r;
    struct store_reader_stats stats;
    char from[128], to[128];
    int n = 0;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 2, 1);
    write_image("bme280.000001.mwlog", &im);
    build(&im, SENSOR_BME280, 2, g_boot, 2, 3);
    write_image("bme280.000002.mwlog", &im);
    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    path(from, sizeof from, "bme280.000002.mwlog");
    path(to, sizeof to, "elsewhere");
    CHECK(rename(from, to) == 0);
    while (rd != NULL && store_reader_next(rd, &r, NULL) == 1)
        n++;
    CHECK(n == 2);
    if (rd != NULL) {
        store_reader_stats(rd, &stats);
        CHECK(stats.vanished == 1 && stats.segments == 1);
        CHECK(store_reader_close(rd) == STORE_OK);
    }
    teardown();
}

/* D8: no fd leaks, whether the reader is closed mid-segment or at the end. */
static void test_fds(void)
{
    struct image im;
    struct store_reader *rd = NULL;
    struct record r;
    int before[256], n0;

    setup();
    build(&im, SENSOR_BME280, 1, g_boot, 5, 1);
    write_image("bme280.000001.mwlog", &im);
    n0 = list_fds(before, 256);

    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    CHECK(rd != NULL && store_reader_next(rd, &r, NULL) == 1); /* mid-segment */
    CHECK(store_reader_close(rd) == STORE_OK);
    CHECK(list_fds(before, 256) == n0);

    rd = NULL;
    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    while (rd != NULL && store_reader_next(rd, &r, NULL) == 1)
        ;
    CHECK(store_reader_close(rd) == STORE_OK);
    CHECK(list_fds(before, 256) == n0);
    teardown();
}

int main(void)
{
    log_init("test_store_reader", "store");
    load_boot_id();

    test_order_and_source();
    test_tail_ends_segment();
    test_mid_segment_stop();
    test_bad_headers();
    test_salvage();
    test_salvage_partial_only();
    test_salvage_bad_header();
    test_order_within_counter();
    test_writer_quarantine_then_read();
    test_empty_and_errors();
    test_beside_writer();
    test_vanished();
    test_fds();

    if (failures == 0)
        printf("test_store_reader: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
