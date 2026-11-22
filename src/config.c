#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *env_str(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? v : def;
}

// Like env_str, but a variable that is set and empty stays empty ("" = off).
static const char *env_str_empty(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v != NULL ? v : def;
}

static int env_octal(const char *name, int def)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    char *end = NULL;
    const long n = strtol(v, &end, 8);
    if (end == v || *end != '\0' || n < 0 || n > 0777) {
        LOG_WARN("%s='%s' is invalid (octal 0..0777), using %04o", name, v, def);
        return def;
    }
    return (int)n;
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
    cfg->wdx_socket     = env_str_empty("WDX_SOCKET", "/run/kbus-gateway/wdx.sock");
    cfg->wdx_socket_mode = env_octal("WDX_SOCKET_MODE", 0660);
    cfg->wdx_socket_group = env_str("WDX_SOCKET_GROUP", "");
    cfg->control_socket = env_str_empty("CONTROL_SOCKET", "/run/kbus-gateway-control/control.sock");
    cfg->http_enable    = env_int("HTTP_ENABLE", 1, 0, 1);
    cfg->http_bind      = env_str("HTTP_BIND", "127.0.0.1");
    cfg->http_port      = env_int("HTTP_PORT", 8000, 1, 65535);
    cfg->http_write_enable = env_int("HTTP_WRITE_ENABLE", 1, 0, 1);
    cfg->http_log       = env_int("HTTP_LOG", 1, 0, 2);
    cfg->http_priority  = env_int("HTTP_PRIORITY", 0, 0, 99);
    cfg->max_clients    = env_int("WDX_MAX_CLIENTS", 16, 1, 64);
    cfg->client_timeout_ms = env_int("WDX_CLIENT_TIMEOUT_MS", 5000, 100, 60000);
    cfg->event_ms       = env_int("WDX_EVENT_MS", 10, 2, 1000);
    cfg->max_streams    = env_int("WDX_MAX_STREAMS", 4, 0, 16);
    cfg->ai_status_ms   = env_int("WDX_AI_STATUS_MS", 1000, 0, 3600000);
    if (cfg->ai_status_ms > 0 && cfg->ai_status_ms < 1000) {
        LOG_WARN("WDX_AI_STATUS_MS=%d is below the minimum, using 1000", cfg->ai_status_ms);
        cfg->ai_status_ms = 1000;
    }
    cfg->register_file  = env_str("REGISTER_FILE", "/etc/kbus-gateway-registers.conf");
    cfg->register_store = env_str_empty("REGISTER_STORE", "/home/kbus-gateway/registers.dat");
    cfg->register_save_s = env_int("REGISTER_SAVE_S", 10, 1, 3600);
    cfg->cycle_ms       = env_int("KBUS_CYCLE_MS", 5, 1, 40);
    cfg->settings_store = env_str_empty("KBUS_SETTINGS_STORE", "/home/kbus-gateway/kbus-settings.conf");
    cfg->local_control  = 0;
    cfg->serial_baud    = env_int("SERIAL_BAUD", 19200, 1200, 230400);
    snprintf(cfg->serial_format, sizeof(cfg->serial_format), "%.3s", env_str("SERIAL_FORMAT", "8E1"));
    cfg->serial_frame_gap_ms = env_int("SERIAL_FRAME_GAP_MS", 10, 1, 1000);
    cfg->rt_priority    = env_int("KBUS_PRIORITY", 40, 0, 99);
    cfg->simulation     = env_int("KBUS_SIMULATION", 0, 0, 1);
    cfg->serial_device  = env_str_empty("SERIAL_DEVICE", cfg->simulation ? "" : "/dev/serial");
    cfg->dal_libs       = env_str("KBUS_DAL_LIB",
                                  "libdal.so:libdal.so.0:libdal.so.1");
    cfg->info_libs      = env_str("KBUS_INFO_LIB",
                                  "libdbuskbuscommon.so:libdbuskbuscommon.so.0:libdbuskbuscommon.so.1");
    cfg->digital_offset = env_str("KBUS_DIGITAL_OFFSET_MODE", "auto");
}

// What the settings file holds (0 = no CYCLE_MS line: KBUS_CYCLE_MS applies).
static int stored_cycle_ms;
static int stored_local_control;
static int stored_serial_baud;              // 0 = not stored
static char stored_serial_format[4];
static int stored_serial_gap;

