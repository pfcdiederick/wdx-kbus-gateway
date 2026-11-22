#include "gateway.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define ERRORS_UNTIL_UNHEALTHY 10
#define RESCAN_AFTER_MS        500     // 0.5 s of failing cycles: the node has changed
#define RESCAN_BACKOFF_MIN_S   5       // automatic rescans: 5, 10, 20, 40, 60, 60 ... s apart
#define RESCAN_BACKOFF_MAX_S   60

static struct {
    kbus_backend_t *be;
    kbus_module_t   modules[KBUS_MAX_MODULES];
    size_t          module_count;

    io_channel_t    ch[IO_KIND_COUNT][GW_MAX_CHANNELS];
    size_t          ch_count[IO_KIND_COUNT];
    uint8_t         mod_has_ch[KBUS_MAX_MODULES];    // 1 = the module has DI/DO/AI/AO channels
    size_t          count_495;

    uint8_t         in[KBUS_IMAGE_BYTES];
    uint8_t         out[KBUS_IMAGE_BYTES];
    size_t          in_len, out_len;

    pthread_mutex_t lock;
    pthread_cond_t  cycle_cond;         // broadcast after every completed cycle (gateway_exchange)
    uint64_t        seq_started;        // under lock: cycles that took the output image
    uint64_t        seq_done;           // under lock: last cycle that completed (inputs stored)
    pthread_mutex_t reg_lock;
    int             bus_open;           // under reg_lock: the backend is open (register reads allowed)
    pthread_t       thread;
    volatile int    running;
    int             thread_started;
    int             rt_priority;
    uint32_t        consecutive_errors;
    gw_stats_t      stats;

    // Dynamic configuration: a changed node is found by rescanning the bus.
    uint32_t        config_version;     // +1 for every scan (wdx: KBus/ConfigurationVersion)
    volatile int    rescan_requested;   // 0 no, 1 manual (now), 2 automatic (after the backoff)
    int             rescan_backoff_s;
    struct timespec next_auto_rescan;
} gw;

static const char *const kind_names[IO_KIND_COUNT] = { "DI", "DO", "AI", "AO" };

const char *io_kind_name(io_kind_t kind) { return kind_names[kind]; }
int io_kind_writable(io_kind_t kind)     { return kind == IO_DO || kind == IO_AO; }

int io_kind_parse(const char *s, io_kind_t *kind)
{
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        if (strcasecmp(s, kind_names[k]) == 0) {
            *kind = (io_kind_t)k;
            return 0;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Bit access. KBUS images are little endian and LSB-first within a byte, so a
// generic bit loop handles both digital bits and byte-aligned analog words.
// ---------------------------------------------------------------------------
static uint32_t bits_get(const uint8_t *img, uint32_t off, uint8_t width)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < width; ++i) {
        const uint32_t b = off + i;
        if ((img[b >> 3] >> (b & 7u)) & 1u)
            v |= 1u << i;
    }
    return v;
}

static void bits_set(uint8_t *img, uint32_t off, uint8_t width, uint32_t v)
{
    for (uint8_t i = 0; i < width; ++i) {
        const uint32_t b = off + i;
        const uint8_t mask = (uint8_t)(1u << (b & 7u));
        if ((v >> i) & 1u)
            img[b >> 3] |= mask;
        else
            img[b >> 3] &= (uint8_t)~mask;
    }
}

// ---------------------------------------------------------------------------
// Channel map (as FbDynamicConfiguration of the reference application)
//
// - Each class (DI/DO/AI/AO) has its own numbering from 1, in slot order; a
//   module with N channels takes the next N numbers of its class.
// - At most 250 modules and 250 channels per class; later ones get no number.
// - Digital: channel k = bit k of the module data; only k = 0..15 is
//   exchanged, higher channels still take a number.
// - Analog: channel k = unsigned 16-bit WORD at byte offset 2*k of the module
//   data (at most 48 bytes), raw value.
// - 750-495 and other complex modules (750-657, 750-671, ...): no channels. Their
//   process data is exchanged RAW, byte by byte, by clients (CODESYS) through the wdx
//   method KBus/Exchange and KBus/Modules/<m>/RawInput|RawOutput (gateway_exchange());
//   the gateway itself never writes their outputs (only 0 at start and rescan).
// ---------------------------------------------------------------------------
#define MODULE_DATA_BYTES 48
#define DIGITAL_MAPPED_CHANNELS 16

