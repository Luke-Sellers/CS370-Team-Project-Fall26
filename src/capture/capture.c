/* capture -- read one sensor on a fixed schedule and emit records.
 *
 * One process per sensor, so a supervisor can restart either one alone:
 *   capture --sensor bme280
 *   capture --sensor mlx90614
 *
 * stdout: one record per sample, as readable lines by default, or raw
 *         32-byte records with --binary for piping into storage.
 * stderr: timestamped log -- startup, heartbeats, sensor lost/recovered,
 *         shutdown. This is the soak evidence (handout Section 3.4).
 *
 * Exit codes: 0 stopped cleanly (count reached, SIGINT or SIGTERM),
 * 1 bad arguments, 2 sensor missing at startup, 3 output failed,
 * 4 process setup failed.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bme280.h"
#include "fake.h"
#include "i2c.h"
#include "log.h"
#include "mlx90614.h"
#include "record.h"

enum { EXIT_USAGE = 1, EXIT_SENSOR = 2, EXIT_OUTPUT = 3, EXIT_SETUP = 4 };

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

struct opts {
    enum sensor_id sensor;
    int fake;
    int binary;
    long interval_ms;
    long heartbeat_s;
    long count;      /* 0 = run until stopped */
    long drop_start; /* --fake-drop: first failing sample, 0 = none */
    long drop_len;
    const char *bus;
};

struct sensor_state {
    int fd;
    struct bme280_dev bme;
    struct mlx90614_dev mlx;
};

struct counters {
    unsigned long records;
    unsigned long read_errors;
    unsigned long losses;     /* times the sensor went from ok to failing */
    unsigned long recoveries; /* times it came back */
};

static void usage(FILE *f)
{
    fprintf(f,
            "usage: capture --sensor bme280|mlx90614 [--binary] [--bus /dev/i2c-1]\n"
            "               [--interval-ms N] [--heartbeat-s N] [--count N]\n"
            "               [--fake [--fake-drop START:LEN]]\n"
            "  --fake        synthetic readings, labeled SYNTHETIC everywhere\n"
            "  --fake-drop   simulate the sensor failing for LEN samples from START\n");
}

static int parse_long(const char *s, long min, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min)
        return -1;
    *out = v;
    return 0;
}

static int parse_drop(const char *s, long *start, long *len)
{
    char buf[64];
    char *colon;

    if (strlen(s) >= sizeof buf)
        return -1;
    strcpy(buf, s);
    colon = strchr(buf, ':');
    if (colon == NULL)
        return -1;
    *colon = '\0';
    return (parse_long(buf, 1, start) == 0 && parse_long(colon + 1, 1, len) == 0) ? 0 : -1;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    *o = (struct opts){ .interval_ms = 10000, .heartbeat_s = 60, .bus = "/dev/i2c-1" };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--fake") == 0) {
            o->fake = 1;
        } else if (strcmp(arg, "--binary") == 0) {
            o->binary = 1;
        } else if (strcmp(arg, "--sensor") == 0 && val) {
            if (strcmp(val, "bme280") == 0)
                o->sensor = SENSOR_BME280;
            else if (strcmp(val, "mlx90614") == 0)
                o->sensor = SENSOR_MLX90614;
            else
                return -1;
            i++;
        } else if (strcmp(arg, "--interval-ms") == 0 && val) {
            if (parse_long(val, 1, &o->interval_ms) < 0)
                return -1;
            i++;
        } else if (strcmp(arg, "--heartbeat-s") == 0 && val) {
            /* At most 60 s: CLAUDE.md wants a heartbeat within 60 s of start,
             * and the soak needs one at least hourly. */
            if (parse_long(val, 1, &o->heartbeat_s) < 0 || o->heartbeat_s > 3600)
                return -1;
            i++;
        } else if (strcmp(arg, "--count") == 0 && val) {
            if (parse_long(val, 0, &o->count) < 0)
                return -1;
            i++;
        } else if (strcmp(arg, "--fake-drop") == 0 && val) {
            if (parse_drop(val, &o->drop_start, &o->drop_len) < 0)
                return -1;
            i++;
        } else if (strcmp(arg, "--bus") == 0 && val) {
            o->bus = val;
            i++;
        } else {
            return -1;
        }
    }
    if (o->drop_start && !o->fake)
        return -1; /* fault simulation is synthetic only */
    return o->sensor ? 0 : -1;
}

