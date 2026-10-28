/* test_stored -- step 6 of docs/store-plan.md: the store daemon, end to end.
 * Spawns the real binaries (capture --fake --binary | store) with fork/exec
 * and real pipes, then checks exit codes, the log on stderr, and what landed
 * on disk via the reader.
 *
 * usage: test_stored <build-dir>   (where capture and store were built) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

static char g_cap[256], g_store[256];
static char g_dir[64];
static uint8_t g_boot[16];

/* ---- fixtures ---- */

static void setup(void)
{
    strcpy(g_dir, "/tmp/mwstored.XXXXXX");
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

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
}

static void cloexec(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        perror("fcntl");
        exit(2);
    }
}

static void make_pipe(int p[2])
{
    if (pipe(p) < 0) {
        perror("pipe");
        exit(2);
    }
    cloexec(p[0]);
    cloexec(p[1]);
}

/* A log file in the test dir for a child's stderr. */
static int open_log(const char *name)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        perror(p);
        exit(2);
    }
    return fd;
}

/* fork + exec with the given stdin/stdout/stderr (-1 = inherit). All other
 * test fds are close-on-exec, so pipes see EOF when they should. */
static pid_t spawn(const char *const argv[], int in, int out, int err)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL); /* the test ignores it; children must not inherit that */
        if ((in >= 0 && dup2(in, 0) < 0) || (out >= 0 && dup2(out, 1) < 0) ||
            (err >= 0 && dup2(err, 2) < 0))
            _exit(127);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    return pid;
}

/* Exit code of pid, -signal if killed, or -1000 if it had to be killed
 * after timeout_ms (a hang is a failure, never a stuck suite). */
static int wait_exit(pid_t pid, long timeout_ms)
{
    int status;

    for (long waited = 0;; waited += 10) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0) {
            perror("waitpid");
            return -2000;
        }
        if (waited >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1000;
        }
        sleep_ms(10);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? -WTERMSIG(status) : -3000;
}