static void add_channel(io_kind_t kind, size_t module_index, unsigned module_channel,
                        uint32_t bit_offset, uint8_t width, int mapped, size_t image_len)
{
    if (gw.ch_count[kind] >= GW_MAX_CHANNELS)
        return;   // channels beyond 250 are not assigned
    if (module_index < KBUS_MAX_MODULES)
        gw.mod_has_ch[module_index] = 1;
    if (mapped && (uint64_t)bit_offset + width > (uint64_t)image_len * 8u) {
        LOG_WARN("module %zu: %s channel %u lies outside the process image, not exchanged",
                 module_index + 1, kind_names[kind], module_channel);
        mapped = 0;
    }
    io_channel_t *c = &gw.ch[kind][gw.ch_count[kind]++];
    memset(c, 0, sizeof(*c));
    c->module_index   = (uint16_t)module_index;
    c->module_channel = (uint16_t)module_channel;
    c->bit_offset     = bit_offset;
    c->bit_width      = width;
    c->mapped         = (uint8_t)mapped;
}

static void add_digital(io_kind_t kind, size_t mi, uint32_t off, uint32_t channels, size_t image_len)
{
    for (uint32_t k = 0; k < channels; ++k)
        add_channel(kind, mi, k + 1, off + k, 1, k < DIGITAL_MAPPED_CHANNELS, image_len);
}

static void add_analog(io_kind_t kind, size_t mi, const kbus_module_t *m,
                       uint32_t off, uint32_t size_bits, size_t image_len)
{
    // Channel count as reported by the module; fall back to one WORD per
    // 16 bits when the KBUS information does not report it.
    unsigned channels = m->channel_count ? m->channel_count : size_bits / 16u;
    for (unsigned k = 0; k < channels; ++k) {
        const uint32_t byte = 2u * k;
        const int mapped = off % 8u == 0 && byte + 2u <= MODULE_DATA_BYTES &&
                           (byte + 2u) * 8u <= size_bits;
        add_channel(kind, mi, k + 1, off + byte * 8u, 16, mapped, image_len);
    }
}

static void build_channel_map(void)
{
    const size_t count = gw.module_count < GW_MAX_MODULES ? gw.module_count : GW_MAX_MODULES;
    if (gw.module_count > GW_MAX_MODULES)
        LOG_WARN("%zu modules found, only the first %d are handled", gw.module_count, GW_MAX_MODULES);

    memset(gw.mod_has_ch, 0, sizeof(gw.mod_has_ch));
    for (size_t mi = 0; mi < count; ++mi) {
        const kbus_module_t *m = &gw.modules[mi];
        if (m->type == 495) {
            // 750-495 power measurement: no channels; its 24 bytes are exchanged raw
            // (KBus/Exchange), the register communication is done by the client.
            if (gw.count_495++ == 0)
                LOG_INFO("module %u: 750-495 found; no channels, raw process data via KBus/Exchange",
                         m->position);
            continue;
        }
        if (m->type & 0x8000u) {
            add_digital(IO_DI, mi, m->in_offset_bits, m->in_size_bits, gw.in_len);
            add_digital(IO_DO, mi, m->out_offset_bits, m->out_size_bits, gw.out_len);
        } else if (m->in_size_bits && !m->out_size_bits) {
            add_analog(IO_AI, mi, m, m->in_offset_bits, m->in_size_bits, gw.in_len);
        } else if (m->out_size_bits && !m->in_size_bits) {
            add_analog(IO_AO, mi, m, m->out_offset_bits, m->out_size_bits, gw.out_len);
        } else if (m->in_size_bits || m->out_size_bits) {
            LOG_INFO("module %u (75x-%u) is a complex module (in %u / out %u bits): no channels, "
                     "raw process data via KBus/Exchange", m->position, m->type, m->in_size_bits, m->out_size_bits);
        }
    }
}

