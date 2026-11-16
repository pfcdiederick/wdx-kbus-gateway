// Abstract KBUS backend: the real WAGO ADI/DAL backend or a simulation.
#ifndef KBUS_GW_BACKEND_H
#define KBUS_GW_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define KBUS_MAX_MODULES  255
#define KBUS_IMAGE_BYTES  16384

// One I/O module (terminal) on the KBUS. All offsets are ABSOLUTE bit offsets
// in the ADI process images; the backend resolves any digital offset.
typedef struct {
    uint16_t position;        // 1-based slot behind the controller
    uint16_t type;            // raw terminal code (bit 15 set = simple digital module)
    uint32_t in_offset_bits;
    uint32_t in_size_bits;
    uint32_t out_offset_bits;
    uint32_t out_size_bits;
    uint16_t channel_count;   // from KBUS info (complex modules)
    uint16_t pi_format;
} kbus_module_t;

typedef struct kbus_backend kbus_backend_t;

struct kbus_backend {
    const char *name;
    // Connect to the bus and describe the attached modules and image sizes.
    int  (*open)(kbus_backend_t *self, kbus_module_t *modules, size_t max_modules,
                 size_t *module_count, size_t *in_bytes, size_t *out_bytes);
    // One bus cycle: write the output image, trigger the cycle, read the input image.
    int  (*exchange)(kbus_backend_t *self, const uint8_t *out, size_t out_len,
                     uint8_t *in, size_t in_len);
    void (*close)(kbus_backend_t *self);
    void *priv;
};

kbus_backend_t *backend_adi_create(const gw_config_t *cfg);
kbus_backend_t *backend_sim_create(const gw_config_t *cfg);

#endif