int config_load_stored(gw_config_t *cfg)
{
    if (!*cfg->settings_store)
        return 0;
    FILE *f = fopen(cfg->settings_store, "r");
    if (f == NULL)
        return 0;                                         // nothing stored yet
    char line[128];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "CYCLE_MS=%d", &v) == 1) {
            if (v >= 1 && v <= 40) {
                if (v != cfg->cycle_ms)
                    LOG_INFO("cycle time %d ms from %s (KBUS_CYCLE_MS: %d ms)", v, cfg->settings_store,
                             cfg->cycle_ms);
                cfg->cycle_ms = v;
                stored_cycle_ms = v;
            } else {
                LOG_WARN("%s: CYCLE_MS=%d out of range 1..40, ignored", cfg->settings_store, v);
                rc = -1;
            }
        } else if (sscanf(line, "SERIAL_BAUD=%d", &v) == 1 && v >= 1200 && v <= 230400) {
            cfg->serial_baud = stored_serial_baud = v;
        } else if (sscanf(line, "SERIAL_FRAME_GAP_MS=%d", &v) == 1 && v >= 1 && v <= 1000) {
            cfg->serial_frame_gap_ms = stored_serial_gap = v;
        } else if (strncmp(line, "SERIAL_FORMAT=", 14) == 0 && strlen(line) >= 17) {
            memcpy(stored_serial_format, line + 14, 3);
            stored_serial_format[3] = '\0';
            memcpy(cfg->serial_format, stored_serial_format, 4);
        } else if (sscanf(line, "LOCAL_CONTROL=%d", &v) == 1) {
            cfg->local_control = stored_local_control = (v != 0);
            if (v)
                LOG_WARN("local control is ON (from %s): only the dashboard may write the KBUS",
                         cfg->settings_store);
        }
    }
    fclose(f);
    return rc;
}

// Write the settings file: temporary file, fsync, rename (never a half file).
static int store_settings(const gw_config_t *cfg)
{
    if (!*cfg->settings_store)
        return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->settings_store);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        LOG_WARN("cannot store the settings in %s: %s", cfg->settings_store, strerror(errno));
        return -1;
    }
    fprintf(f, "# written by kbus-gateway (KBus/CycleTime, KBus/LocalControl);\n"
               "# without CYCLE_MS, KBUS_CYCLE_MS applies\n");
    if (stored_cycle_ms)
        fprintf(f, "CYCLE_MS=%d\n", stored_cycle_ms);
    fprintf(f, "LOCAL_CONTROL=%d\n", stored_local_control);
    if (stored_serial_baud)
        fprintf(f, "SERIAL_BAUD=%d\nSERIAL_FORMAT=%s\nSERIAL_FRAME_GAP_MS=%d\n", stored_serial_baud,
                stored_serial_format, stored_serial_gap);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0 || rename(tmp, cfg->settings_store) != 0) {
        LOG_WARN("cannot store the settings in %s: %s", cfg->settings_store, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_store_cycle(const gw_config_t *cfg, int cycle_ms)
{
    stored_cycle_ms = cycle_ms;
    return store_settings(cfg);
}

int config_store_serial(const gw_config_t *cfg, int baud, const char *format, int frame_gap_ms)
{
    stored_serial_baud = baud;
    snprintf(stored_serial_format, sizeof(stored_serial_format), "%.3s", format);
    stored_serial_gap = frame_gap_ms;
    return store_settings(cfg);
}

int config_store_local_control(const gw_config_t *cfg, int on)
{
    stored_local_control = on != 0;
    return store_settings(cfg);
}

void config_log(const gw_config_t *cfg)
{
    static const char *const log_names[] = { "none", "writes", "all" };
    LOG_INFO("config: socket=%s (mode %04o%s%s) control=%s tcp=%s %s:%d output-writes=%s log=%s clients=%d "
             "server-prio=%d cycle=%dms rt-prio=%d mode=%s",
             *cfg->wdx_socket ? cfg->wdx_socket : "off", (unsigned)cfg->wdx_socket_mode,
             *cfg->wdx_socket_group ? " group " : "", cfg->wdx_socket_group,
             *cfg->control_socket ? cfg->control_socket : "off",
             cfg->http_enable ? "on" : "off", cfg->http_bind, cfg->http_port,
             cfg->http_write_enable ? "allowed" : "disabled", log_names[cfg->http_log], cfg->max_clients,
             cfg->http_priority, cfg->cycle_ms, cfg->rt_priority,
             cfg->simulation ? "SIMULATION" : "kbus");
    if (*cfg->serial_device)
        LOG_INFO("config: COM1 %s %d %s, frame gap %d ms", cfg->serial_device, cfg->serial_baud,
                 cfg->serial_format, cfg->serial_frame_gap_ms);
    else
        LOG_INFO("config: COM1 off (SERIAL_DEVICE empty%s)", cfg->simulation ? ", simulation" : "");
    if (cfg->http_priority > 0 && cfg->rt_priority > 0 && cfg->http_priority >= cfg->rt_priority)
        LOG_WARN("HTTP_PRIORITY %d is not below KBUS_PRIORITY %d: requests may delay the KBUS cycle",
                 cfg->http_priority, cfg->rt_priority);
}
