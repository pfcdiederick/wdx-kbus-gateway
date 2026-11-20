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

enum { GW_OK = 0, GW_ENOENT = -1, GW_EREADONLY = -2, GW_ERANGE = -3 };

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
void                 gateway_module_name(const kbus_module_t *m, char *buf, size_t len);
void                 gateway_stats(gw_stats_t *out);
void                 gateway_reset_stats(void);   // cycles, errors, maximum values and overruns back to 0

#define GW_CYCLE_MS_MIN 1
#define GW_CYCLE_MS_MAX 40
// New cycle time, effective from the next cycle (wdx KBus/CycleTime). Returns the
// previous value, or GW_ERANGE. A change resets the timing statistics.
int                  gateway_set_cycle_ms(int ms);

#endif
