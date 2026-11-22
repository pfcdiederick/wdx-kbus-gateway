// Simulated KBUS for testing without a PFC (KBUS_SIMULATION=1).
//
// Simulated node (positions 1..5 and their channels are the same as before 2.6.0:
// 12 DI, 8 DO, 4 AI, 4 AO; the complex modules without channels come behind them):
//   pos 1  750-455  4AI   in  bits   0..63  (AI1..AI3 ramps, AI4 = loopback of AO1)
//   pos 2  750-559  4AO   out bits   0..63
//   pos 3  750-430  8DI   in  bits  64..71  (loopback of DO1..DO8)
//   pos 4  750-530  8DO   out bits  64..71
//   pos 5  750-402  4DI   in  bits  72..75  (binary seconds counter)
//   pos 6  750-671  stepper, 12 bytes: in bits 80..175, out bits 72..167 (model of the
//                   positioning handshake, see sim_stepper; taken from the MQTT gateway)
//   pos 7  "750-657" 24 bytes: in bits 176..367, out bits 168..359. NOT a model of the
//                   657: its inputs are its outputs of the previous cycle (mirror), to test
//                   the order of KBus/Exchange
//   pos 8  750-450  4RTD  in bits 368..431 (AI5..AI8: 21.4, -12.5, 850.0 degC, AI8 = AO2),
//                   ONLY with KBUS_SIM_450=1 (it adds 4 analog input channels)
//
// Register communication (read only, KBUS_REG_*): every module except the digital ones,
// one table per channel. Register 6 of the 750-450 = 0x0060 while the channel shows
// 850.0 degC or more (like a real 750-450 with an open input), else 0; register 8 =
// module type; register 30 = type * 10 (order number); everything else 0. 20 ms per read.
//
// Hot-plug test (KBUS_SIM_HOTPLUG=<file>): while <file> exists, one more module
// "was added" behind the last one (750-430 8DI, 1 byte, all inputs TRUE). Until the bus
// is opened again, every cycle fails, as the real KBUS does after a change; the
// next open (rescan) then reports it. Delete the file to remove it.
#include "backend.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int sim_extra;          // 1 = the hot-plug module was present at the last open
static int sim_450;            // KBUS_SIM_450=1: the 750-450 is part of the node

// For the register reads (other thread than the cycle): modules and input image.
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static kbus_module_t   sim_mods[10];
static size_t          sim_count;
static uint8_t         sim_in[64];

#define SIM_IN_BASE   46       // input bytes without the 450 and the hot-plug module
#define SIM_OUT_BYTES 45

static int hotplug_present(void)
{
    const char *f = getenv("KBUS_SIM_HOTPLUG");
    return f != NULL && *f != '\0' && access(f, F_OK) == 0;
}

static void put16(uint8_t *img, size_t byte, uint16_t v)
{
    img[byte]     = (uint8_t)(v & 0xffu);
    img[byte + 1] = (uint8_t)(v >> 8);
}

static int sim_open(kbus_backend_t *self, kbus_module_t *m, size_t max_modules,
                    size_t *module_count, size_t *in_bytes, size_t *out_bytes)
{
    (void)self;
    static const kbus_module_t node[] = {
        { 1, 455,      0, 64,   0,   0, 4, 0 },
        { 2, 559,      0,  0,   0,  64, 4, 0 },
        { 3, 0x8801,  64,  8,   0,   0, 8, 0 },  // digital, 8 channels, input
        { 4, 0x8802,   0,  0,  64,   8, 8, 0 },  // digital, 8 channels, output
        { 5, 0x8401,  72,  4,   0,   0, 4, 0 },  // digital, 4 channels, input
        { 6, 671,     80, 96,  72,  96, 0, 0 },  // stepper controller, 12 bytes in/out
        { 7, 657,    176, 192, 168, 192, 0, 0 }, // 24 bytes in/out, mirror (see above)
    };
    const char *e450 = getenv("KBUS_SIM_450");
    sim_450 = e450 != NULL && strcmp(e450, "1") == 0;
    sim_extra = hotplug_present();
    kbus_module_t all[10];
    size_t n = sizeof(node) / sizeof(node[0]);
    memcpy(all, node, sizeof(node));
    size_t in_len = SIM_IN_BASE;
    if (sim_450) {
        all[n] = (kbus_module_t){ (uint16_t)(n + 1), 450, (uint32_t)in_len * 8u, 64, 0, 0, 4, 0 };
        n++;
        in_len += 8;
    }
    if (sim_extra) {                                       // digital, 8 channels, input
        all[n] = (kbus_module_t){ (uint16_t)(n + 1), 0x8801, (uint32_t)in_len * 8u, 8, 0, 0, 8, 0 };
        n++;
        in_len += 1;
    }
    if (n > max_modules)
        n = max_modules;
    memcpy(m, all, n * sizeof(all[0]));
    pthread_mutex_lock(&sim_lock);
    memcpy(sim_mods, all, n * sizeof(all[0]));
    sim_count = n;
    pthread_mutex_unlock(&sim_lock);
    *module_count = n;
    *in_bytes = in_len;
    *out_bytes = SIM_OUT_BYTES;
    LOG_WARN("SIMULATION mode: no real KBUS I/O is read or written%s", sim_450 ? " (with 750-450)" : "");
    return 0;
}

