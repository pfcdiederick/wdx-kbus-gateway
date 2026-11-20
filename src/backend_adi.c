// WAGO ADI/DAL backend.
//
// Follows the WAGO examples in docs/reference/HowTo_ADI-MyKBusApplikation:
//   - getkbusinfo.c : module discovery through the KBUS-info D-Bus API
//   - kbusdemo.c    : KBUS in application state "Running", every cycle is
//                     triggered with "libpackbus_Push"
//
// The WAGO libraries are NOT linked at build time. They are dlopen()'ed at
// runtime from the controller's firmware (/usr/lib), so the gateway always
// uses the libraries that match the installed firmware and the build only
// needs the SDK headers.
#define _GNU_SOURCE  // RTLD_DEFAULT (needed with the glibc 2.35 of the WAGO toolchain)
#include "backend.h"
#include "log.h"

#include <stdlib.h>

#ifndef HAVE_WAGO_ADI

kbus_backend_t *backend_adi_create(const gw_config_t *cfg)
{
    (void)cfg;
    LOG_ERROR("this binary was built WITHOUT the WAGO ADI/DAL headers (sdk/include);"
              " only KBUS_SIMULATION=1 is available");
    return NULL;
}

#else

#include <dal/adi_application_interface.h>
#include <ldkc_kbus_information.h>

// Defined in <ldkc_kbus_register_communication.h>, which also pulls in glib.
#ifndef LDKC_KBUS_TERMINAL_COUNT_MAX
#define LDKC_KBUS_TERMINAL_COUNT_MAX (255U)
#endif

#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    const gw_config_t *cfg;
    tApplicationDeviceInterface *adi;
    tDeviceId dev;
    int adi_initialized;
    int dev_open;
    uint32_t task_id;

    __typeof__(&adi_GetApplicationInterface)   get_adi;
    __typeof__(&ldkc_KbusInfo_Create)          info_create;
    __typeof__(&ldkc_KbusInfo_GetStatus)       info_status;
    __typeof__(&ldkc_KbusInfo_GetTerminalInfo) info_terminal_info;
    __typeof__(&ldkc_KbusInfo_GetTerminalList) info_terminal_list;
    __typeof__(&ldkc_KbusInfo_Destroy)         info_destroy;
} adi_priv_t;

// Try every colon separated candidate until one loads (RTLD_GLOBAL so that the
// symbols and their dependencies become visible through RTLD_DEFAULT).
static int load_first(const char *candidates)
{
    char list[512];
    snprintf(list, sizeof(list), "%s", candidates);
    for (char *save = NULL, *name = strtok_r(list, ":", &save); name != NULL;
         name = strtok_r(NULL, ":", &save)) {
        if (dlopen(name, RTLD_NOW | RTLD_GLOBAL) != NULL) {
            LOG_INFO("loaded %s", name);
            return 0;
        }
        LOG_WARN("dlopen(%s): %s", name, dlerror());
    }
    return -1;
}

#define RESOLVE(field, symbol)                                         \
    do {                                                               \
        *(void **)(&p->field) = dlsym(RTLD_DEFAULT, symbol);           \
        if (p->field == NULL) {                                        \
            LOG_ERROR("symbol %s not found in the WAGO libraries", symbol); \
            return -1;                                                 \
        }                                                              \
    } while (0)

static int resolve_symbols(adi_priv_t *p)
{
    if (load_first(p->cfg->dal_libs) != 0) {
        LOG_ERROR("could not load the ADI/DAL library. Is the host /usr/lib mounted and"
                  " LD_LIBRARY_PATH set? (see README, 'Run on the PFC')");
        return -1;
    }
    RESOLVE(get_adi, "adi_GetApplicationInterface");

    // The KBUS info functions usually come in through libdal's dependencies;
    // load the dedicated library only when they are not visible yet.
    if (dlsym(RTLD_DEFAULT, "ldkc_KbusInfo_Create") == NULL &&
        load_first(p->cfg->info_libs) != 0) {
        LOG_ERROR("could not load the KBUS information library (KBUS_INFO_LIB)");
        return -1;
    }
    RESOLVE(info_create,        "ldkc_KbusInfo_Create");
    RESOLVE(info_status,        "ldkc_KbusInfo_GetStatus");
    RESOLVE(info_terminal_info, "ldkc_KbusInfo_GetTerminalInfo");
    RESOLVE(info_terminal_list, "ldkc_KbusInfo_GetTerminalList");
    RESOLVE(info_destroy,       "ldkc_KbusInfo_Destroy");
    return 0;
}

static int set_state(adi_priv_t *p, tApplicationState state, const char *name)
{
    tApplicationStateChangedEvent event;
    memset(&event, 0, sizeof(event));
    event.State = state;
    if (p->adi->ApplicationStateChanged(event) != DAL_SUCCESS) {
        LOG_ERROR("setting application state to '%s' failed", name);
        return -1;
    }
    LOG_INFO("application state set to '%s'", name);
    return 0;
}

