// Minimal logging: stderr (terminal, simulation) or
// syslog (native installation on the PFC: LOG_TARGET=syslog, see log_init()).
// <syslog.h> is deliberately NOT included here: it defines its own LOG_INFO.
#ifndef KBUS_GW_LOG_H
#define KBUS_GW_LOG_H

#include <stdio.h>

extern int g_log_syslog;   // 1 = syslog, 0 = stderr (default)

// Call once at start: LOG_TARGET=syslog switches to syslog (ident kbus-gateway).
void log_init(void);
// prio: 3 = error, 4 = warning, 6 = info (syslog priorities)
void log_syslog(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_AT(prio, tag, fmt, ...)                                              \
    do {                                                                         \
        if (g_log_syslog)                                                        \
            log_syslog(prio, fmt, ##__VA_ARGS__);                                \
        else                                                                     \
            fprintf(stderr, tag fmt "\n", ##__VA_ARGS__);                        \
    } while (0)

#define LOG_INFO(fmt, ...)  LOG_AT(6, "[info]  ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_AT(4, "[warn]  ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_AT(3, "[error] ", fmt, ##__VA_ARGS__)

#endif
