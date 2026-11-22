// Gateway core: channel map (DI/DO/AI/AO), process images and the KBUS cycle thread.
#ifndef KBUS_GW_GATEWAY_H
#define KBUS_GW_GATEWAY_H

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "config.h"

typedef enum { IO_DI = 0, IO_DO, IO_AI, IO_AO, IO_KIND_COUNT } io_kind_t;

// Limits of the reference application (arrays of 250 elements per class).
#define GW_MAX_MODULES   250
#define GW_MAX_CHANNELS  250

typedef struct {
    uint16_t module_index;    // index into the module list (0-based)
    uint16_t module_channel;  // channel on that module (1-based)
    uint32_t bit_offset;      // absolute bit offset in the input or output image
    uint8_t  bit_width;       // 1 (digital) or 16 (analog, unsigned WORD)
    uint8_t  mapped;          // 0: has a channel number but no process data is exchanged
    uint16_t value;           // outputs: value last written (the "output array")
} io_channel_t;

typedef struct {
    const char *backend;
    int         healthy;          // 0 after repeated cycle errors
    uint64_t    cycles;
    uint64_t    errors;
    uint64_t    overruns;
    uint32_t    last_cycle_us;    // duration of the last KBUS exchange
    uint32_t    max_cycle_us;     // longest KBUS exchange
    uint32_t    max_late_us;      // longest delay between the planned and the real start of a cycle
    int         cycle_ms;
    int         realtime;
} gw_stats_t;

enum { GW_OK = 0, GW_ENOENT = -1, GW_EREADONLY = -2, GW_ERANGE = -3,
       GW_EFAILED = -4, GW_EUNAVAILABLE = -5, GW_EOFFLINE = -6, GW_ETIMEOUT = -7 };

int  gateway_init(const gw_config_t *cfg, kbus_backend_t *backend);
int  gateway_start(void);
void gateway_stop(void);

const char *io_kind_name(io_kind_t kind);
int         io_kind_parse(const char *s, io_kind_t *kind);
int         io_kind_writable(io_kind_t kind);

size_t              gateway_channel_count(io_kind_t kind);
const io_channel_t *gateway_channel(io_kind_t kind, size_t number);   // number is 1-based
int                 gateway_read(io_kind_t kind, size_t number, int64_t *value);

// Bulk access (wdx parameters, filter[id], batch writes): one lock for all channels.
void gateway_snapshot(uint16_t out[IO_KIND_COUNT][GW_MAX_CHANNELS]);
// vals[k][i] = new value of channel i+1 of class k, or -1 = unchanged; n[k] values per class.
// *changed (may be NULL) = 1 when an output value changed.
int  gateway_write_multi(const int32_t *const vals[IO_KIND_COUNT], const size_t n[IO_KIND_COUNT],
                         int *changed);

// Dynamic configuration: rescan the bus when the node changed.
uint32_t gateway_config_version(void);   // +1 for every scan (startup = 1)
void     gateway_request_rescan(void);   // manual request (wdx KBus/Rescan)
int      gateway_rescan_due(void);       // 1 = call gateway_rescan() now
int      gateway_rescan(void);           // server main thread only, no request running; 0 = ok

const kbus_module_t *gateway_modules(size_t *count);
// Module index (0-based) of the module at KBUS position <pos>, -1 = none.
int                  gateway_module_index(uint16_t pos);
// 1 = at least one DI/DO/AI/AO channel belongs to the module.
int                  gateway_module_has_channels(size_t mi);

// Raw process data of complex modules (750-495, 750-657, 750-671, ...): modules WITHOUT
// channels whose data is byte aligned and at most GW_MODULE_RAW_MAX bytes per direction.
// The gateway has no driver of its own for them: only clients write these output bytes
// (the gateway sets them to 0 at start and rescan). A driver added later must refuse raw
// writes to its module (as the MQTT gateway does for the 750-671 stepper).
#define GW_MODULE_RAW_MAX 64
// 1 = raw module; *in_bytes / *out_bytes (may be NULL) = its data sizes.
int                  gateway_module_raw(size_t mi, size_t *in_bytes, size_t *out_bytes);
// Input bytes / last written output bytes of a raw module, under the lock (one bus cycle).
// Return the number of bytes, 0 = not a raw module or no data in that direction.
size_t               gateway_module_input(size_t mi, uint8_t *buf, size_t len);
size_t               gateway_module_output_read(size_t mi, uint8_t *buf, size_t len);
// Write the output bytes of one raw module (len = its output size), sent with the next
// cycle. GW_OK, GW_ENOENT (not a raw module), GW_EREADONLY (no output data), GW_ERANGE.
int                  gateway_module_output(size_t mi, const uint8_t *buf, size_t len);
// Write and read several raw modules in one go (wdx method KBus/Exchange):
//   - out_len > 0: the output bytes of all modules (concatenated, each exactly its output
//     size; out_len = the sum) are written under ONE lock, so they reach the modules in the
//     same bus cycle (*out_cycle = the number of that cycle). The call then waits for the
//     cycle AFTER it and returns the input bytes read in that cycle (*cycle), so a module
//     that answers within one cycle shows its answer.
//   - out_len == 0: read only: the inputs of the next cycle that completes (at most one
//     cycle of waiting; a stalled bus gives GW_ETIMEOUT, never stale data), *out_cycle = 0.
// Inputs are concatenated in the order of mi[]. GW_OK, GW_ENOENT, GW_ERANGE (sizes),
// GW_ETIMEOUT (no completed cycle within timeout_ms, e.g. bus error or rescan).
int                  gateway_exchange(const size_t *mi, size_t n, const uint8_t *out, size_t out_len,
                                      uint8_t *in, size_t in_cap, size_t *in_len,
                                      uint64_t *cycle, uint64_t *out_cycle, int timeout_ms);
// Number of the last completed bus cycle (never reset while the gateway runs).
uint64_t             gateway_cycle_number(void);

// Register read, READ ONLY (register communication of the firmware): count (1..16)
// registers from <reg> of table <table> of the module at position <pos>. Blocks for about
// 20 ms per register: call it from a worker thread (regcom.c), never from the cycle thread.
// Serialised with bus rescans (a rescan waits for a running read). GW_OK, GW_EFAILED
// (request failed), GW_EUNAVAILABLE (no register API), GW_EOFFLINE (bus not open).
int                  gateway_reg_read(uint16_t pos, uint16_t table, uint16_t reg, uint16_t count,
                                      uint16_t *buf, uint16_t *n);
void                 gateway_module_name(const kbus_module_t *m, char *buf, size_t len);
void                 gateway_stats(gw_stats_t *out);
void                 gateway_reset_stats(void);   // cycles, errors, maximum values and overruns back to 0

#define GW_CYCLE_MS_MIN 1
#define GW_CYCLE_MS_MAX 40
// New cycle time, effective from the next cycle (wdx KBus/CycleTime). Returns the
// previous value, or GW_ERANGE. A change resets the timing statistics.
int                  gateway_set_cycle_ms(int ms);

#endif
