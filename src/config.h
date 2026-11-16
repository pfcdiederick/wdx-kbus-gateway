// Runtime configuration, read from environment variables.
#ifndef KBUS_GW_CONFIG_H
#define KBUS_GW_CONFIG_H

typedef struct {
    int         http_port;          // HTTP_PORT          (default 8000)
    int         http_enable;        // HTTP_ENABLE        (default 1): REST server on/off
    int         http_write_enable;  // HTTP_WRITE_ENABLE  (default 1): 0 = POST answers 403
    int         http_log;           // HTTP_LOG           (default 1): 0 none, 1 writes, 2 every request
    int         cycle_ms;        // KBUS_CYCLE_MS    (default 10, must stay well below 50 ms)
    int         rt_priority;     // KBUS_PRIORITY    (default 40, SCHED_FIFO; 0 = no real-time)
    int         simulation;      // KBUS_SIMULATION  (default 0): simulated I/O, no hardware
    const char *dal_libs;        // KBUS_DAL_LIB     colon separated dlopen() candidates
    const char *info_libs;       // KBUS_INFO_LIB    colon separated dlopen() candidates
    const char *digital_offset;  // KBUS_DIGITAL_OFFSET_MODE auto|relative|absolute
} gw_config_t;

void config_load(gw_config_t *cfg);
void config_log(const gw_config_t *cfg);

#endif
