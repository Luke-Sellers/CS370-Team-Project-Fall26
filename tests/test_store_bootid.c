/* test_store_bootid -- the per-boot rule when the boot id is known, unknown or
 * malformed. This binary builds store.c with -DBOOT_ID_PATH pointing at a
 * file the test writes, so every case is deterministic and none touches /proc.
 * Rule under test: an unknown boot id is stored as zeros and never counts as
 * "same boot", so every open starts a new segment. */
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

#ifndef BOOT_ID_PATH
#error "build with -DBOOT_ID_PATH=<file the test controls>"
#endif

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static char g_dir[64];
static const uint8_t ZERO[16];

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

/* Put text at BOOT_ID_PATH, or remove it when text is NULL. */
static void set_boot_id(const char *text)
{
    if (text == NULL) {
        if (unlink(BOOT_ID_PATH) < 0 && access(BOOT_ID_PATH, F_OK) == 0) {
            perror(BOOT_ID_PATH);
            exit(2);
        }
        return;
    }
    FILE *f = fopen(BOOT_ID_PATH, "w");
    if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
        perror(BOOT_ID_PATH);
        exit(2);
    }
}

static long read_file(const char *name, void *buf, size_t cap)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
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
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, data, len) != (ssize_t)len || close(fd) < 0) {
        perror(p);
        exit(2);
    }
}

/* Header of a segment on disk; zeroed if it cannot be read. */
static struct store_header header_of(const char *name)
{
    struct store_header h;
    memset(&h, 0, sizeof h);
    if (read_file(name, &h, sizeof h) != (long)sizeof h)
        memset(&h, 0xEE, sizeof h); /* never mistaken for a zero boot id */
    return h;
}

/* Open and close once; returns the info for checking. */
static struct store_open_info open_once(void)
{
    struct store *st = NULL;
    struct store_open_info info;

    memset(&info, 0, sizeof info);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(store_close(st) == STORE_OK);
    return info;
}

/* Unknown boot id: three opens make three segments, each header zeroed. */
static void expect_unknown(const char *boot_text)
{
    struct store_open_info i;
    struct store_header h;

    setup();
    set_boot_id(boot_text);

    i = open_once();
    CHECK(i.counter == 1 && i.created == 1 && i.new_boot == 0);
    h = header_of("bme280.000001.mwlog");
    CHECK(memcmp(h.boot_id, ZERO, 16) == 0);

    i = open_once();
    CHECK(i.counter == 2 && i.created == 1 && i.new_boot == 1);
    h = header_of("bme280.000002.mwlog");
    CHECK(memcmp(h.boot_id, ZERO, 16) == 0);

    i = open_once();
    CHECK(i.counter == 3 && i.created == 1 && i.new_boot == 1);

    teardown();
}

/* B1: the boot id file does not exist. */
static void test_missing(void)
{
    expect_unknown(NULL);
}

/* B2: the file holds something that is not a UUID. */
static void test_garbage(void)
{
    expect_unknown("not-a-uuid\n");
}

/* B3: almost-UUIDs are unknown too: one digit short, one long, uppercase
 * (the kernel writes lowercase; anything else is not what we expect). */
static void test_near_misses(void)
{
    expect_unknown("01234567-89ab-cdef-0123-456789abcde\n");
    expect_unknown("01234567-89ab-cdef-0123-456789abcdef0\n");
    expect_unknown("01234567-89AB-CDEF-0123-456789ABCDEF\n");
    expect_unknown(""); /* empty file */
}

/* B4 + B5: a fixed valid id is parsed byte for byte and reused within the
 * boot; a different id afterwards means a new boot and a new segment. */
static void test_known_then_changed(void)
{
    static const uint8_t id1[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                     0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const uint8_t id2[16] = { 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
                                     0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 };
    struct store_open_info i;
    struct store_header h;
    unsigned char before[64], after[64];
    long n;

    setup();
    set_boot_id("01234567-89ab-cdef-0123-456789abcdef\n");
    i = open_once();
    CHECK(i.counter == 1 && i.created == 1);
    h = header_of("bme280.000001.mwlog");
    CHECK(memcmp(h.boot_id, id1, 16) == 0);

    i = open_once(); /* B4: same boot, same segment */
    CHECK(i.counter == 1 && i.created == 0 && i.new_boot == 0);

    n = read_file("bme280.000001.mwlog", before, sizeof before);
    set_boot_id("fedcba98-7654-3210-fedc-ba9876543210\n");
    i = open_once(); /* B5: new boot */
    CHECK(i.counter == 2 && i.created == 1 && i.new_boot == 1);
    h = header_of("bme280.000002.mwlog");
    CHECK(memcmp(h.boot_id, id2, 16) == 0);
    CHECK(read_file("bme280.000001.mwlog", after, sizeof after) == n);
    CHECK(n == 32 && memcmp(before, after, 32) == 0);

    teardown();
}

/* B6: a latest segment whose header holds a zero boot id is not "this boot"
 * even when the current boot id is also unknown (zero == zero is no match). */
static void test_zero_never_matches(void)
{
    struct store_header h;
    struct store_open_info i;
    unsigned char before[64], after[64];

    setup();
    set_boot_id(NULL);
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.sensor = SENSOR_BME280;
    h.record_size = sizeof(struct record);
    h.counter = 1;
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    write_file("bme280.000001.mwlog", &h, sizeof h);

    CHECK(read_file("bme280.000001.mwlog", before, sizeof before) == 32);
    i = open_once();
    CHECK(i.counter == 2 && i.created == 1 && i.new_boot == 1);
    CHECK(read_file("bme280.000001.mwlog", after, sizeof after) == 32);
    CHECK(memcmp(before, after, 32) == 0);

    teardown();
}

int main(void)
{
    log_init("test_store_bootid", "store");

    test_missing();
    test_garbage();
    test_near_misses();
    test_known_then_changed();
    test_zero_never_matches();
    set_boot_id(NULL);

    if (failures == 0)
        printf("test_store_bootid: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