static const char *sensor_name(enum sensor_id s)
{
    return s == SENSOR_BME280 ? "bme280" : "mlx90614";
}

static int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static void timespec_add_ms(struct timespec *t, long ms)
{
    t->tv_sec += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec += 1;
        t->tv_nsec -= 1000000000L;
    }
}

/* Print a 0.01-scaled value such as 2150 as "21.50". */
static void print_centi(FILE *f, int32_t v)
{
    long m = v < 0 ? -(long)v : (long)v;
    fprintf(f, "%s%ld.%02ld", v < 0 ? "-" : "", m / 100, m % 100);
}

static int emit(const struct opts *o, const struct record *r)
{
    if (o->binary) {
        if (fwrite(r, sizeof *r, 1, stdout) != 1)
            return -1;
    } else {
        int bme = (r->sensor == SENSOR_BME280);
        printf("#%u %s t=%lld.%03lld ", r->seq, bme ? "bme280" : "mlx90614",
               (long long)(r->t_ns / 1000000000LL), (long long)((r->t_ns / 1000000LL) % 1000LL));
        if (r->flags & REC_READ_ERR) {
            printf("READ_ERROR");
        } else {
            printf(bme ? "air=" : "ceiling=");
            print_centi(stdout, r->a);
            printf(bme ? "C rh=" : "C sensor=");
            print_centi(stdout, r->b);
            printf(bme ? "%%" : "C");
        }
        printf("%s\n", (r->flags & REC_SYNTHETIC) ? " SYNTHETIC" : "");
    }
    return fflush(stdout) == 0 ? 0 : -1;
}

static int sensor_init(const struct opts *o, struct sensor_state *s)
{
    return o->sensor == SENSOR_BME280 ? bme280_init(s->fd, &s->bme, BME280_ADDR)
                                      : mlx90614_init(s->fd, &s->mlx, MLX90614_ADDR);
}

/* Fill r->a and r->b. Returns 0, or -1 with errno set. */
static int take_sample(const struct opts *o, struct sensor_state *s, struct record *r)
{
    if (o->fake) {
        if (o->drop_start && (long)r->seq >= o->drop_start &&
            (long)r->seq < o->drop_start + o->drop_len) {
            errno = EIO; /* simulated unplug */
            return -1;
        }
        fake_fill(r);
        return 0;
    }
    return o->sensor == SENSOR_BME280 ? bme280_read(s->fd, &s->bme, &r->a, &r->b)
                                      : mlx90614_read(s->fd, &s->mlx, &r->a, &r->b);
}

static void heartbeat(const struct timespec *start, const struct counters *c, int sensor_ok)
{
    struct timespec now;
    long uptime = -1;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        uptime = (long)(now.tv_sec - start->tv_sec);
    log_msg("heartbeat uptime_s=%ld rss_kb=%ld records=%lu read_errors=%lu losses=%lu "
            "recoveries=%lu sensor=%s",
            uptime, log_rss_kb(), c->records, c->read_errors, c->losses, c->recoveries,
            sensor_ok ? "ok" : "DEGRADED");
}

