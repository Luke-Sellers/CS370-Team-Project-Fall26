#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static const char *g_prog = "?";
static const char *g_tag = "?";

void log_init(const char *prog, const char *tag)
{
    g_prog = prog;
    g_tag = tag;
}

void log_msg(const char *fmt, ...)
{
    struct timespec ts = { 0, 0 };
    struct tm tm;
    char when[32] = "0000-00-00T00:00:00";

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && gmtime_r(&ts.tv_sec, &tm) != NULL)
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);

    fprintf(stderr, "%s.%03ldZ %s[%ld] %s: ", when, ts.tv_nsec / 1000000L, g_prog,
            (long)getpid(), g_tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

long log_rss_kb(void)
{
    /* /proc/self/statm: total resident ... (in pages) */
    FILE *f = fopen("/proc/self/statm", "re");
    long total, resident;
    long page = sysconf(_SC_PAGESIZE);
    long rc = -1;

    if (f == NULL || page <= 0)
        goto out;
    if (fscanf(f, "%ld %ld", &total, &resident) == 2)
        rc = resident * (page / 1024);
out:
    if (f != NULL)
        fclose(f);
    return rc;
}