// Start of the digital area in the process image, in bytes. Digital data is
// placed behind the analog/complex data; libpackbus reports where.
static uint32_t digital_offset_bytes(adi_priv_t *p, bool output, uint32_t analog_bits)
{
    int32_t func_ret = DAL_FAILURE;
    uint16_t active = 0, internal = 0;
    if (p->adi->CallDeviceSpecificFunction("libpackbus_get_dig_offset", &func_ret,
                                           output, &active, &internal) == DAL_SUCCESS &&
        func_ret == DAL_SUCCESS) {
        return active != 0 ? active : internal;
    }
    LOG_WARN("libpackbus_get_dig_offset unavailable, deriving the %s digital offset"
             " from the analog bit count", output ? "output" : "input");
    return (analog_bits + 7u) / 8u;
}

// Decide whether the KBUS-info digital offsets are relative to the digital
// area (and must be shifted) or already absolute.
static bool digital_offsets_relative(const char *mode, const kbus_module_t *m, size_t n,
                                     uint32_t dig_in_bits, uint32_t dig_out_bits)
{
    if (strcmp(mode, "relative") == 0)
        return true;
    if (strcmp(mode, "absolute") == 0)
        return false;
    // auto: an absolute digital offset can never point into the analog area.
    for (size_t i = 0; i < n; ++i) {
        if (!(m[i].type & 0x8000u))
            continue;
        if ((m[i].in_size_bits && m[i].in_offset_bits < dig_in_bits) ||
            (m[i].out_size_bits && m[i].out_offset_bits < dig_out_bits))
            return true;
    }
    return false;
}

static int adi_open(kbus_backend_t *self, kbus_module_t *modules, size_t max_modules,
                    size_t *module_count, size_t *in_bytes, size_t *out_bytes)
{
    adi_priv_t *p = self->priv;

    if (resolve_symbols(p) != 0)
        return -1;

    p->adi = p->get_adi();
    if (p->adi == NULL || p->adi->Init() != DAL_SUCCESS) {
        LOG_ERROR("ADI Init failed");
        return -1;
    }
    p->adi_initialized = 1;

    tDeviceInfo devices[10];
    size_t device_count = 0;
    if (p->adi->ScanDevices() != DAL_SUCCESS ||
        p->adi->GetDeviceList(sizeof(devices), devices, &device_count) != DAL_SUCCESS) {
        LOG_ERROR("ADI device scan failed");
        return -1;
    }
    int found = -1;
    for (size_t i = 0; i < device_count && i < 10; ++i) {
        if (strcmp(devices[i].DeviceName, "libpackbus") == 0)
            found = (int)i;
    }
    if (found < 0) {
        LOG_ERROR("no KBUS device (libpackbus) found");
        return -1;
    }
    p->dev = devices[found].DeviceId;

    if (p->adi->OpenDevice(p->dev) != DAL_SUCCESS) {
        LOG_ERROR("opening the KBUS device failed. Only ONE KBUS master is allowed:"
                  " stop the CODESYS/e!RUNTIME PLC runtime first (see README)");
        return -1;
    }
    p->dev_open = 1;
    LOG_INFO("KBUS device opened");

    // "Unconfigured": libpackbus drives the bus itself and the KBUS info is valid.
    if (set_state(p, ApplicationState_Unconfigured, "Unconfigured") != 0)
        return -1;

    uint32_t in_size = 0, out_size = 0;
    if (p->adi->GetIoSizes(p->dev, &in_size, &out_size) != DAL_SUCCESS) {
        LOG_ERROR("ADI GetIoSizes failed");
        return -1;
    }

    if (p->info_create() == KbusInfo_Failed) {
        LOG_ERROR("ldkc_KbusInfo_Create failed (is the host D-Bus socket mounted?)");
        return -1;
    }

    int rc = -1;
    tldkc_KbusInfo_Status status;
    static tldkc_KbusInfo_TerminalInfo info[LDKC_KBUS_TERMINAL_COUNT_MAX];
    static u16 terminals[LDKC_KBUS_TERMINAL_COUNT_MAX];
    size_t terminal_count = 0;

    memset(&status, 0, sizeof(status));
    if (p->info_status(&status) == KbusInfo_Failed ||
        p->info_terminal_info(LDKC_KBUS_TERMINAL_COUNT_MAX, info, &terminal_count) == KbusInfo_Failed ||
        p->info_terminal_list(LDKC_KBUS_TERMINAL_COUNT_MAX, terminals, NULL) == KbusInfo_Failed) {
        LOG_ERROR("reading the KBUS configuration via D-Bus failed");
        goto out;
    }
    LOG_INFO("KBUS: %d modules, error code %d, bits AI=%d AO=%d DI=%d DO=%d, image in=%u out=%u bytes",
             (int)status.TerminalCount, (int)status.ErrorCode,
             (int)status.BitCountAnalogInput, (int)status.BitCountAnalogOutput,
             (int)status.BitCountDigitalInput, (int)status.BitCountDigitalOutput,
             in_size, out_size);
    if (status.ErrorCode != 0)
        LOG_WARN("KBUS reports error code %d (arg %d, position %d)",
                 (int)status.ErrorCode, (int)status.ErrorArg, (int)status.ErrorPos);

    if (terminal_count > max_modules || terminal_count > LDKC_KBUS_TERMINAL_COUNT_MAX)
        terminal_count = max_modules < LDKC_KBUS_TERMINAL_COUNT_MAX ? max_modules
                                                                     : LDKC_KBUS_TERMINAL_COUNT_MAX;
    for (size_t i = 0; i < terminal_count; ++i) {
        kbus_module_t *m = &modules[i];
        memset(m, 0, sizeof(*m));
        m->position        = (uint16_t)(i + 1);
        m->type            = terminals[i];
        m->in_offset_bits  = info[i].OffsetInput_bits;
        m->in_size_bits    = info[i].SizeInput_bits;
        m->out_offset_bits = info[i].OffsetOutput_bits;
        m->out_size_bits   = info[i].SizeOutput_bits;
        m->channel_count   = info[i].AdditionalInfo.ChannelCount;
        m->pi_format       = info[i].AdditionalInfo.PiFormat;
    }

    {
        const uint32_t dig_in  = 8u * digital_offset_bytes(p, false, (uint32_t)status.BitCountAnalogInput);
        const uint32_t dig_out = 8u * digital_offset_bytes(p, true,  (uint32_t)status.BitCountAnalogOutput);
        const bool relative = digital_offsets_relative(p->cfg->digital_offset, modules,
                                                       terminal_count, dig_in, dig_out);
        LOG_INFO("digital area starts at byte in=%u out=%u; KBUS-info digital offsets are %s",
                 dig_in / 8u, dig_out / 8u, relative ? "relative (shifted)" : "absolute");
        if (relative) {
            for (size_t i = 0; i < terminal_count; ++i) {
                if (modules[i].type & 0x8000u) {
                    modules[i].in_offset_bits  += dig_in;
                    modules[i].out_offset_bits += dig_out;
                }
            }
        }
    }

    // GetIoSizes reports the full capacity of the process image (12000 bytes
    // on a PFC200). Exchange only the bytes the attached modules occupy.
    {
        uint32_t in_used = 0, out_used = 0;
        for (size_t i = 0; i < terminal_count; ++i) {
            const uint32_t ie = (modules[i].in_offset_bits + modules[i].in_size_bits + 7u) / 8u;
            const uint32_t oe = (modules[i].out_offset_bits + modules[i].out_size_bits + 7u) / 8u;
            if (modules[i].in_size_bits && ie > in_used)
                in_used = ie;
            if (modules[i].out_size_bits && oe > out_used)
                out_used = oe;
        }
        if (in_used > in_size || out_used > out_size) {
            LOG_ERROR("module data lies outside the process image (in %u/%u, out %u/%u bytes)",
                      in_used, in_size, out_used, out_size);
            goto out;
        }
        LOG_INFO("process image used: in %u of %u bytes, out %u of %u bytes",
                 in_used, in_size, out_used, out_size);
        *in_bytes  = in_used;
        *out_bytes = out_used;
    }
    *module_count = terminal_count;

    // "Running": from now on this application drives every KBUS cycle.
    rc = set_state(p, ApplicationState_Running, "Running");
out:
    p->info_destroy();
    return rc;
}