static long read_log(const char *name, char *buf, size_t cap)
{
    char p[128];
    path(p, sizeof p, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = '\0';
    return (long)n;
}

static int count_in_log(const char *name, const char *needle)
{
    static char buf[1 << 16];
    int n = 0;
    if (read_log(name, buf, sizeof buf) < 0)
        return -1;
    for (const char *p = buf; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* Poll the log until needle appears; 1 if it did within timeout_ms. */
static int wait_for_log(const char *name, const char *needle, long timeout_ms)
{
    for (long waited = 0; waited <= timeout_ms; waited += 20) {
        if (count_in_log(name, needle) > 0)
            return 1;
        sleep_ms(20);
    }
    return 0;
}

static void write_all_fd(int fd, const void *data, size_t len)
{
    if (write(fd, data, len) != (ssize_t)len) {
        perror("write");
        exit(2);
    }
}

static struct record mk(uint8_t sensor, uint32_t seq)
{
    struct record r = {
        .magic = RECORD_MAGIC,
        .sensor = sensor,
        .seq = seq,
        .t_ns = 1700000000000000000LL + (int64_t)seq * 10000000000LL,
        .a = 2150,
        .b = 5500,
    };
    return r;
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

/* Everything on disk for bme280, via the reader. */
struct stored {
    int n;
    struct record r[512];
    struct store_rec_src src[512];
    struct store_reader_stats st;
};

static void drain(struct stored *s)
{
    struct store_reader *rd = NULL;
    struct record r;
    struct store_rec_src src;

    memset(s, 0, sizeof *s);
    CHECK(store_reader_open(g_dir, SENSOR_BME280, &rd) == STORE_OK);
    if (rd == NULL)
        return;
    while (store_reader_next(rd, &r, &src) == 1) {
        if (s->n < 512) {
            s->r[s->n] = r;
            s->src[s->n] = src;
        }
        s->n++;
    }
    store_reader_stats(rd, &s->st);
    CHECK(store_reader_close(rd) == STORE_OK);
}

/* capture <args...> | store --sensor bme280 --dir g_dir [extra]; returns the
 * two pids so the caller decides when and how they end. */
struct pair {
    pid_t cap, store;
};

static struct pair start_pipeline(const char *const cap_args[], const char *log,
                                  const char *heartbeat_s)
{
    const char *cargv[16];
    const char *sargv[] = { g_store, "--sensor", "bme280", "--dir", g_dir,
                            heartbeat_s ? "--heartbeat-s" : NULL, heartbeat_s, NULL };
    int p[2], clog, slog, i = 0;
    struct pair pr;

    cargv[i++] = g_cap;
    for (int j = 0; cap_args[j] != NULL && i < 15; j++)
        cargv[i++] = cap_args[j];
    cargv[i] = NULL;

    make_pipe(p);
    clog = open_log("capture.err");
    slog = open_log(log);
    pr.cap = spawn(cargv, -1, p[1], clog);
    pr.store = spawn(sargv, p[0], -1, slog);
    close(p[0]);
    close(p[1]);
    close(clog);
    close(slog);
    return pr;
}

/* store reading from a pipe the test writes; returns the write end. */
static pid_t start_store_fed(const char *log, const char *heartbeat_s, int *wr)
{
    const char *sargv[] = { g_store, "--sensor", "bme280", "--dir", g_dir,
                            heartbeat_s ? "--heartbeat-s" : NULL, heartbeat_s, NULL };
    int p[2], slog;
    pid_t pid;

    make_pipe(p);
    slog = open_log(log);
    pid = spawn(sargv, p[0], -1, slog);
    close(p[0]);
    close(slog);
    *wr = p[1];
    return pid;
}

static int run_store_args(const char *const argv[], const char *log)
{
    int slog = open_log(log);
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pid_t pid = spawn(argv, devnull, -1, slog);
    close(slog);
    close(devnull);
    return wait_exit(pid, 10000);
}

/* ---- tests ---- */

/* S1: 50 fake records flow through and are stored, all labeled SYNTHETIC;
 * input closing is a clean stop (exit 0). */
static void test_fake_pipeline(void)
{
    static const char *const cap[] = { "--sensor", "bme280", "--fake", "--binary",
                                       "--interval-ms", "1", "--count", "50", NULL };
    struct pair pr;
    struct stored s;
    int bad = 0;

    setup();
    pr = start_pipeline(cap, "store.err", NULL);
    CHECK(wait_exit(pr.cap, 10000) == 0);
    CHECK(wait_exit(pr.store, 10000) == 0);
    drain(&s);
    CHECK(s.n == 50);
    for (int i = 0; i < s.n && i < 512; i++)
        bad += s.r[i].seq != (uint32_t)i + 1 || !(s.r[i].flags & REC_SYNTHETIC) ||
               (s.r[i].flags & REC_READ_ERR) || s.src[i].counter != 1;
    CHECK(bad == 0);
    CHECK(count_in_log("store.err", "heartbeat") >= 1);
    CHECK(count_in_log("store.err", "stop reason=input closed exit=0") == 1);
    teardown();
}

/* S2: a sensor that drops out is recorded, not hidden: READ_ERR records with
 * zeroed values at the dropped seqs, everything else normal. */
static void test_read_errors_stored(void)
{
    static const char *const cap[] = { "--sensor", "bme280", "--fake", "--binary",
                                       "--interval-ms", "1", "--count", "20",
                                       "--fake-drop", "5:3", NULL };
    struct pair pr;
    struct stored s;
    int bad = 0;

    setup();
    pr = start_pipeline(cap, "store.err", NULL);
    CHECK(wait_exit(pr.cap, 10000) == 0);
    CHECK(wait_exit(pr.store, 10000) == 0);
    drain(&s);
    CHECK(s.n == 20);
    for (int i = 0; i < s.n && i < 512; i++) {
        int dropped = s.r[i].seq >= 5 && s.r[i].seq <= 7;
        int err = (s.r[i].flags & REC_READ_ERR) != 0;
        bad += dropped != err || (err && (s.r[i].a != 0 || s.r[i].b != 0));
    }
    CHECK(bad == 0);
    teardown();
}

/* S3: capture without --binary sends text; storage refuses to guess where a
 * record starts: exit 6, nothing stored, and the log says why. */
static void test_text_input_refused(void)
{
    static const char *const cap[] = { "--sensor", "bme280", "--fake",
                                       "--interval-ms", "1", "--count", "5", NULL };
    struct pair pr;
    struct stored s;

    setup();
    pr = start_pipeline(cap, "store.err", NULL);
    CHECK(wait_exit(pr.store, 10000) == 6);
    wait_exit(pr.cap, 10000); /* 0 or 3 (EPIPE), depending on timing */
    drain(&s);
    CHECK(s.n == 0);
    CHECK(count_in_log("store.err", "--binary") >= 1);
    teardown();
}

/* S4: the other sensor's records into this sensor's store: exit 6. */
static void test_wrong_sensor_refused(void)
{
    static const char *const cap[] = { "--sensor", "mlx90614", "--fake", "--binary",
                                       "--interval-ms", "1", "--count", "5", NULL };
    struct pair pr;
    struct stored s;

    setup();
    pr = start_pipeline(cap, "store.err", NULL);
    CHECK(wait_exit(pr.store, 10000) == 6);
    wait_exit(pr.cap, 10000);
    drain(&s);
    CHECK(s.n == 0);
    teardown();
}

/* S5: input ending mid-record: the whole record is kept, the partial one is
 * dropped and logged, and it is still a clean stop. */
static void test_partial_at_eof(void)
{
    struct record r = mk(SENSOR_BME280, 1);
    struct stored s;
    unsigned char junk[8];
    int wr;
    pid_t pid;

    setup();
    pid = start_store_fed("store.err", NULL, &wr);
    memset(junk, 0x11, sizeof junk);
    write_all_fd(wr, &r, sizeof r);
    write_all_fd(wr, junk, sizeof junk);
    close(wr);
    CHECK(wait_exit(pid, 10000) == 0);
    drain(&s);
    CHECK(s.n == 1 && s.r[0].seq == 1);
    CHECK(count_in_log("store.err", "partial record of 8 bytes") == 1);
    teardown();
}

/* S6: SIGTERM mid-stream is a clean stop (exit 0) and leaves a log that
 * reopens without truncation or quarantine. */
static void test_sigterm(void)
{
    static const char *const cap[] = { "--sensor", "bme280", "--fake", "--binary",
                                       "--interval-ms", "5", NULL };
    struct store *st = NULL;
    struct store_open_info info = { 0 };
    struct pair pr;
    struct stored s;
    int bad = 0;

    setup();
    pr = start_pipeline(cap, "store.err", NULL);
    CHECK(wait_for_log("store.err", "heartbeat", 5000));
    sleep_ms(300);
    kill(pr.store, SIGTERM);
    CHECK(wait_exit(pr.store, 10000) == 0);
    kill(pr.cap, SIGTERM);
    wait_exit(pr.cap, 10000);
    CHECK(count_in_log("store.err", "stop reason=signal exit=0") == 1);

    drain(&s);
    CHECK(s.n > 0);
    for (int i = 0; i < s.n && i < 512; i++)
        bad += s.r[i].seq != (uint32_t)i + 1;
    CHECK(bad == 0);
    CHECK(store_open(g_dir, SENSOR_BME280, &st, &info) == STORE_OK);
    CHECK(info.truncated_bytes == 0 && info.quarantined == 0 && info.counter == 1);
    if (st != NULL)
        CHECK(store_close(st) == STORE_OK);
    teardown();
}

/* S7: with no input at all, heartbeats still come: one at start, then one
 * per --heartbeat-s. Closing the input then stops it cleanly. */
static void test_idle_heartbeat(void)
{
    int wr, n;
    pid_t pid;

    setup();
    pid = start_store_fed("store.err", "1", &wr);
    CHECK(wait_for_log("store.err", "heartbeat", 1000)); /* the start one */
    sleep_ms(2500);
    n = count_in_log("store.err", "heartbeat");
    CHECK(n >= 3 && n <= 4);
    close(wr);
    CHECK(wait_exit(pid, 10000) == 0);
    teardown();
}

/* S8: SIGKILL (no chance to clean up), then a restart in the same boot:
 * the same segment is recovered and reused; nothing is quarantined. */
static void test_sigkill_restart(void)
{
    static const char *const cap_forever[] = { "--sensor", "bme280", "--fake", "--binary",
                                               "--interval-ms", "2", NULL };
    static const char *const cap_ten[] = { "--sensor", "bme280", "--fake", "--binary",
                                           "--interval-ms", "1", "--count", "10", NULL };
    struct pair pr;
    struct stored s;
    int bad = 0, first;

    setup();
    pr = start_pipeline(cap_forever, "store1.err", NULL);
    CHECK(wait_for_log("store1.err", "heartbeat", 5000));
    sleep_ms(200);
    kill(pr.store, SIGKILL);
    CHECK(wait_exit(pr.store, 10000) == -SIGKILL);
    kill(pr.cap, SIGTERM);
    wait_exit(pr.cap, 10000);

    pr = start_pipeline(cap_ten, "store2.err", NULL);
    CHECK(wait_exit(pr.cap, 10000) == 0);
    CHECK(wait_exit(pr.store, 10000) == 0);
    CHECK(count_in_log("store2.err", "segment=000001") >= 1);
    CHECK(count_in_log("store2.err", "created=0") == 1);
    CHECK(count_in_log("store2.err", "quarantined=0") == 1);

    drain(&s);
    first = s.n - 10;
    CHECK(first > 0);
    for (int i = 0; i < s.n && i < 512; i++)
        bad += s.src[i].counter != 1 || s.src[i].quarantined;
    for (int i = 0; first > 0 && i < 10 && first + i < 512; i++)
        bad += s.r[first + i].seq != (uint32_t)i + 1; /* the restarted capture */
    CHECK(bad == 0);
    CHECK(s.st.corrupt_stops == 0 && s.st.quarantined_files == 0);
    teardown();
}

/* S9: a corrupt latest segment does not stop the daemon: it is quarantined
 * with a loud log line, new records go to a fresh segment, exit 0. */
static void test_quarantine_and_continue(void)
{
    unsigned char img[HDR + 10 * REC];
    struct store_header h;
    struct stored s;
    char p[128];
    int wr, fd, bad = 0;
    pid_t pid;

    setup();
    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.sensor = SENSOR_BME280;
    h.record_size = sizeof(struct record);
    h.counter = 1;
    memcpy(h.boot_id, g_boot, 16);
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    memcpy(img, &h, sizeof h);
    for (int i = 0; i < 10; i++) {
        struct record r = mk(SENSOR_BME280, (uint32_t)i + 1);
        r.crc32 = store_crc32(&r, offsetof(struct record, crc32));
        memcpy(img + HDR + i * REC, &r, sizeof r);
    }
    img[HDR + 4 * REC + offsetof(struct record, a)] ^= 1; /* record 5 is bad */
    path(p, sizeof p, "bme280.000001.mwlog");
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, img, sizeof img) != (ssize_t)sizeof img || close(fd) < 0) {
        perror(p);
        exit(2);
    }

    pid = start_store_fed("store.err", NULL, &wr);
    for (uint32_t seq = 101; seq <= 105; seq++) {
        struct record r = mk(SENSOR_BME280, seq);
        write_all_fd(wr, &r, sizeof r);
    }
    close(wr);
    CHECK(wait_exit(pid, 10000) == 0);
    CHECK(count_in_log("store.err", "QUARANTINED") >= 1);
    CHECK(exists("bme280.000001.mwlog.corrupt-1"));

    drain(&s);
    CHECK(s.n == 9);
    for (int i = 0; i < 4 && i < s.n; i++)
        bad += s.r[i].seq != (uint32_t)i + 1 || s.src[i].counter != 1 || !s.src[i].quarantined;
    for (int i = 4; i < 9 && i < s.n; i++)
        bad += s.r[i].seq != (uint32_t)(97 + i) || s.src[i].counter != 2 || s.src[i].quarantined;
    CHECK(bad == 0);
    teardown();
}

/* S10: one writer per sensor: a second daemon on the same dir exits 5. */
static void test_second_daemon_refused(void)
{
    const char *argv2[] = { g_store, "--sensor", "bme280", "--dir", g_dir, NULL };
    int wr;
    pid_t first;

    setup();
    first = start_store_fed("store1.err", NULL, &wr);
    CHECK(wait_for_log("store1.err", "heartbeat", 5000));
    CHECK(run_store_args(argv2, "store2.err") == 5);
    CHECK(count_in_log("store2.err", "lock") >= 1);
    close(wr);
    CHECK(wait_exit(first, 10000) == 0);
    teardown();
}

/* S11: arguments and setup: usage errors exit 1, a missing dir exits 4, and
 * a log in another format version is refused with exit 5, untouched. */
static void test_usage_and_setup(void)
{
    char nope[128], p[128];
    struct store_header h;
    int fd;

    setup();
    path(nope, sizeof nope, "nope");
    {
        const char *a[] = { g_store, NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme999", "--dir", g_dir, NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", "--dir", g_dir,
                            "--heartbeat-s", "0", NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", "--dir", g_dir,
                            "--heartbeat-s", "61", NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", "--dir", g_dir, "--bogus", NULL };
        CHECK(run_store_args(a, "u.err") == 1);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", "--dir", nope, NULL };
        CHECK(run_store_args(a, "u.err") == 4);
    }

    memset(&h, 0, sizeof h);
    h.magic = STORE_MAGIC;
    h.version = 2;
    h.sensor = SENSOR_BME280;
    h.record_size = sizeof(struct record);
    h.counter = 1;
    h.crc32 = store_crc32(&h, offsetof(struct store_header, crc32));
    path(p, sizeof p, "bme280.000001.mwlog");
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, &h, sizeof h) != (ssize_t)sizeof h || close(fd) < 0) {
        perror(p);
        exit(2);
    }
    {
        const char *a[] = { g_store, "--sensor", "bme280", "--dir", g_dir, NULL };
        CHECK(run_store_args(a, "u.err") == 5);
    }
    CHECK(!exists("bme280.000002.mwlog"));
    teardown();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test_stored <build-dir>\n");
        return 2;
    }
    snprintf(g_cap, sizeof g_cap, "%s/capture", argv[1]);
    snprintf(g_store, sizeof g_store, "%s/store", argv[1]);
    if (access(g_cap, X_OK) < 0 || access(g_store, X_OK) < 0) {
        fprintf(stderr, "test_stored: need %s and %s\n", g_cap, g_store);
        return 2;
    }
    log_init("test_stored", "store");
    load_boot_id();
    signal(SIGPIPE, SIG_IGN); /* a dead store makes our write fail, not kill us */

    test_fake_pipeline();
    test_read_errors_stored();
    test_text_input_refused();
    test_wrong_sensor_refused();
    test_partial_at_eof();
    test_sigterm();
    test_idle_heartbeat();
    test_sigkill_restart();
    test_quarantine_and_continue();
    test_second_daemon_refused();
    test_usage_and_setup();

    if (failures == 0)
        printf("test_stored: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
