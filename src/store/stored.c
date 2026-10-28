/* store -- persist one sensor's records from a capture pipe.
 *
 * One process per sensor, beside its capture:
 *   capture --sensor bme280 --binary | store --sensor bme280 --dir /var/lib/moldwatch
 *
 * stdin:  raw 32-byte records from capture --binary. Anything else is a
 *         framing error: storage never guesses where a record starts.
 * stderr: timestamped log -- startup and recovery summary, heartbeats, stop.
 * Every record is fdatasync'd before the next is read (see store.h).
 *
 * Exit codes: 0 stopped cleanly (input closed, SIGINT or SIGTERM),
 * 1 bad arguments, 3 storage write failed, 4 setup failed (including a
 * missing dir), 5 log refused (another format version, another sensor's
 * log, or another store already writing), 6 bad input (not this sensor's
 * record stream, or stdin failed).
 */
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "record.h"
#include "store.h"

enum { EXIT_USAGE = 1, EXIT_OUTPUT = 3, EXIT_SETUP = 4, EXIT_REFUSED = 5, EXIT_INPUT = 6 };

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

struct opts {
    enum sensor_id sensor;
    const char *dir;
    long heartbeat_s;
};

struct counters {
    unsigned long records;  /* appended by this process */
    uint32_t last_seq;
    long append_max_ms;     /* slowest write + fdatasync so far */
};

static void usage(FILE *f)
{
    fprintf(f, "usage: store --sensor bme280|mlx90614 --dir DIR [--heartbeat-s N]\n"
               "  reads capture --binary records on stdin; N is 1..60 (default 60)\n");
}

static int parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    *o = (struct opts){ .heartbeat_s = 60 };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--sensor") == 0 && val) {
            if (strcmp(val, "bme280") == 0)
                o->sensor = SENSOR_BME280;
            else if (strcmp(val, "mlx90614") == 0)
                o->sensor = SENSOR_MLX90614;
            else
                return -1;
            i++;
        } else if (strcmp(arg, "--dir") == 0 && val) {
            o->dir = val;
            i++;
        } else if (strcmp(arg, "--heartbeat-s") == 0 && val) {
            /* At most 60 s: CLAUDE.md wants a heartbeat within 60 s. */
            if (parse_long(val, 1, 60, &o->heartbeat_s) < 0)
                return -1;
            i++;
        } else {
            return -1;
        }
    }
    return (o->sensor && o->dir) ? 0 : -1;
}

static const char *sensor_name(enum sensor_id s)
{
    return s == SENSOR_BME280 ? "bme280" : "mlx90614";
}

static int64_t ts_ms(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000 + t->tv_nsec / 1000000;
}

/* Milliseconds from now until `when` on CLOCK_MONOTONIC, clamped for poll. */
static int ms_until(const struct timespec *when)
{
    struct timespec now;
    int64_t d;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 1000; /* poll again soon rather than spin or sleep forever */
    d = ts_ms(when) - ts_ms(&now);
    if (d < 0)
        return 0;
    return d > 60000 ? 60000 : (int)d;
}

static void heartbeat(const struct timespec *start, const struct store *st,
                      const struct counters *c)
{
    struct timespec now;
    long uptime = -1;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        uptime = (long)(now.tv_sec - start->tv_sec);
    log_msg("heartbeat uptime_s=%ld rss_kb=%ld segment=%06" PRIu32 " records=%lu "
            "last_seq=%" PRIu32 " append_max_ms=%ld",
            uptime, log_rss_kb(), store_counter(st), c->records, c->last_seq,
            c->append_max_ms);
}

static const char *open_error(int rc)
{
    switch (rc) {
    case STORE_EVERSION:
        return "latest segment is another format version";
    case STORE_ESENSOR:
        return "latest segment belongs to the other sensor";
    case STORE_EBUSY:
        return "another store is already writing this sensor's log";
    default:
        return "cannot open the log";
    }
}

/* Append one complete record; returns 0 or an exit code. */
static int store_one(struct store *st, enum sensor_id sensor, const struct record *r,
                     struct counters *c)
{
    struct timespec t0, t1;
    int have_t0 = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;
    int rc = store_append(st, r);

