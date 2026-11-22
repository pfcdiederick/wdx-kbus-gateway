// Register communication, READ ONLY, in a worker thread of its own (each read blocks for
// about 20 ms, so it may neither run in an HTTP server thread for long nor in the KBUS
// cycle thread). Taken from the MQTT gateway (src/regcom.c there); here an answer is
// handed to the waiting HTTP request instead of being published.
//
//   - status of every analog input channel: register 6 of the channel's register table
//     (table = channel on the module - 1), at most once per WDX_AI_STATUS_MS (>= 1000 ms)
//     per channel. A module whose first read fails (no register access) is left out
//     until the next bus scan. ONLY modules with analog input channels are read, never
//     modules without channels (750-495, 750-657, 750-671 and other complex modules):
//     a firmware register access there would disturb the handshake that CODESYS runs
//     through their raw process data.
//   - single register reads on request (wdx method KBus/ReadRegister), regcom_read().
//
// Writing registers is deliberately not implemented.
#ifndef KBUS_GW_REGCOM_H
#define KBUS_GW_REGCOM_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define REGCOM_STATUS_REGISTER 6
#define REGCOM_QUEUE           16     // requests waiting at the same time

enum { REGCOM_PENDING = 0, REGCOM_VALID, REGCOM_UNSUPPORTED };   // status of one AI channel

void regcom_start(const gw_config_t *cfg);   // after gateway_start()
void regcom_stop(void);                      // before gateway_stop()

// After every bus (re)scan, while no request runs (wda_init()): new AI channel list,
// all states back to pending.
void regcom_layout(void);

// Status per AI channel (index = channel number - 1), for n channels.
void regcom_status(uint8_t *state, uint16_t *value, size_t n);

// Read one register and wait for the answer (blocks: ~20 ms per queued read).
// Returns GW_OK (value set), GW_EFAILED, GW_EUNAVAILABLE, GW_EOFFLINE, or -100 = queue
// full, -101 = worker not running. *reason = short text for an error.
int  regcom_read(uint16_t pos, uint16_t table, uint16_t reg, uint16_t *value, const char **reason);

#endif
