// Runtime configuration, read from environment variables.
#ifndef KBUS_GW_CONFIG_H
#define KBUS_GW_CONFIG_H

typedef struct {
    // Unix socket for containers (the main interface)
    const char *wdx_socket;         // WDX_SOCKET         (default /run/kbus-gateway/wdx.sock, "" = off)
    int         wdx_socket_mode;    // WDX_SOCKET_MODE    (default 0660, octal)
    const char *wdx_socket_group;   // WDX_SOCKET_GROUP   (default "": keep root)
    // Control socket: only for the dashboard (kbus-manager). While KBus/LocalControl
    // is on, only requests through this socket may write the KBUS.
    const char *control_socket;     // CONTROL_SOCKET     (default /run/kbus-gateway-control/control.sock,
                                    //                     mode 0600 in a 0700 directory, "" = off)
    // TCP
    int         http_enable;        // HTTP_ENABLE        (default 1): TCP listener on/off
    const char *http_bind;          // HTTP_BIND          (default 127.0.0.1; 0.0.0.0 = all interfaces)
    int         http_port;          // HTTP_PORT          (default 8000)
    // Both listeners
    int         http_write_enable;  // HTTP_WRITE_ENABLE  (default 1): 0 = KBUS outputs read-only (403)
    int         http_log;           // HTTP_LOG           (default 1): 0 none, 1 output writes, 2 every request
    int         http_priority;      // HTTP_PRIORITY      (default 0): SCHED_FIFO priority of the server, 0 = normal
    int         max_clients;        // WDX_MAX_CLIENTS    (default 16): connections served at the same time
    int         client_timeout_ms;  // WDX_CLIENT_TIMEOUT_MS (default 5000): time for a client to send its request
    // Registers
    const char *register_file;      // REGISTER_FILE      (default /etc/kbus-gateway-registers.conf)
    const char *register_store;     // REGISTER_STORE     (default /home/kbus-gateway/registers.dat, "" = off)
    int         register_save_s;    // REGISTER_SAVE_S    (default 10): at most one save per this many seconds
    // KBUS
    int         cycle_ms;        // KBUS_CYCLE_MS    (default 5, must stay well below 50 ms);
                                 //   a value written to KBus/CycleTime is stored in settings_store
                                 //   and wins over KBUS_CYCLE_MS
    const char *settings_store;  // KBUS_SETTINGS_STORE (default /home/kbus-gateway/kbus-settings.conf, "" = off)
    int         local_control;   // stored KBus/LocalControl (settings_store), default 0
    int         rt_priority;     // KBUS_PRIORITY    (default 40, SCHED_FIFO; 0 = no real-time)
    int         simulation;      // KBUS_SIMULATION  (default 0): simulated I/O, no hardware
    const char *dal_libs;        // KBUS_DAL_LIB     colon separated dlopen() candidates
    const char *info_libs;       // KBUS_INFO_LIB    colon separated dlopen() candidates
    const char *digital_offset;  // KBUS_DIGITAL_OFFSET_MODE auto|relative|absolute
} gw_config_t;

void config_load(gw_config_t *cfg);
void config_log(const gw_config_t *cfg);
// Stored settings (settings_store): CYCLE_MS=<n> (only once written via KBus/CycleTime)
// and LOCAL_CONTROL=0|1. config_load_stored() applies them to cfg (call after
// config_load); config_store_cycle() / config_store_local_control() change one
// and write the file. 0 = ok.
int  config_load_stored(gw_config_t *cfg);
int  config_store_cycle(const gw_config_t *cfg, int cycle_ms);
int  config_store_local_control(const gw_config_t *cfg, int on);

#endif