void gateway_module_name(const kbus_module_t *m, char *buf, size_t len)
{
    if (!(m->type & 0x8000u)) {
        snprintf(buf, len, "75x-%u", m->type);
        return;
    }
    // Bit coded digital module (see getkbusinfo.c)
    const unsigned t = m->type;
    unsigned ch = (t & 0x0800u) ? 8 : (t & 0x0400u) ? 4 : (t & 0x0200u) ? 2 : (t & 0x0100u) ? 1 : 0;
    if (ch == 0)
        ch = m->in_size_bits > m->out_size_bits ? m->in_size_bits : m->out_size_bits;
    const char *dir = ((t & 3u) == 3u) ? "DI/DO" : (t & 1u) ? "DI" : (t & 2u) ? "DO" : "D";
    snprintf(buf, len, "digital %u%s", ch, dir);
}

// ---------------------------------------------------------------------------
// Cycle thread
// ---------------------------------------------------------------------------
static void timespec_add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static int64_t timespec_diff_us(const struct timespec *a, const struct timespec *b)
{
    return (int64_t)(a->tv_sec - b->tv_sec) * 1000000 + (a->tv_nsec - b->tv_nsec) / 1000;
}

static void *cycle_thread(void *arg)
{
    (void)arg;
    static uint8_t out_snapshot[KBUS_IMAGE_BYTES];
    static uint8_t in_buffer[KBUS_IMAGE_BYTES];
    struct timespec next, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (gw.running) {
        pthread_mutex_lock(&gw.lock);
        memcpy(out_snapshot, gw.out, gw.out_len);
        const uint64_t seq = ++gw.seq_started;            // this cycle carries every write made so far
        pthread_mutex_unlock(&gw.lock);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        const int rc = gw.be->exchange(gw.be, out_snapshot, gw.out_len, in_buffer, gw.in_len);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const uint32_t us = (uint32_t)timespec_diff_us(&t1, &t0);

        pthread_mutex_lock(&gw.lock);
        // cycle time can be changed at run time (KBus/CycleTime): read it every cycle
        const int cycle_ms = gw.stats.cycle_ms;
        const long period_ns = (long)cycle_ms * 1000000L;
        const uint32_t rescan_after = RESCAN_AFTER_MS / (uint32_t)cycle_ms;
        gw.stats.last_cycle_us = us;
        if (us > gw.stats.max_cycle_us)
            gw.stats.max_cycle_us = us;
        if (rc == 0) {
            memcpy(gw.in, in_buffer, gw.in_len);
            gw.seq_done = seq;
            pthread_cond_broadcast(&gw.cycle_cond);       // gateway_exchange() waits for it
            gw.stats.cycles++;
            if (gw.consecutive_errors >= ERRORS_UNTIL_UNHEALTHY) {
                LOG_INFO("KBUS cycle recovered");
                gw.rescan_backoff_s = RESCAN_BACKOFF_MIN_S;
            }
            gw.consecutive_errors = 0;
            gw.stats.healthy = 1;
        } else {
            gw.stats.errors++;
            if (++gw.consecutive_errors == ERRORS_UNTIL_UNHEALTHY) {
                gw.stats.healthy = 0;
                LOG_ERROR("KBUS cycle failing (%u errors in a row)", gw.consecutive_errors);
            }
            // Persistent errors usually mean the node was changed (modules added or
            // removed): ask the HTTP thread to rescan. No slow work in this thread.
            if (gw.consecutive_errors == rescan_after && !gw.rescan_requested)
                gw.rescan_requested = 2;
        }
        pthread_mutex_unlock(&gw.lock);

        timespec_add_ns(&next, period_ns);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_diff_us(&now, &next) > 0) {   // overrun: resynchronise
            gw.stats.overruns++;
            next = now;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && gw.running)
            ;
        // scheduling jitter: how late did this thread wake up?
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t late = timespec_diff_us(&now, &next);
        if (late > (int64_t)gw.stats.max_late_us)
            gw.stats.max_late_us = (uint32_t)late;
    }

    // Safe state: switch all outputs off with one last cycle.
    memset(out_snapshot, 0, sizeof(out_snapshot));
    gw.be->exchange(gw.be, out_snapshot, gw.out_len, in_buffer, gw.in_len);
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static int scan_bus(void);
int gateway_init(const gw_config_t *cfg, kbus_backend_t *backend)
{
    memset(&gw, 0, sizeof(gw));
    if (backend == NULL)
        return -1;
    gw.be = backend;
    gw.rt_priority = cfg->rt_priority;
    gw.stats.backend = backend->name;
    gw.stats.cycle_ms = cfg->cycle_ms;
    gw.stats.healthy = 1;

    gw.rescan_backoff_s = RESCAN_BACKOFF_MIN_S;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);  // avoid priority inversion
    pthread_mutex_init(&gw.lock, &attr);
    pthread_mutex_init(&gw.reg_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&gw.cycle_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    const int rc = scan_bus();
    gw.bus_open = rc == 0;
    return rc;
}