static int adi_exchange(kbus_backend_t *self, const uint8_t *out, size_t out_len,
                        uint8_t *in, size_t in_len)
{
    adi_priv_t *p = self->priv;
    int rc = 0;

    if (out_len > 0) {
        p->adi->WriteStart(p->dev, p->task_id);
        if (p->adi->WriteBytes(p->dev, p->task_id, 0, (uint32_t)out_len, (uint8_t *)out) != DAL_SUCCESS)
            rc = -1;
        p->adi->WriteEnd(p->dev, p->task_id);
    }

    int32_t push_ret = DAL_FAILURE;
    if (p->adi->CallDeviceSpecificFunction("libpackbus_Push", &push_ret) != DAL_SUCCESS ||
        push_ret != DAL_SUCCESS)
        return -1;

    if (in_len > 0) {
        p->adi->ReadStart(p->dev, p->task_id);
        if (p->adi->ReadBytes(p->dev, p->task_id, 0, (uint32_t)in_len, in) != DAL_SUCCESS)
            rc = -1;
        p->adi->ReadEnd(p->dev, p->task_id);
    }

    p->adi->WatchdogTrigger();
    return rc;
}

static void adi_close(kbus_backend_t *self)
{
    adi_priv_t *p = self->priv;
    if (p->adi != NULL) {
        if (p->dev_open)
            p->adi->CloseDevice(p->dev);
        if (p->adi_initialized)
            p->adi->Exit();
    }
    p->dev_open = 0;
    p->adi_initialized = 0;
}

kbus_backend_t *backend_adi_create(const gw_config_t *cfg)
{
    static adi_priv_t priv;
    static kbus_backend_t backend;
    memset(&priv, 0, sizeof(priv));
    priv.cfg = cfg;
    backend.name     = "adi";
    backend.open     = adi_open;
    backend.exchange = adi_exchange;
    backend.close    = adi_close;
    backend.priv     = &priv;
    return &backend;
}

#endif // HAVE_WAGO_ADI
