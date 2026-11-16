#ifndef KBUS_GW_HTTP_SERVER_H
#define KBUS_GW_HTTP_SERVER_H

#include <signal.h>

#include "config.h"

// Serve HTTP until *stop becomes non-zero. Returns 0 on clean shutdown.
int http_server_run(const gw_config_t *cfg, volatile sig_atomic_t *stop);

#endif