// Open the bus, read the modules and build the channel map. Used at startup
// and for every rescan. The cycle thread must not run.
static int scan_bus(void)
{
    kbus_backend_t *backend = gw.be;
    gw.config_version++;                 // the tree changes, also when the scan fails
    if (backend->open(backend, gw.modules, KBUS_MAX_MODULES, &gw.module_count,
                      &gw.in_len, &gw.out_len) != 0) {
        backend->close(backend);
        gw.module_count = 0;
        gw.in_len = gw.out_len = 0;
        return -1;
    }
    if (gw.in_len > KBUS_IMAGE_BYTES || gw.out_len > KBUS_IMAGE_BYTES) {
        LOG_ERROR("process image larger than %d bytes", KBUS_IMAGE_BYTES);
        backend->close(backend);
        gw.module_count = 0;
        gw.in_len = gw.out_len = 0;
        return -1;
    }
    build_channel_map();

    for (size_t mi = 0; mi < gw.module_count; ++mi) {
        char name[32];
        gateway_module_name(&gw.modules[mi], name, sizeof(name));
        LOG_INFO("module %2u: %-14s in %3u bits @%-5u out %3u bits @%u",
                 gw.modules[mi].position, name,
                 gw.modules[mi].in_size_bits, gw.modules[mi].in_offset_bits,
                 gw.modules[mi].out_size_bits, gw.modules[mi].out_offset_bits);
    }
    LOG_INFO("channels: %zu DI, %zu DO, %zu AI, %zu AO; %zu x 750-495 (configuration %u)",
             gw.ch_count[IO_DI], gw.ch_count[IO_DO], gw.ch_count[IO_AI], gw.ch_count[IO_AO],
             gw.count_495, gw.config_version);
    return 0;
}

uint32_t gateway_config_version(void) { return gw.config_version; }

void gateway_request_rescan(void) { gw.rescan_requested = 1; }

int gateway_rescan_due(void)
{
    if (!gw.rescan_requested)
        return 0;
    if (gw.rescan_requested == 1)
        return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return timespec_diff_us(&now, &gw.next_auto_rescan) >= 0;
}

// Rescan the bus (called by the HTTP thread, never by the cycle thread):
// stop the cycle (outputs to 0), close, open + scan again, rebuild the channel
// map, start the cycle. Outputs stay 0 until clients write them again.
int gateway_rescan(void)
{
    const int automatic = gw.rescan_requested == 2;
    gw.rescan_requested = 0;
    LOG_INFO("KBUS rescan (%s)", automatic ? "after persistent cycle errors" : "requested");

    pthread_mutex_lock(&gw.reg_lock);                  // waits for a running register read
    gw.bus_open = 0;
    if (gw.thread_started) {
        gw.running = 0;
        pthread_join(gw.thread, NULL);
        gw.thread_started = 0;
    }
    gw.be->close(gw.be);

    pthread_mutex_lock(&gw.lock);
    gw.module_count = 0;
    gw.count_495 = 0;
    memset(gw.ch_count, 0, sizeof(gw.ch_count));
    memset(gw.in, 0, sizeof(gw.in));
    memset(gw.out, 0, sizeof(gw.out));
    gw.consecutive_errors = 0;
    pthread_mutex_unlock(&gw.lock);

    const int rc = scan_bus();
    gw.bus_open = rc == 0;
    pthread_mutex_unlock(&gw.reg_lock);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (automatic || rc != 0) {
        // the next automatic rescan only after the backoff time
        gw.next_auto_rescan = now;
        gw.next_auto_rescan.tv_sec += gw.rescan_backoff_s;
        gw.rescan_backoff_s = gw.rescan_backoff_s * 2 > RESCAN_BACKOFF_MAX_S ? RESCAN_BACKOFF_MAX_S
                                                                              : gw.rescan_backoff_s * 2;
    }
    if (rc != 0) {
        gw.stats.healthy = 0;
        gw.rescan_requested = 2;                       // try again after the backoff
        LOG_ERROR("KBUS rescan failed; next attempt in %d s", (int)(gw.next_auto_rescan.tv_sec - now.tv_sec));
        return -1;
    }
    gw.stats.healthy = 1;
    return gateway_start();
}