// 750-671 model (manual 750-671, 2.1.2.11.1): enough of the positioning handshake to
// test the driver. o = 12 output bytes (C0, -, D0..D6, C3, C2, C1), i = 12 input bytes.
static struct { int32_t pos, target, vel; int busy, on_target, ref_ok, error, reset, ref_left; uint8_t c1, c2, c3; } stp = { .reset = 1 };

static void sim_stepper(const uint8_t *o, uint8_t *i, uint32_t dt_ms)
{
    const uint8_t c1 = o[11], c2 = o[10], c3 = o[9];
    if ((c2 & 0x80) && !(stp.c2 & 0x80))
        stp.error = 0;                                     // Error_Quit edge
    if ((c3 & 0x80) && !(stp.c3 & 0x80))
        stp.reset = 0;                                     // Reset_Quit edge
    const int ready = (c1 & 0x01) && !stp.error;
    const int stop_ack = ready && (c1 & 0x02);              // Input1 is closed in the simulation
    uint8_t mode_ack = 0;
    if (stop_ack)
        mode_ack = c1 & 0xF8;                               // one mode at a time is enough here
    if (!ready || !(mode_ack & c1)) {                       // mode ended: stop (at once here)
        stp.busy = 0;
        stp.vel = 0;
    }
    const int start_ack = (c1 & 0x04) && mode_ack;
    if ((c1 & 0x04) && !(stp.c1 & 0x04) && mode_ack) {      // Start rising edge
        const int32_t v = (int32_t)(int16_t)(o[2] | (o[3] << 8));
        int32_t t = o[6] | (o[7] << 8) | (o[8] << 16);
        if (t & 0x800000)
            t -= 0x1000000;
        if (mode_ack & 0x08) {                              // positioning
            if (v < 1 || v > 25000) {
                stp.error = 1;
            } else {
                stp.target = t;
                stp.vel = v;
                stp.busy = stp.pos != t;
                stp.on_target = stp.pos == t;
            }
        } else if (mode_ack & 0x20) {                       // referencing: 0.5 s, then position 0
            stp.busy = 1;
            stp.on_target = 0;
            stp.ref_ok = 0;
            stp.ref_left = 500;
        }
    }
    if (stp.busy && (mode_ack & 0x08)) {
        int32_t step = (int32_t)((int64_t)stp.vel * dt_ms / 1000);
        if (step < 1)
            step = 1;
        const int32_t d = stp.target - stp.pos;
        stp.pos += d > step ? step : d < -step ? -step : d;
        if (stp.pos == stp.target) {
            stp.busy = 0;
            stp.on_target = 1;
        }
    } else if (stp.busy && (mode_ack & 0x20)) {
        stp.ref_left -= (int)dt_ms;
        stp.pos += (c3 & 0x08) ? -1 : 1;
        if (stp.ref_left <= 0) {
            stp.busy = 0;
            stp.pos = 0;
            stp.ref_ok = 1;
            stp.on_target = 1;
        }
    }
    stp.c1 = c1;
    stp.c2 = c2;
    stp.c3 = c3;
    const int16_t av = stp.busy ? (int16_t)stp.vel : 0;
    memset(i, 0, 12);
    i[0] = stp.error ? 0x40 : 0;
    i[2] = (uint8_t)av;
    i[3] = (uint8_t)(av >> 8);
    i[6] = (uint8_t)stp.pos;
    i[7] = (uint8_t)(stp.pos >> 8);
    i[8] = (uint8_t)(stp.pos >> 16);
    i[9] = (uint8_t)((stp.reset ? 0x80 : 0) | 0x01);                       // S3: Reset, Input1
    i[10] = (uint8_t)((stp.error ? 0x80 : 0) | (stp.ref_ok ? 0x20 : 0) | (stp.busy ? 0x02 : 0x04)
                      | (stp.on_target ? 0x01 : 0));                        // S2
    i[11] = (uint8_t)((ready ? 0x01 : 0) | (stop_ack ? 0x02 : 0) | (start_ack ? 0x04 : 0) | mode_ack); // S1
}

