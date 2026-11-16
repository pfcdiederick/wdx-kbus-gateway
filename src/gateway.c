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

static struct {
    kbus_backend_t *be;
    kbus_module_t   modules[KBUS_MAX_MODULES];
    size_t          module_count;

    io_channel_t    ch[IO_KIND_COUNT][GW_MAX_CHANNELS];
    size_t          ch_count[IO_KIND_COUNT];
    size_t          count_495;

    uint8_t         in[KBUS_IMAGE_BYTES];
    uint8_t         out[KBUS_IMAGE_BYTES];
    size_t          in_len, out_len;

    pthread_mutex_t lock;
    pthread_t       thread;
    volatile int    running;
    int             thread_started;
    int             rt_priority;
    uint32_t        consecutive_errors;
    gw_stats_t      stats;
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
// - 750-495: no channels, process data not exchanged.
// - Other modules: no channels; their outputs stay 0.
// ---------------------------------------------------------------------------
#define MODULE_DATA_BYTES 48
#define DIGITAL_MAPPED_CHANNELS 16

static void add_channel(io_kind_t kind, size_t module_index, unsigned module_channel,
                        uint32_t bit_offset, uint8_t width, int mapped, size_t image_len)
{
    if (gw.ch_count[kind] >= GW_MAX_CHANNELS)
        return;   // channels beyond 250 are not assigned
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

    for (size_t mi = 0; mi < count; ++mi) {
        const kbus_module_t *m = &gw.modules[mi];
        if (m->type == 495) {
            // 750-495 power measurement: excluded from the generic handling.
            if (gw.count_495++ == 0)
                LOG_INFO("module %u: 750-495 found; no channels, measured values not available",
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
            LOG_WARN("module %u (75x-%u) is a complex module (in %u / out %u bits), no channels",
                     m->position, m->type, m->in_size_bits, m->out_size_bits);
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
    const long period_ns = (long)gw.stats.cycle_ms * 1000000L;
    struct timespec next, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (gw.running) {
        pthread_mutex_lock(&gw.lock);
        memcpy(out_snapshot, gw.out, gw.out_len);
        pthread_mutex_unlock(&gw.lock);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        const int rc = gw.be->exchange(gw.be, out_snapshot, gw.out_len, in_buffer, gw.in_len);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const uint32_t us = (uint32_t)timespec_diff_us(&t1, &t0);

        pthread_mutex_lock(&gw.lock);
        gw.stats.last_cycle_us = us;
        if (us > gw.stats.max_cycle_us)
            gw.stats.max_cycle_us = us;
        if (rc == 0) {
            memcpy(gw.in, in_buffer, gw.in_len);
            gw.stats.cycles++;
            if (gw.consecutive_errors >= ERRORS_UNTIL_UNHEALTHY)
                LOG_INFO("KBUS cycle recovered");
            gw.consecutive_errors = 0;
            gw.stats.healthy = 1;
        } else {
            gw.stats.errors++;
            if (++gw.consecutive_errors == ERRORS_UNTIL_UNHEALTHY) {
                gw.stats.healthy = 0;
                LOG_ERROR("KBUS cycle failing (%u errors in a row)", gw.consecutive_errors);
            }
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
    }

    // Safe state: switch all outputs off with one last cycle.
    memset(out_snapshot, 0, sizeof(out_snapshot));
    gw.be->exchange(gw.be, out_snapshot, gw.out_len, in_buffer, gw.in_len);
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
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

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);  // avoid priority inversion
    pthread_mutex_init(&gw.lock, &attr);
    pthread_mutexattr_destroy(&attr);

    if (backend->open(backend, gw.modules, KBUS_MAX_MODULES, &gw.module_count,
                      &gw.in_len, &gw.out_len) != 0) {
        backend->close(backend);
        return -1;
    }
    if (gw.in_len > KBUS_IMAGE_BYTES || gw.out_len > KBUS_IMAGE_BYTES) {
        LOG_ERROR("process image larger than %d bytes", KBUS_IMAGE_BYTES);
        backend->close(backend);
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
    LOG_INFO("channels: %zu DI, %zu DO, %zu AI, %zu AO; %zu x 750-495",
             gw.ch_count[IO_DI], gw.ch_count[IO_DO], gw.ch_count[IO_AI], gw.ch_count[IO_AO],
             gw.count_495);
    return 0;
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
    if (gw.thread_started) {
        gw.running = 0;
        pthread_join(gw.thread, NULL);
        gw.thread_started = 0;
        LOG_INFO("KBUS stopped, outputs switched off");
    }
    if (gw.be != NULL)
        gw.be->close(gw.be);
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

int gateway_write(io_kind_t kind, size_t number, int64_t value)
{
    if (!io_kind_writable(kind))
        return GW_EREADONLY;
    if (number == 0 || number > gw.ch_count[kind])
        return GW_ENOENT;
    io_channel_t *ch = &gw.ch[kind][number - 1];
    const int64_t max = ch->bit_width == 1 ? 1 : 65535;
    if (value < 0 || value > max)
        return GW_ERANGE;

    pthread_mutex_lock(&gw.lock);
    ch->value = (uint16_t)value;
    if (ch->mapped)
        bits_set(gw.out, ch->bit_offset, ch->bit_width, (uint32_t)value);
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
int gateway_write_multi(const int32_t *const vals[IO_KIND_COUNT], const size_t n[IO_KIND_COUNT])
{
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
            ch->value = (uint16_t)vals[k][i];
            if (ch->mapped)
                bits_set(gw.out, ch->bit_offset, ch->bit_width, (uint32_t)vals[k][i]);
        }
    }
    pthread_mutex_unlock(&gw.lock);
    return GW_OK;
}

const kbus_module_t *gateway_modules(size_t *count)
{
    *count = gw.module_count;
    return gw.modules;
}

void gateway_stats(gw_stats_t *out)
{
    pthread_mutex_lock(&gw.lock);
    *out = gw.stats;
    pthread_mutex_unlock(&gw.lock);
}