int gateway_start(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    gw.running = 1;

    if (gw.rt_priority > 0) {
        struct sched_param sp = { .sched_priority = gw.rt_priority };
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        if (pthread_create(&gw.thread, &attr, cycle_thread, NULL) == 0) {
            gw.stats.realtime = 1;
            gw.thread_started = 1;
            LOG_INFO("KBUS cycle thread running: SCHED_FIFO priority %d, every %d ms",
                     gw.rt_priority, gw.stats.cycle_ms);
        } else {
            LOG_WARN("real-time scheduling not permitted (run the container --privileged"
                     " or with --cap-add SYS_NICE); falling back to normal priority");
            pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        }
    }
    if (!gw.thread_started) {
        if (pthread_create(&gw.thread, &attr, cycle_thread, NULL) != 0) {
            pthread_attr_destroy(&attr);
            LOG_ERROR("could not start the KBUS cycle thread");
            gw.running = 0;
            return -1;
        }
        gw.thread_started = 1;
        LOG_INFO("KBUS cycle thread running every %d ms (normal priority)", gw.stats.cycle_ms);
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void gateway_stop(void)
{
    pthread_mutex_lock(&gw.reg_lock);
    gw.bus_open = 0;
    if (gw.thread_started) {
        gw.running = 0;
        pthread_join(gw.thread, NULL);
        gw.thread_started = 0;
        LOG_INFO("KBUS stopped, outputs switched off");
    }
    if (gw.be != NULL)
        gw.be->close(gw.be);
    pthread_mutex_unlock(&gw.reg_lock);
}

size_t gateway_channel_count(io_kind_t kind) { return gw.ch_count[kind]; }

const io_channel_t *gateway_channel(io_kind_t kind, size_t number)
{
    if (number == 0 || number > gw.ch_count[kind])
        return NULL;
    return &gw.ch[kind][number - 1];
}

// Inputs: value from the input image (0 when the channel is not exchanged).
// Outputs: the value last written, not a value read back from the module.
int gateway_read(io_kind_t kind, size_t number, int64_t *value)
{
    const io_channel_t *ch = gateway_channel(kind, number);
    if (ch == NULL)
        return GW_ENOENT;
    pthread_mutex_lock(&gw.lock);
    if (io_kind_writable(kind))
        *value = ch->value;
    else
        *value = ch->mapped ? bits_get(gw.in, ch->bit_offset, ch->bit_width) : 0;
    pthread_mutex_unlock(&gw.lock);
    return GW_OK;
}

// Bulk read: every channel of every class under ONE lock, so the values come
// from the same KBUS cycle. Outputs give the value last written.
void gateway_snapshot(uint16_t out[IO_KIND_COUNT][GW_MAX_CHANNELS])
{
    pthread_mutex_lock(&gw.lock);
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        for (size_t i = 0; i < gw.ch_count[k]; ++i) {
            const io_channel_t *ch = &gw.ch[k][i];
            if (io_kind_writable((io_kind_t)k))
                out[k][i] = ch->value;
            else
                out[k][i] = ch->mapped ? (uint16_t)bits_get(gw.in, ch->bit_offset, ch->bit_width) : 0;
        }
    }
    pthread_mutex_unlock(&gw.lock);
}

// Bulk write: vals[k][i] is the new value of channel i+1 of class k, or -1 to
// leave it unchanged; n[k] values per class (0 = class not written). Every
// value is checked first; then all are applied under ONE lock, so the KBUS
// cycle sends them together. Nothing is written when one value is invalid.
// *changed (may be NULL) = 1 when an output value changed.
int gateway_write_multi(const int32_t *const vals[IO_KIND_COUNT], const size_t n[IO_KIND_COUNT],
                        int *changed)
{
    int ch_any = 0;
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        if (n[k] == 0)
            continue;
        if (!io_kind_writable((io_kind_t)k))
            return GW_EREADONLY;
        if (n[k] > gw.ch_count[k])
            return GW_ENOENT;
        for (size_t i = 0; i < n[k]; ++i) {
            const int32_t max = gw.ch[k][i].bit_width == 1 ? 1 : 65535;
            if (vals[k][i] < -1 || vals[k][i] > max)
                return GW_ERANGE;
        }
    }
    pthread_mutex_lock(&gw.lock);
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        for (size_t i = 0; i < n[k]; ++i) {
            if (vals[k][i] < 0)
                continue;
            io_channel_t *ch = &gw.ch[k][i];
            if (ch->value != (uint16_t)vals[k][i])
                ch_any = 1;
            ch->value = (uint16_t)vals[k][i];
            if (ch->mapped)
                bits_set(gw.out, ch->bit_offset, ch->bit_width, (uint32_t)vals[k][i]);
        }
    }
    pthread_mutex_unlock(&gw.lock);
    if (changed)
        *changed = ch_any;
    return GW_OK;
}