static int sim_exchange(kbus_backend_t *self, const uint8_t *out, size_t out_len,
                        uint8_t *in, size_t in_len)
{
    (void)self;
    const size_t want = SIM_IN_BASE + (sim_450 ? 8u : 0u) + (sim_extra ? 1u : 0u);
    if (in_len < want || out_len < SIM_OUT_BYTES)
        return -1;
    if (hotplug_present() != sim_extra)
        return -1;                                                 // node changed: bus error until rescan
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    static uint32_t last_ms;
    const uint32_t dt = last_ms ? ms - last_ms : 0;
    last_ms = ms;

    put16(in, 0, (uint16_t)((ms / 2) % 32768u));                 // AI1: sawtooth ~16 s
    uint32_t tri = (ms / 4) % 65536u;                              // AI2: triangle
    put16(in, 2, (uint16_t)(tri < 32768u ? tri : 65535u - tri));
    put16(in, 4, 16384);                                           // AI3: constant
    in[6] = out[0];                                                // AI4 = AO1
    in[7] = out[1];
    in[8] = out[8];                                                // DI1..8 = DO1..8
    in[9] = (uint8_t)(now.tv_sec & 0x0f);                          // DI9..12 = counter
    sim_stepper(out + 9, in + 10, dt);                             // 750-671 at position 6
    static uint8_t mirror[24];                                     // position 7: last cycle's outputs
    memcpy(in + 22, mirror, sizeof(mirror));
    memcpy(mirror, out + 21, sizeof(mirror));
    size_t o = SIM_IN_BASE;
    if (sim_450) {                                                 // position 8: 750-450
        put16(in, o, 214);                                         // 21.4 degC
        put16(in, o + 2, (uint16_t)(int16_t)-125);                 // -12.5 degC
        put16(in, o + 4, 8500);                                    // 850.0 degC (open input)
        in[o + 6] = out[2];                                        // = AO2 (raw)
        in[o + 7] = out[3];
        o += 8;
    }
    if (sim_extra)
        in[o] = 0xff;                                              // hot-plugged 8DI
    pthread_mutex_lock(&sim_lock);
    memcpy(sim_in, in, in_len < sizeof(sim_in) ? in_len : sizeof(sim_in));
    pthread_mutex_unlock(&sim_lock);
    return 0;
}

static int sim_reg_read(kbus_backend_t *self, uint16_t terminal, uint16_t table, uint16_t reg,
                        uint16_t count, uint16_t *buf, uint16_t *n)
{
    (void)self;
    usleep(20000);                                                 // like the real register access
    pthread_mutex_lock(&sim_lock);
    int rc = KBUS_REG_FAILED;
    if (terminal >= 1 && terminal <= sim_count) {
        const kbus_module_t *m = &sim_mods[terminal - 1];
        const unsigned tables = m->channel_count ? m->channel_count : 1;
        if (!(m->type & 0x8000u) && table < tables) {
            for (uint16_t i = 0; i < count; ++i) {
                const unsigned r = reg + i;
                uint16_t v = 0;
                if (r == 8)
                    v = m->type;
                else if (r == 30)
                    v = (uint16_t)(m->type * 10u);
                else if (r == 6 && m->type == 450) {
                    const size_t b = m->in_offset_bits / 8u + 2u * table;
                    const int16_t w = (int16_t)(sim_in[b] | (sim_in[b + 1] << 8));
                    v = w >= 8500 ? 0x0060 : 0;
                }
                buf[i] = v;
            }
            *n = count;
            rc = KBUS_REG_OK;
        }
    }
    pthread_mutex_unlock(&sim_lock);
    return rc;
}

static void sim_close(kbus_backend_t *self)
{
    (void)self;
}

kbus_backend_t *backend_sim_create(const gw_config_t *cfg)
{
    (void)cfg;
    static kbus_backend_t backend = {
        .name = "simulation", .open = sim_open, .exchange = sim_exchange,
        .close = sim_close, .reg_read = sim_reg_read, .priv = NULL,
    };
    return &backend;
}
