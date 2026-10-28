/* log.h -- timestamped log lines on stderr, for the soak evidence.
 * Every line: ISO-8601 UTC time, program[pid], tag, message. */
#ifndef MOLDWATCH_LOG_H
#define MOLDWATCH_LOG_H

/* Set the program name and tag (e.g. "capture", "bme280") once at startup. */
void log_init(const char *prog, const char *tag);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Current resident memory of this process in KiB, or -1 if unreadable. */
long log_rss_kb(void);

#endif