const kbus_module_t *gateway_modules(size_t *count)
{
    *count = gw.module_count;
    return gw.modules;
}

int gateway_module_index(uint16_t pos)
{
    for (size_t mi = 0; mi < gw.module_count; ++mi)
        if (gw.modules[mi].position == pos)
            return (int)mi;
    return -1;
}

int gateway_module_has_channels(size_t mi)
{
    return mi < gw.module_count && mi < KBUS_MAX_MODULES && gw.mod_has_ch[mi];
}

// Byte range of the raw data of a module in one direction; 0 = none / not usable raw.
static size_t raw_range(const kbus_module_t *m, int out, size_t *off)
{
    const uint32_t bits = out ? m->out_size_bits : m->in_size_bits;
    const uint32_t start = out ? m->out_offset_bits : m->in_offset_bits;
    const size_t img = out ? gw.out_len : gw.in_len;
    if (bits == 0 || start % 8u != 0)
        return 0;
    const size_t n = (bits + 7u) / 8u;
    if (n > GW_MODULE_RAW_MAX || start / 8u + n > img)
        return 0;
    *off = start / 8u;
    return n;
}

int gateway_module_raw(size_t mi, size_t *in_bytes, size_t *out_bytes)
{
    if (mi >= gw.module_count || gateway_module_has_channels(mi))
        return 0;
    const kbus_module_t *m = &gw.modules[mi];
    size_t off, ni = raw_range(m, 0, &off), no = raw_range(m, 1, &off);
    // unusable when one direction has data that is not byte aligned or too long
    if ((m->in_size_bits && ni == 0) || (m->out_size_bits && no == 0) || (ni == 0 && no == 0))
        return 0;
    if (in_bytes)
        *in_bytes = ni;
    if (out_bytes)
        *out_bytes = no;
    return 1;
}

size_t gateway_module_input(size_t mi, uint8_t *buf, size_t len)
{
    size_t off, n;
    if (!gateway_module_raw(mi, NULL, NULL) || (n = raw_range(&gw.modules[mi], 0, &off)) == 0 || n > len)
        return 0;
    pthread_mutex_lock(&gw.lock);
    memcpy(buf, gw.in + off, n);
    pthread_mutex_unlock(&gw.lock);
    return n;
}

size_t gateway_module_output_read(size_t mi, uint8_t *buf, size_t len)
{
    size_t off, n;
    if (!gateway_module_raw(mi, NULL, NULL) || (n = raw_range(&gw.modules[mi], 1, &off)) == 0 || n > len)
        return 0;
    pthread_mutex_lock(&gw.lock);
    memcpy(buf, gw.out + off, n);
    pthread_mutex_unlock(&gw.lock);
    return n;
}

int gateway_module_output(size_t mi, const uint8_t *buf, size_t len)
{
    size_t off, n;
    if (!gateway_module_raw(mi, NULL, NULL))
        return GW_ENOENT;
    if ((n = raw_range(&gw.modules[mi], 1, &off)) == 0)
        return GW_EREADONLY;
    if (len != n)
        return GW_ERANGE;
    pthread_mutex_lock(&gw.lock);
    memcpy(gw.out + off, buf, n);
    pthread_mutex_unlock(&gw.lock);
    return GW_OK;
}

