// Syslog backend of log.h (kept separate: <syslog.h> defines LOG_INFO too).
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

int g_log_syslog;

void log_init(void)
{
    const char *t = getenv("LOG_TARGET");
    if (t != NULL && strcmp(t, "syslog") == 0) {
        openlog("kbus-gateway", LOG_PID, LOG_DAEMON);
        g_log_syslog = 1;
    }
}

void log_syslog(int prio, const char *fmt, ...)
{
    // The WAGO KBUS library calls openlog("kbus") during initialisation, which
    // renames the whole process in syslog. Set our ident again for each of our
    // messages (openlog only stores the settings; the socket stays open).
    openlog("kbus-gateway", LOG_PID, LOG_DAEMON);
    va_list ap;
    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);
    va_end(ap);
}