    if (rc == STORE_EINVAL) {
        log_msg("input is not a %s record stream (magic=0x%04x sensor=%u); "
                "is capture running with --binary and the same --sensor?",
                sensor_name(sensor), (unsigned)r->magic, (unsigned)r->sensor);
        return EXIT_INPUT;
    }
    if (rc != STORE_OK) {
        log_msg("storing record #%" PRIu32 " failed; stopping (no retry after a failed write)",
                r->seq);
        return EXIT_OUTPUT;
    }
    if (have_t0 && clock_gettime(CLOCK_MONOTONIC, &t1) == 0) {
        long ms = (long)(ts_ms(&t1) - ts_ms(&t0));
        if (ms > c->append_max_ms)
            c->append_max_ms = ms;
    }
    c->records++;
    c->last_seq = r->seq;
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o;
    struct store *st = NULL;
    struct store_open_info info;
    struct counters c = { 0 };
    unsigned char buf[sizeof(struct record)];
    size_t have = 0;
    struct timespec start, next_hb;
    int rc = 0, orc;
    const char *stop_reason = "input closed";

    if (parse_args(argc, argv, &o) < 0) {
        usage(stderr);
        return EXIT_USAGE;
    }
    log_init("store", sensor_name(o.sensor));

    struct sigaction sa = { .sa_handler = on_signal }; /* no SA_RESTART: wake poll */
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        log_msg("setup failed: sigaction: %s", strerror(errno));
        return EXIT_SETUP;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        log_msg("setup failed: clock_gettime: %s", strerror(errno));
        return EXIT_SETUP;
    }
    log_msg("start dir=%s heartbeat_s=%ld", o.dir, o.heartbeat_s);

    orc = store_open(o.dir, o.sensor, &st, &info);
    if (orc != STORE_OK) {
        log_msg("%s in %s", open_error(orc), o.dir);
        rc = (orc == STORE_EVERSION || orc == STORE_ESENSOR || orc == STORE_EBUSY)
                 ? EXIT_REFUSED : EXIT_SETUP;
        goto out;
    }
    log_msg("open segment=%06" PRIu32 " records=%" PRIu64 " created=%d new_boot=%d "
            "truncated_bytes=%" PRIu64 " quarantined=%d last_seq=%" PRIu32,
            info.counter, info.records, info.created, info.new_boot, info.truncated_bytes,
            info.quarantined, info.last_seq);
    if (info.quarantined)
        log_msg("segment %06" PRIu32 " was QUARANTINED (bad data at offset %" PRIu64
                "); its bytes are kept as .corrupt-<n>, recording continues in %06" PRIu32,
                info.quarantined_counter, info.bad_offset, info.counter);

    heartbeat(&start, st, &c); /* first heartbeat right away */
    next_hb = start;
    next_hb.tv_sec += o.heartbeat_s;

    while (!g_stop) {
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
        struct timespec now;
        int n = poll(&p, 1, ms_until(&next_hb));

        if (n < 0 && errno != EINTR) {
            log_msg("poll stdin: %s", strerror(errno));
            rc = EXIT_INPUT;
            goto out;
        }
        if (n > 0) {
            ssize_t r = read(STDIN_FILENO, buf + have, sizeof buf - have);
            if (r < 0 && errno != EINTR) {
                log_msg("read stdin: %s", strerror(errno));
                rc = EXIT_INPUT;
                goto out;
            }
            if (r == 0)
                break; /* capture closed its end */
            if (r > 0)
                have += (size_t)r;
            if (have == sizeof buf) {
                struct record rec;
                memcpy(&rec, buf, sizeof rec);
                have = 0;
                rc = store_one(st, o.sensor, &rec, &c);
                if (rc != 0)
                    goto out;
            }
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && ts_ms(&now) >= ts_ms(&next_hb)) {
            heartbeat(&start, st, &c);
            next_hb.tv_sec += o.heartbeat_s;
        }
    }
    if (g_stop)
        stop_reason = "signal";
    if (have > 0)
        log_msg("dropping a partial record of %zu bytes at stop", have);

out:
    if (st != NULL && store_close(st) != STORE_OK && rc == 0)
        rc = EXIT_OUTPUT;
    log_msg("stop reason=%s exit=%d records=%lu last_seq=%" PRIu32,
            rc ? "error" : stop_reason, rc, c.records, c.last_seq);
    return rc;
}