int gateway_exchange(const size_t *mi, size_t n, const uint8_t *out, size_t out_len,
                     uint8_t *in, size_t in_cap, size_t *in_len,
                     uint64_t *cycle, uint64_t *out_cycle, int timeout_ms)
{
    size_t need_out = 0, need_in = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t ni = 0, no = 0;
        if (!gateway_module_raw(mi[i], &ni, &no))
            return GW_ENOENT;
        need_in += ni;
        need_out += no;
    }
    if ((out_len != 0 && out_len != need_out) || need_in > in_cap)
        return GW_ERANGE;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    timespec_add_ns(&deadline, (long)timeout_ms * 1000000L);

    pthread_mutex_lock(&gw.lock);
    uint64_t want = gw.seq_done + 1;                    // read only: the next completed cycle (fresh)
    *out_cycle = 0;
    if (out_len) {
        size_t o = 0;
        for (size_t i = 0; i < n; ++i) {               // all modules in the SAME cycle
            size_t off = 0, no = raw_range(&gw.modules[mi[i]], 1, &off);
            memcpy(gw.out + off, out + o, no);
            o += no;
        }
        *out_cycle = gw.seq_started + 1;               // the next cycle takes these outputs
        want = *out_cycle + 1;                         // inputs from the cycle AFTER it
    }
    while (gw.seq_done < want) {
        if (pthread_cond_timedwait(&gw.cycle_cond, &gw.lock, &deadline) == ETIMEDOUT && gw.seq_done < want) {
            pthread_mutex_unlock(&gw.lock);
            return GW_ETIMEOUT;
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t off = 0, ni = raw_range(&gw.modules[mi[i]], 0, &off);
        memcpy(in + o, gw.in + off, ni);
        o += ni;
    }
    *in_len = o;
    *cycle = gw.seq_done;
    pthread_mutex_unlock(&gw.lock);
    return GW_OK;
}

uint64_t gateway_cycle_number(void)
{
    pthread_mutex_lock(&gw.lock);
    const uint64_t c = gw.seq_done;
    pthread_mutex_unlock(&gw.lock);
    return c;
}

int gateway_reg_read(uint16_t pos, uint16_t table, uint16_t reg, uint16_t count, uint16_t *buf, uint16_t *n)
{
    if (count == 0 || count > 16)
        return GW_ERANGE;
    if (gw.be == NULL || gw.be->reg_read == NULL)
        return GW_EUNAVAILABLE;
    pthread_mutex_lock(&gw.reg_lock);
    int rc = GW_EOFFLINE;
    if (gw.bus_open) {
        *n = 0;
        const int r = gw.be->reg_read(gw.be, pos, table, reg, count, buf, n);
        rc = r == KBUS_REG_OK ? GW_OK : r == KBUS_REG_UNAVAILABLE ? GW_EUNAVAILABLE : GW_EFAILED;
    }
    pthread_mutex_unlock(&gw.reg_lock);
    return rc;
}

void gateway_stats(gw_stats_t *out)
{
    pthread_mutex_lock(&gw.lock);
    *out = gw.stats;
    pthread_mutex_unlock(&gw.lock);
}

int gateway_set_cycle_ms(int ms)
{
    if (ms < GW_CYCLE_MS_MIN || ms > GW_CYCLE_MS_MAX)
        return GW_ERANGE;
    pthread_mutex_lock(&gw.lock);
    const int old = gw.stats.cycle_ms;
    if (ms != old) {
        gw.stats.cycle_ms = ms;
        gw.stats.max_cycle_us = 0;                        // the statistics belong to one cycle time
        gw.stats.max_late_us = 0;
        gw.stats.overruns = 0;
    }
    pthread_mutex_unlock(&gw.lock);
    return old;
}

void gateway_reset_stats(void)
{
    pthread_mutex_lock(&gw.lock);
    gw.stats.cycles = 0;                                  // KBus/ResetStatistics: all statistics
    gw.stats.errors = 0;
    gw.stats.max_cycle_us = 0;
    gw.stats.max_late_us = 0;
    gw.stats.overruns = 0;
    pthread_mutex_unlock(&gw.lock);
}
