// WAGO KBUS HTTP gateway
//
// Owns the KBUS of a WAGO PFC100/PFC200/PFC300 (ADI/DAL interface) and serves
// its DI / DO / AI / AO channels, plus named registers that containers share,
// as a wdx data layer (/wda/parameters, JSON:API, WAGO wdx conventions) on a
// Unix socket for containers and optionally on TCP.
// Runs natively on the PTXdist firmware (opkg package, tools/build-ipk.sh).
#include "backend.h"
#include "config.h"
#include "gateway.h"
#include "http_server.h"
#include "log.h"
#include "registers.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    log_init();
    LOG_INFO("WAGO KBUS HTTP gateway starting");

    gw_config_t cfg;
    config_load(&cfg);
    config_load_stored(&cfg);           // cycle time written via KBus/CycleTime
    config_log(&cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Keep the real-time cycle free of page faults.
    if (cfg.rt_priority > 0 && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        LOG_WARN("mlockall failed; memory is not locked (needs IPC_LOCK / --privileged)");

    kbus_backend_t *backend = cfg.simulation ? backend_sim_create(&cfg) : backend_adi_create(&cfg);
    if (gateway_init(&cfg, backend) != 0) {
        LOG_ERROR("KBUS initialisation failed, exiting");
        return 1;
    }
    if (gateway_start() != 0) {
        gateway_stop();
        return 1;
    }

    registers_load(&cfg);
    const int rc = http_server_run(&cfg, &g_stop);

    LOG_INFO("shutting down");
    registers_save(1);
    gateway_stop();
    return rc == 0 ? 0 : 1;
}
