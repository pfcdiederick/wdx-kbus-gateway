// Simulated KBUS for testing without a PFC (KBUS_SIMULATION=1).
//
// Simulated node (same process-image layout rules as the real KBUS: complex /
// analog data first, digital data packed behind it):
//   pos 1  750-455  4AI   in  bits   0..63  (AI1..AI3 ramps, AI4 = loopback of AO1)
//   pos 2  750-559  4AO   out bits   0..63
//   pos 3  750-430  8DI   in  bits  64..71  (loopback of DO1..DO8)
//   pos 4  750-530  8DO   out bits  64..71
//   pos 5  750-402  4DI   in  bits  72..75  (binary seconds counter)
//
// Hot-plug test (KBUS_SIM_HOTPLUG=<file>): while <file> exists, a 6th module
// "was added" (750-430 8DI, in bits 80..87, all inputs TRUE). Until the bus is
// opened again, every cycle fails, as the real KBUS does after a change; the
// next open (rescan) then reports 6 modules. Delete the file to remove it.
#include "backend.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int sim_extra;          // 1 = the 6th module was present at the last open

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
        { 1, 455,    0, 64,  0,  0, 4, 0 },
        { 2, 559,    0,  0,  0, 64, 4, 0 },
        { 3, 0x8801, 64, 8,  0,  0, 8, 0 },  // digital, 8 channels, input
        { 4, 0x8802, 0,  0, 64,  8, 8, 0 },  // digital, 8 channels, output
        { 5, 0x8401, 72, 4,  0,  0, 4, 0 },  // digital, 4 channels, input
        { 6, 0x8801, 80, 8,  0,  0, 8, 0 },  // hot-plug test only: digital, 8 channels, input
    };
    sim_extra = hotplug_present();
    size_t n = sizeof(node) / sizeof(node[0]) - (sim_extra ? 0 : 1);
    if (n > max_modules)
        n = max_modules;
    memcpy(m, node, n * sizeof(node[0]));
    *module_count = n;
    *in_bytes = sim_extra ? 11 : 10;
    *out_bytes = 9;
    LOG_WARN("SIMULATION mode: no real KBUS I/O is read or written");
    return 0;
}

static int sim_exchange(kbus_backend_t *self, const uint8_t *out, size_t out_len,
                        uint8_t *in, size_t in_len)
{
    (void)self;
    if (in_len < 10 || out_len < 9)
        return -1;
    if (hotplug_present() != sim_extra)
        return -1;                                                 // node changed: bus error until rescan
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);

    put16(in, 0, (uint16_t)((ms / 2) % 32768u));                 // AI1: sawtooth ~16 s
    uint32_t tri = (ms / 4) % 65536u;                              // AI2: triangle
    put16(in, 2, (uint16_t)(tri < 32768u ? tri : 65535u - tri));
    put16(in, 4, 16384);                                           // AI3: constant
    in[6] = out[0];                                                // AI4 = AO1
    in[7] = out[1];
    in[8] = out[8];                                                // DI1..8 = DO1..8
    in[9] = (uint8_t)(now.tv_sec & 0x0f);                          // DI9..12 = counter
    if (sim_extra && in_len >= 11)
        in[10] = 0xff;                                             // DI13..20 (hot-plugged 8DI)
    return 0;
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
        .close = sim_close, .priv = NULL,
    };
    return &backend;
}
