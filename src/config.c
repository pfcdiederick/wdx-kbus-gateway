#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *env_str(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? v : def;
}

static int env_int(const char *name, int def, int min, int max)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n < min || n > max) {
        LOG_WARN("%s='%s' is invalid (allowed %d..%d), using %d", name, v, min, max, def);
        return def;
    }
    return (int)n;
}

void config_load(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->http_port      = env_int("HTTP_PORT", 8000, 1, 65535);
    cfg->http_enable    = env_int("HTTP_ENABLE", 1, 0, 1);
    cfg->http_write_enable = env_int("HTTP_WRITE_ENABLE", 1, 0, 1);
    cfg->http_log       = env_int("HTTP_LOG", 1, 0, 2);
    cfg->cycle_ms       = env_int("KBUS_CYCLE_MS", 10, 1, 40);
    cfg->rt_priority    = env_int("KBUS_PRIORITY", 40, 0, 99);
    cfg->simulation     = env_int("KBUS_SIMULATION", 0, 0, 1);
    cfg->dal_libs       = env_str("KBUS_DAL_LIB",
                                  "libdal.so:libdal.so.0:libdal.so.1");
    cfg->info_libs      = env_str("KBUS_INFO_LIB",
                                  "libdbuskbuscommon.so:libdbuskbuscommon.so.0:libdbuskbuscommon.so.1");
    cfg->digital_offset = env_str("KBUS_DIGITAL_OFFSET_MODE", "auto");
}

void config_log(const gw_config_t *cfg)
{
    static const char *const log_names[] = { "none", "writes", "all" };
    LOG_INFO("config: rest=%s port=%d writes=%s log=%s cycle=%dms rt-prio=%d mode=%s",
             cfg->http_enable ? "on" : "off", cfg->http_port,
             cfg->http_write_enable ? "allowed" : "disabled", log_names[cfg->http_log],
             cfg->cycle_ms, cfg->rt_priority,
             cfg->simulation ? "SIMULATION" : "kbus");
}