int main(int argc, char **argv)
{
    struct opts o;
    struct sensor_state s = { .fd = -1 };
    struct counters c = { 0 };
    struct timespec start, next, next_hb;
    int sensor_ok = 1;
    int rc = 0;
    const char *stop_reason = "count reached";

    if (parse_args(argc, argv, &o) < 0) {
        usage(stderr);
        return EXIT_USAGE;
    }
    log_init("capture", sensor_name(o.sensor));

    struct sigaction sa = { .sa_handler = on_signal }; /* no SA_RESTART: wake the sleep */
    struct sigaction ign = { .sa_handler = SIG_IGN };
    sigemptyset(&sa.sa_mask);
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGPIPE, &ign, NULL) < 0) { /* storage gone: get EPIPE, exit with a log line */
        log_msg("setup failed: sigaction: %s", strerror(errno));
        return EXIT_SETUP;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        log_msg("setup failed: clock_gettime: %s", strerror(errno));
        return EXIT_SETUP;
    }

    log_msg("start mode=%s interval_ms=%ld heartbeat_s=%ld output=%s%s",
            o.fake ? "SYNTHETIC" : "live", o.interval_ms, o.heartbeat_s,
            o.binary ? "binary" : "text", o.fake ? " (all records flagged REC_SYNTHETIC)" : "");

    if (!o.fake) {
        s.fd = i2c_open(o.bus);
        if (s.fd < 0) {
            log_msg("cannot open %s: %s", o.bus, strerror(errno));
            rc = EXIT_SENSOR;
            goto out;
        }
        if (sensor_init(&o, &s) < 0) {
            log_msg("sensor not found at startup: %s", strerror(errno));
            rc = EXIT_SENSOR;
            goto out;
        }
        log_msg("sensor found on %s", o.bus);
    }

    heartbeat(&start, &c, sensor_ok); /* first heartbeat right away */
    next = start;
    next_hb = start;
    next_hb.tv_sec += o.heartbeat_s;

    for (uint32_t seq = 1; !g_stop && (o.count == 0 || seq <= (uint32_t)o.count); seq++) {
        struct timespec wall;
        if (clock_gettime(CLOCK_REALTIME, &wall) < 0) {
            log_msg("clock_gettime(REALTIME): %s", strerror(errno));
            wall = (struct timespec){ 0, 0 };
        }
        struct record r = {
            .magic = RECORD_MAGIC,
            .sensor = (uint8_t)o.sensor,
            .flags = o.fake ? REC_SYNTHETIC : 0,
            .seq = seq,
            .t_ns = ts_ns(&wall),
        };

        /* After a failure, try to bring the sensor back before reading:
         * an unplugged BME280 loses its configuration. */
        int ok = 0;
        if (!sensor_ok && !o.fake && sensor_init(&o, &s) < 0)
            ok = -1;
        if (ok == 0)
            ok = take_sample(&o, &s, &r);

        if (ok < 0) {
            r.flags |= REC_READ_ERR;
            r.a = r.b = 0;
            c.read_errors++;
            if (sensor_ok) {
                c.losses++;
                log_msg("sensor LOST at #%u: %s; continuing, recording READ_ERR", seq,
                        strerror(errno));
            }
            sensor_ok = 0;
        } else if (!sensor_ok) {
            c.recoveries++;
            log_msg("sensor RECOVERED at #%u after %lu failed reads total", seq, c.read_errors);
            sensor_ok = 1;
        }

        if (emit(&o, &r) < 0) {
            log_msg("output failed: %s", strerror(errno));
            rc = EXIT_OUTPUT;
            goto out;
        }
        c.records++;

        if (o.count != 0 && seq == (uint32_t)o.count)
            break;

        /* Sleep to an absolute deadline so the schedule never drifts. */
        timespec_add_ms(&next, o.interval_ms);
        int e;
        do {
            e = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        } while (e == EINTR && !g_stop);
        if (e != 0 && e != EINTR)
            log_msg("clock_nanosleep: %s", strerror(e));

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && ts_ns(&now) >= ts_ns(&next_hb)) {
            heartbeat(&start, &c, sensor_ok);
            next_hb.tv_sec += o.heartbeat_s;
        }
    }
    if (g_stop)
        stop_reason = "signal";

out:
    if (s.fd >= 0 && close(s.fd) < 0)
        log_msg("close: %s", strerror(errno));
    log_msg("stop reason=%s exit=%d records=%lu read_errors=%lu losses=%lu recoveries=%lu",
            rc ? "error" : stop_reason, rc, c.records, c.read_errors, c.losses, c.recoveries);
    return rc;
}
