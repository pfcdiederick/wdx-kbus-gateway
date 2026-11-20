// wdx data layer: the parameters of the KBUS and the registers, served as a
// subset of the WAGO Device Access (WDA) REST-API, API version 1.11.0
// (Information/WDX/WDA_REST-API_Manual_v1_0.pdf, chapters 6, 7 and 9).
//
//   - JSON:API 1.0, content type application/vnd.api+json
//   - path  = hierarchical name, e.g. KBus/DigitalOutputs/3/Value
//   - id    = "0-0-" + lower-case path with '/' -> '-',
//             e.g. 0-0-kbus-digitaloutputs-3-value
//   - attributes: path, dataType, dataRank (scalar | array), value and, for
//     numeric types, stringValue
//
// Requests (manual chapter in brackets)
//   GET   /wda                                   service identity (7)
//   GET   /wda/parameters                        parameters, paged (9, 6)
//           page[offset] (0), page[limit] (255); links self/first/last/next/prev
//           filter[path|device|beta|deprecated|definition.writeable|definition.userSetting]
//           filter[id]=a,b,c   extension of this gateway: exactly these parameters
//           several filters: all must match; an unsupported filter gives 400
//   GET   /wda/parameters/{id}                   one parameter
//   PATCH /wda/parameters/{id}                   write one parameter          -> 204
//   PATCH /wda/parameters                        write several, all or nothing -> 204
//           body data[].attributes.value or .stringValue (numeric types)
//   GET   /wda/parameters/{id}/definition[/...]  301 -> /wda/parameter-definitions/{id}[/...]
//   GET   /wda/parameter-definitions             definitions, paged; filters path, device,
//                                                beta, deprecated, writeable, userSetting (and id)
//   GET   /wda/parameter-definitions/{id}        one definition
//
// Errors: {"errors":[{status, title, detail, code, source.pointer}]}, code = WDA status
// code (manual appendix B): 17 unknown id (404), 24 wrong type, 25 wrong representation,
// 31 read-only, 36 null, 39 invalid value / out of range (400), 41 not written because
// another value of the request was rejected, 57 / 58 writes disabled (403),
// 21 could not set (500). A batch PATCH reports one error object per parameter.
//
// Not implemented (deliberate, local gateway): HTTPS (426) and authentication, the
// device, method, enum, instance, monitoring-list, feature and File-API resources.
// KBus/Rescan and KBus/ResetStatistics are writable parameters, not methods.
//
// Parameter tree
//   KBus/Healthy, CycleCount, ErrorCount
//   KBus/CycleTime (ms)         PATCH 1..40 = new cycle time from the next cycle,
//                               stored (KBUS_SETTINGS_STORE), resets the timing statistics
//   KBus/ConfigurationVersion   +1 for every bus scan: the tree below changed
//   KBus/Rescan                 PATCH true = rescan the bus now (reads false)
//   KBus/CycleDuration, MaxCycleDuration, MaxWakeupDelay (us), Overruns:
//                               timing of the cycle thread; KBus/ResetStatistics
//                               PATCH true = CycleCount, ErrorCount, maximum values and
//                               overruns back to 0 (control socket only)
//   KBus/LocalControl           true = only requests through the control socket (the
//                               dashboard, after its login) may write the KBUS; other
//                               clients get 403 (code 57) for KBUS writes, reading and
//                               registers stay free. Switched only through the control
//                               socket; stored in KBUS_SETTINGS_STORE
//   KBus/Modules                 instantiations [1..n]
//   KBus/Modules/{m}/Name, TypeCode, ChannelCount
//   KBus/DigitalInputs|DigitalOutputs|AnalogInputs|AnalogOutputs   instantiations
//   KBus/<Class>/{n}/Value (DO/AO writable), Module, ModuleChannel
//   KBus/DigitalInputValues|DigitalOutputValues|AnalogInputValues|AnalogOutputValues
//                                 all values of a class as one array (DO/AO writable)
//   Registers/ChangeCounter       +1 for every write that changed a register
//   Registers/<Name>/Value        declared in REGISTER_FILE (registers.c), writable
//
// Thread safety: wda_handle() runs in several server threads at once; the
// tables are only rebuilt by wda_init() while no request runs (http_server.c).
#include "wda.h"
#include "gateway.h"
#include "log.h"
#include "registers.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef GW_VERSION
#define GW_VERSION "unknown"
#endif
#define WDA_API_VERSION   "1.11.0"   // version of the WDA REST-API this subset follows
#define PAGE_LIMIT_DEFAULT 255
#define REQ_MAX           16384      // largest request (RX_MAX in http_server.c)

// WDA status codes (manual appendix B)
enum {
    WC_NONE = 0, WC_INTERNAL = 2, WC_UNKNOWN_PATH = 17, WC_COULD_NOT_SET = 21,
    WC_WRONG_TYPE = 24, WC_WRONG_REPR = 25, WC_NOT_WRITEABLE = 31, WC_VALUE_NULL = 36,
    WC_INVALID_VALUE = 39, WC_OTHER_INVALID = 41, WC_UNAUTHORIZED = 57, WC_OTHER_UNAUTH = 58
};

// ---------------------------------------------------------------------------
// Output buffer
// ---------------------------------------------------------------------------
void wbuf_free(wbuf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void wb_printf(wbuf_t *b, const char *fmt, ...)
{
    for (;;) {
        const size_t room = b->cap - b->len;
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(b->data ? b->data + b->len : NULL, b->data ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->data && (size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + (size_t)n + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (p == NULL)
            return;
        b->data = p;
        b->cap = cap;
    }
}

static void wb_json_string(wbuf_t *b, const char *s)
{
    wb_printf(b, "\"");
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            wb_printf(b, "\\%c", c);
        else if (c < 0x20)
            wb_printf(b, "\\u%04x", c);
        else
            wb_printf(b, "%c", c);
    }
    wb_printf(b, "\"");
}

static const char *title_of(int status)
{
    switch (status) {
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

// One error object (without the surrounding array).
static void error_object(wbuf_t *b, int status, int code, const char *pointer, const char *detail)
{
    wb_printf(b, "{\"status\":\"%d\",\"title\":", status);
    wb_json_string(b, title_of(status));
    wb_printf(b, ",\"detail\":");
    wb_json_string(b, detail);
    if (code != WC_NONE)
        wb_printf(b, ",\"code\":\"%d\"", code);
    if (pointer != NULL && *pointer)
        wb_printf(b, ",\"source\":{\"pointer\":\"%s\"}", pointer);
    wb_printf(b, "}");
}

static int error(wbuf_t *b, int status, int code, const char *pointer, const char *detail)
{
    b->len = 0;
    wb_printf(b, "{\"errors\":[");
    error_object(b, status, code, pointer, detail);
    wb_printf(b, "]}");
    return status;
}

// ---------------------------------------------------------------------------
// Parameter list
// ---------------------------------------------------------------------------
typedef enum { DT_BOOL, DT_UINT16, DT_INT32, DT_UINT32, DT_UINT64, DT_FLOAT64, DT_STRING, DT_INST } dtype_t;

typedef enum {
    P_HEALTHY, P_CYCLETIME, P_CYCLES, P_ERRORS, P_CONFIGVER, P_RESCAN, P_MODULES,
    P_MOD_NAME, P_MOD_TYPE, P_MOD_CHCOUNT,
    P_CLS_INST, P_CH_VALUE, P_CH_MODULE, P_CH_MODCH, P_CLS_VALUES,
    P_REG_CHANGES, P_REG_VALUE,
    P_CYCLEDUR, P_MAXCYCLEDUR, P_MAXLATE, P_OVERRUNS, P_RESETSTATS, P_LOCALCTRL
} pkind_t;

typedef struct {
    uint8_t  kind;    // pkind_t
    uint8_t  cls;     // io_kind_t for class parameters
    uint16_t idx;     // module or channel number (1-based), register index (0-based)
} param_t;

#define MAX_PARAMS (14 + KBUS_MAX_MODULES * 3 + IO_KIND_COUNT * (2 + GW_MAX_CHANNELS * 3) + 1 + REG_MAX)

#define ID_MAX 64                    // longest: 0-0-registers-<40 characters>-value

static param_t  plist[MAX_PARAMS];
static char     pids[MAX_PARAMS][ID_MAX];   // ids precomputed once: lookups stay cheap
static uint8_t  pidlen[MAX_PARAMS];
static size_t   pcount;
static uint16_t mod_channels[KBUS_MAX_MODULES];
static uint32_t built_version;          // configuration the tables belong to (0 = none)
static int      local_control;          // KBus/LocalControl (__atomic: several threads)

static const char *const cls_name[IO_KIND_COUNT]   = { "DigitalInputs", "DigitalOutputs",
                                                       "AnalogInputs", "AnalogOutputs" };
static const char *const cls_values[IO_KIND_COUNT] = { "DigitalInputValues", "DigitalOutputValues",
                                                       "AnalogInputValues", "AnalogOutputValues" };

static void add(pkind_t k, io_kind_t cls, size_t idx)
{
    if (pcount < MAX_PARAMS)
        plist[pcount++] = (param_t){ (uint8_t)k, (uint8_t)cls, (uint16_t)idx };
}

// The module list and channel map only change with a bus scan, so the
// parameter list is built once per configuration (see wda_init()).
static void build_list(void)
{
    pcount = 0;
    size_t nmod = 0;
    gateway_modules(&nmod);
    memset(mod_channels, 0, sizeof(mod_channels));
    for (int k = 0; k < IO_KIND_COUNT; ++k)
        for (size_t c = 1; c <= gateway_channel_count((io_kind_t)k); ++c) {
            const io_channel_t *ch = gateway_channel((io_kind_t)k, c);
            if (ch && ch->module_index < KBUS_MAX_MODULES)
                mod_channels[ch->module_index]++;
        }

    add(P_HEALTHY, 0, 0);
    add(P_CYCLETIME, 0, 0);
    add(P_CYCLES, 0, 0);
    add(P_ERRORS, 0, 0);
    add(P_CONFIGVER, 0, 0);
    add(P_RESCAN, 0, 0);
    add(P_CYCLEDUR, 0, 0);
    add(P_MAXCYCLEDUR, 0, 0);
    add(P_MAXLATE, 0, 0);
    add(P_OVERRUNS, 0, 0);
    add(P_RESETSTATS, 0, 0);
    add(P_LOCALCTRL, 0, 0);
    add(P_MODULES, 0, 0);
    for (size_t m = 1; m <= nmod; ++m) {
        add(P_MOD_NAME, 0, m);
        add(P_MOD_TYPE, 0, m);
        add(P_MOD_CHCOUNT, 0, m);
    }
    for (int k = 0; k < IO_KIND_COUNT; ++k) {
        add(P_CLS_INST, (io_kind_t)k, 0);
        for (size_t c = 1; c <= gateway_channel_count((io_kind_t)k); ++c) {
            add(P_CH_VALUE, (io_kind_t)k, c);
            add(P_CH_MODULE, (io_kind_t)k, c);
            add(P_CH_MODCH, (io_kind_t)k, c);
        }
        add(P_CLS_VALUES, (io_kind_t)k, 0);
    }
    add(P_REG_CHANGES, 0, 0);
    for (size_t r = 0; r < registers_count(); ++r)
        add(P_REG_VALUE, 0, r);
}

static void id_of(const param_t *p, char *buf, size_t len);

static void build_ids(void)
{
    for (size_t i = 0; i < pcount; ++i) {
        id_of(&plist[i], pids[i], ID_MAX);
        pidlen[i] = (uint8_t)strlen(pids[i]);
    }
}

static int is_digital(io_kind_t k) { return k == IO_DI || k == IO_DO; }

static void path_of(const param_t *p, char *buf, size_t len)
{
    switch ((pkind_t)p->kind) {
    case P_HEALTHY:     snprintf(buf, len, "KBus/Healthy"); break;
    case P_CYCLETIME:   snprintf(buf, len, "KBus/CycleTime"); break;
    case P_CYCLES:      snprintf(buf, len, "KBus/CycleCount"); break;
    case P_ERRORS:      snprintf(buf, len, "KBus/ErrorCount"); break;
    case P_CONFIGVER:   snprintf(buf, len, "KBus/ConfigurationVersion"); break;
    case P_RESCAN:      snprintf(buf, len, "KBus/Rescan"); break;
    case P_CYCLEDUR:    snprintf(buf, len, "KBus/CycleDuration"); break;
    case P_MAXCYCLEDUR: snprintf(buf, len, "KBus/MaxCycleDuration"); break;
    case P_MAXLATE:     snprintf(buf, len, "KBus/MaxWakeupDelay"); break;
    case P_OVERRUNS:    snprintf(buf, len, "KBus/Overruns"); break;
    case P_RESETSTATS:  snprintf(buf, len, "KBus/ResetStatistics"); break;
    case P_LOCALCTRL:   snprintf(buf, len, "KBus/LocalControl"); break;
    case P_MODULES:     snprintf(buf, len, "KBus/Modules"); break;
    case P_MOD_NAME:    snprintf(buf, len, "KBus/Modules/%u/Name", p->idx); break;
    case P_MOD_TYPE:    snprintf(buf, len, "KBus/Modules/%u/TypeCode", p->idx); break;
    case P_MOD_CHCOUNT: snprintf(buf, len, "KBus/Modules/%u/ChannelCount", p->idx); break;
    case P_CLS_INST:    snprintf(buf, len, "KBus/%s", cls_name[p->cls]); break;
    case P_CH_VALUE:    snprintf(buf, len, "KBus/%s/%u/Value", cls_name[p->cls], p->idx); break;
    case P_CH_MODULE:   snprintf(buf, len, "KBus/%s/%u/Module", cls_name[p->cls], p->idx); break;
    case P_CH_MODCH:    snprintf(buf, len, "KBus/%s/%u/ModuleChannel", cls_name[p->cls], p->idx); break;
    case P_CLS_VALUES:  snprintf(buf, len, "KBus/%s", cls_values[p->cls]); break;
    case P_REG_CHANGES: snprintf(buf, len, "Registers/ChangeCounter"); break;
    case P_REG_VALUE:   snprintf(buf, len, "Registers/%s/Value", registers_def(p->idx)->name); break;
    }
}

// id = "0-0-" + lower-case path, '/' -> '-'
static void id_of(const param_t *p, char *buf, size_t len)
{
    char path[96];
    path_of(p, path, sizeof(path));
    size_t o = (size_t)snprintf(buf, len, "0-0-");
    for (const char *s = path; *s && o + 1 < len; ++s)
        buf[o++] = (*s == '/') ? '-' : (char)tolower((unsigned char)*s);
    buf[o < len ? o : len - 1] = '\0';
}

static dtype_t dtype_of(const param_t *p)
{
    switch ((pkind_t)p->kind) {
    case P_HEALTHY:
    case P_RESETSTATS:
    case P_LOCALCTRL:
    case P_RESCAN:    return DT_BOOL;
    case P_CYCLEDUR:
    case P_MAXCYCLEDUR:
    case P_MAXLATE:
    case P_CONFIGVER: return DT_UINT32;
    case P_OVERRUNS:  return DT_UINT64;
    case P_CYCLES:
    case P_ERRORS:
    case P_REG_CHANGES: return DT_UINT64;
    case P_REG_VALUE:
        switch (registers_def(p->idx)->type) {
        case REG_BOOL:    return DT_BOOL;
        case REG_INT32:   return DT_INT32;
        case REG_UINT32:  return DT_UINT32;
        case REG_FLOAT64: return DT_FLOAT64;
        case REG_STRING:  return DT_STRING;
        }
        return DT_STRING;
    case P_MODULES:
    case P_CLS_INST:  return DT_INST;
    case P_MOD_NAME:  return DT_STRING;
    case P_CH_VALUE:
    case P_CLS_VALUES: return is_digital((io_kind_t)p->cls) ? DT_BOOL : DT_UINT16;
    default:          return DT_UINT16;
    }
}

// Types with a string representation (manual chapter 6, Data types).
static int is_numeric(dtype_t t)
{
    return t == DT_UINT16 || t == DT_INT32 || t == DT_UINT32 || t == DT_UINT64 || t == DT_FLOAT64;
}

static int is_array(const param_t *p)  { return p->kind == P_CLS_VALUES; }
static int writeable(const param_t *p)
{
    if (p->kind == P_RESCAN || p->kind == P_RESETSTATS || p->kind == P_CYCLETIME || p->kind == P_REG_VALUE ||
        p->kind == P_LOCALCTRL)
        return 1;
    return (p->kind == P_CH_VALUE || p->kind == P_CLS_VALUES) && io_kind_writable((io_kind_t)p->cls);
}

// userSetting: a setting that the gateway keeps over a restart
static int user_setting(const param_t *p, const gw_config_t *cfg)
{
    return (p->kind == P_REG_VALUE && registers_def(p->idx)->persistent) ||
           ((p->kind == P_CYCLETIME || p->kind == P_LOCALCTRL) && cfg->settings_store && *cfg->settings_store);
}

static const char *dtype_name(dtype_t t)
{
    switch (t) {
    case DT_BOOL:   return "boolean";
    case DT_UINT16: return "uint16";
    case DT_INT32:  return "int32";
    case DT_UINT32: return "uint32";
    case DT_UINT64: return "uint64";
    case DT_FLOAT64: return "float64";
    case DT_STRING: return "string";
    case DT_INST:   return "instantiations";
    }
    return "?";
}

static const param_t *find_id(const char *id, size_t len)
{
    for (size_t i = 0; i < pcount; ++i)
        if (pidlen[i] == len && memcmp(pids[i], id, len) == 0)
            return &plist[i];
    return NULL;
}

static size_t index_of(const param_t *p) { return (size_t)(p - plist); }

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
// One consistent view for a GET (allocated per request: several threads).
typedef struct {
    uint16_t    snap[IO_KIND_COUNT][GW_MAX_CHANNELS];
    gw_stats_t  stats;
    uint64_t    reg_changes;
    reg_value_t reg[REG_MAX];
} values_t;

static values_t *values_take(void)
{
    values_t *v = malloc(sizeof(*v));
    if (v == NULL)
        return NULL;
    gateway_snapshot(v->snap);
    gateway_stats(&v->stats);
    registers_snapshot(v->reg, &v->reg_changes);
    return v;
}

static void write_value(wbuf_t *b, const param_t *p, const values_t *v)
{
    size_t nmod = 0;
    const kbus_module_t *mods = gateway_modules(&nmod);
    const io_kind_t k = (io_kind_t)p->cls;
    switch ((pkind_t)p->kind) {
    case P_HEALTHY:   wb_printf(b, v->stats.healthy ? "true" : "false"); break;
    case P_CYCLETIME: wb_printf(b, "%d", v->stats.cycle_ms); break;
    case P_CYCLES:    wb_printf(b, "%llu", (unsigned long long)v->stats.cycles); break;
    case P_ERRORS:    wb_printf(b, "%llu", (unsigned long long)v->stats.errors); break;
    case P_CONFIGVER: wb_printf(b, "%u", (unsigned)gateway_config_version()); break;
    case P_RESCAN:
    case P_RESETSTATS: wb_printf(b, "false"); break;
    case P_LOCALCTRL:  wb_printf(b, __atomic_load_n(&local_control, __ATOMIC_RELAXED) ? "true" : "false"); break;
    case P_CYCLEDUR:    wb_printf(b, "%u", (unsigned)v->stats.last_cycle_us); break;
    case P_MAXCYCLEDUR: wb_printf(b, "%u", (unsigned)v->stats.max_cycle_us); break;
    case P_MAXLATE:     wb_printf(b, "%u", (unsigned)v->stats.max_late_us); break;
    case P_OVERRUNS:    wb_printf(b, "%llu", (unsigned long long)v->stats.overruns); break;
    case P_MODULES:
        wb_printf(b, "[");
        for (size_t m = 1; m <= nmod; ++m)
            wb_printf(b, m > 1 ? ",%zu" : "%zu", m);
        wb_printf(b, "]");
        break;
    case P_MOD_NAME: {
        char name[48] = "";
        if (p->idx >= 1 && p->idx <= nmod)
            gateway_module_name(&mods[p->idx - 1], name, sizeof(name));
        wb_json_string(b, name);
        break;
    }
    case P_MOD_TYPE:
        wb_printf(b, "%u", (p->idx >= 1 && p->idx <= nmod) ? mods[p->idx - 1].type : 0u);
        break;
    case P_MOD_CHCOUNT:
        wb_printf(b, "%u", (p->idx >= 1 && p->idx <= KBUS_MAX_MODULES) ? mod_channels[p->idx - 1] : 0u);
        break;
    case P_CLS_INST:
        wb_printf(b, "[");
        for (size_t c = 1; c <= gateway_channel_count(k); ++c)
            wb_printf(b, c > 1 ? ",%zu" : "%zu", c);
        wb_printf(b, "]");
        break;
    case P_CH_VALUE:
        if (is_digital(k))
            wb_printf(b, v->snap[k][p->idx - 1] ? "true" : "false");
        else
            wb_printf(b, "%u", v->snap[k][p->idx - 1]);
        break;
    case P_CH_MODULE: {
        const io_channel_t *ch = gateway_channel(k, p->idx);
        wb_printf(b, "%u", ch ? ch->module_index + 1u : 0u);
        break;
    }
    case P_CH_MODCH: {
        const io_channel_t *ch = gateway_channel(k, p->idx);
        wb_printf(b, "%u", ch ? ch->module_channel : 0u);
        break;
    }
    case P_CLS_VALUES:
        wb_printf(b, "[");
        for (size_t c = 0; c < gateway_channel_count(k); ++c) {
            if (c)
                wb_printf(b, ",");
            if (is_digital(k))
                wb_printf(b, v->snap[k][c] ? "true" : "false");
            else
                wb_printf(b, "%u", v->snap[k][c]);
        }
        wb_printf(b, "]");
        break;
    case P_REG_CHANGES:
        wb_printf(b, "%llu", (unsigned long long)v->reg_changes);
        break;
    case P_REG_VALUE: {
        char json[REG_JSON_MAX];
        registers_format(registers_def(p->idx)->type, &v->reg[p->idx], json, sizeof(json));
        wb_printf(b, "%s", json);
        break;
    }
    }
}

// stringValue of a numeric value that was just written to b[vs..ve): the same
// literals in double quotes ("123", or ["1","2"] for an array).
static void write_string_value(wbuf_t *b, size_t vs, size_t ve)
{
    char lit[40];
    size_t i = vs;
    const int arr = b->data[i] == '[';
    if (arr) {
        wb_printf(b, "[");
        i++;
        ve--;                                  // without the closing ']'
    }
    int first = 1;
    while (i < ve) {
        size_t n = 0;
        while (i < ve && b->data[i] != ',' && n + 1 < sizeof(lit))
            lit[n++] = b->data[i++];
        lit[n] = '\0';
        if (i < ve && b->data[i] == ',')
            i++;
        wb_printf(b, first ? "\"%s\"" : ",\"%s\"", lit);   // lit is a copy: b may move
        first = 0;
    }
    if (arr)
        wb_printf(b, "]");
}

static void write_resource(wbuf_t *b, const param_t *p, const values_t *v)
{
    char path[96];
    path_of(p, path, sizeof(path));
    const dtype_t t = dtype_of(p);
    wb_printf(b, "{\"type\":\"parameters\",\"id\":\"%s\",\"attributes\":{\"path\":\"%s\","
                 "\"dataType\":\"%s\",\"dataRank\":\"%s\",\"value\":",
              pids[index_of(p)], path, dtype_name(t), is_array(p) ? "array" : "scalar");
    const size_t vs = b->len;
    write_value(b, p, v);
    const size_t ve = b->len;
    if (is_numeric(t) && ve > vs) {
        wb_printf(b, ",\"stringValue\":");
        write_string_value(b, vs, ve);
    }
    wb_printf(b, "}}");
}

static void write_definition(wbuf_t *b, const param_t *p, const gw_config_t *cfg)
{
    char path[96];
    path_of(p, path, sizeof(path));
    wb_printf(b, "{\"type\":\"parameter-definitions\",\"id\":\"%s\",\"attributes\":{"
                 "\"path\":\"%s\",\"dataType\":\"%s\",\"dataRank\":\"%s\",\"writeable\":%s,"
                 "\"userSetting\":%s,\"beta\":false,\"deprecated\":false}}",
              pids[index_of(p)], path, dtype_name(dtype_of(p)), is_array(p) ? "array" : "scalar",
              writeable(p) ? "true" : "false", user_setting(p, cfg) ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Minimal JSON reader (enough for a JSON:API PATCH body)
// ---------------------------------------------------------------------------
typedef struct { const char *s; size_t n, i; } jr_t;

static void jr_ws(jr_t *j) { while (j->i < j->n && isspace((unsigned char)j->s[j->i])) j->i++; }

static int jr_string(jr_t *j, const char **str, size_t *slen)
{
    jr_ws(j);
    if (j->i >= j->n || j->s[j->i] != '"')
        return -1;
    const size_t st = ++j->i;
    while (j->i < j->n && j->s[j->i] != '"') {
        if (j->s[j->i] == '\\')
            j->i++;
        j->i++;
    }
    if (j->i >= j->n)
        return -1;
    *str = j->s + st;
    *slen = j->i - st;
    j->i++;
    return 0;
}

static int jr_skip(jr_t *j, int depth)
{
    jr_ws(j);
    if (j->i >= j->n || depth > 32)
        return -1;
    const char c = j->s[j->i];
    if (c == '"') {
        const char *s; size_t l;
        return jr_string(j, &s, &l);
    }
    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        j->i++;
        jr_ws(j);
        if (j->i < j->n && j->s[j->i] == close) { j->i++; return 0; }
        for (;;) {
            if (c == '{') {
                const char *s; size_t l;
                if (jr_string(j, &s, &l) != 0) return -1;
                jr_ws(j);
                if (j->i >= j->n || j->s[j->i] != ':') return -1;
                j->i++;
            }
            if (jr_skip(j, depth + 1) != 0) return -1;
            jr_ws(j);
            if (j->i >= j->n) return -1;
            if (j->s[j->i] == ',') { j->i++; continue; }
            if (j->s[j->i] == close) { j->i++; return 0; }
            return -1;
        }
    }
    // number / true / false / null
    const size_t st = j->i;
    while (j->i < j->n && (isalnum((unsigned char)j->s[j->i]) || strchr("+-.", j->s[j->i])))
        j->i++;
    return j->i > st ? 0 : -1;
}

// Find the value at key path keys[0..nkeys-1]; returns its raw span.
static int jr_get(const char *s, size_t n, const char *const *keys, int nkeys,
                  const char **val, size_t *vlen)
{
    jr_t j = { s, n, 0 };
    for (int d = 0; d < nkeys; ++d) {
        jr_ws(&j);
        if (j.i >= j.n || j.s[j.i] != '{')
            return -1;
        j.i++;
        int found = 0;
        for (;;) {
            const char *k; size_t kl;
            jr_ws(&j);
            if (j.i < j.n && j.s[j.i] == '}')
                return -1;
            if (jr_string(&j, &k, &kl) != 0)
                return -1;
            jr_ws(&j);
            if (j.i >= j.n || j.s[j.i] != ':')
                return -1;
            j.i++;
            if (kl == strlen(keys[d]) && memcmp(k, keys[d], kl) == 0) {
                found = 1;
                break;
            }
            if (jr_skip(&j, 0) != 0)
                return -1;
            jr_ws(&j);
            if (j.i < j.n && j.s[j.i] == ',') { j.i++; continue; }
            return -1;
        }
        if (!found)
            return -1;
    }
    jr_ws(&j);
    const size_t st = j.i;
    if (jr_skip(&j, 0) != 0)
        return -1;
    *val = s + st;
    *vlen = j.i - st;
    return 0;
}

static int span_eq(const char *s, size_t n, const char *lit) { return n == strlen(lit) && memcmp(s, lit, n) == 0; }

// One scalar: boolean (true/false) or uint16 (0..65535).
// Returns 0 or a WDA status code (24 wrong type, 36 null, 39 out of range).
static int parse_scalar(const char *s, size_t n, int digital, int32_t *out)
{
    if (span_eq(s, n, "null"))
        return WC_VALUE_NULL;
    if (digital) {
        if (span_eq(s, n, "true"))  { *out = 1; return 0; }
        if (span_eq(s, n, "false")) { *out = 0; return 0; }
        return WC_WRONG_TYPE;
    }
    size_t i = 0;
    const int neg = n > 0 && s[0] == '-';
    if (neg)
        i = 1;
    if (i >= n)
        return WC_WRONG_TYPE;
    int64_t v = 0;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return WC_WRONG_TYPE;                      // text, fraction, exponent
        if (v <= 65535)
            v = v * 10 + (s[i] - '0');
    }
    if ((neg && v != 0) || v > 65535)
        return WC_INVALID_VALUE;
    *out = (int32_t)v;
    return 0;
}

// ---------------------------------------------------------------------------
// Request helpers
// ---------------------------------------------------------------------------
// Value of request header `name` (without the colon), or NULL.
static const char *header_value(const char *req, size_t len, const char *name, size_t *vlen)
{
    const size_t nl = strlen(name);
    for (size_t i = 0; i + 1 < len; ++i) {
        if (req[i] != '\r' || req[i + 1] != '\n')
            continue;
        const char *h = req + i + 2;
        const size_t rest = len - (i + 2);
        if (rest >= 2 && h[0] == '\r' && h[1] == '\n')
            return NULL;                                   // end of headers
        if (rest > nl && strncasecmp(h, name, nl) == 0 && h[nl] == ':') {
            size_t s = nl + 1, e;
            while (s < rest && h[s] == ' ')
                s++;
            e = s;
            while (e < rest && h[e] != '\r' && h[e] != '\n')
                e++;
            *vlen = e - s;
            return h + s;
        }
    }
    return NULL;
}

static int span_contains(const char *s, size_t n, const char *needle)
{
    const size_t l = strlen(needle);
    for (size_t p = 0; p + l <= n; ++p)
        if (strncasecmp(s + p, needle, l) == 0)
            return 1;
    return 0;
}

static int has_jsonapi_content_type(const char *req, size_t len)
{
    size_t vl = 0;
    const char *v = header_value(req, len, "content-type", &vl);
    return v != NULL && span_contains(v, vl, "application/vnd.api+json");
}

// No Accept header, or one that allows JSON:API (406 otherwise).
static int accepts_jsonapi(const char *req, size_t len)
{
    size_t vl = 0;
    const char *v = header_value(req, len, "accept", &vl);
    return v == NULL || vl == 0 || span_contains(v, vl, "application/vnd.api+json") ||
           span_contains(v, vl, "*/*") || span_contains(v, vl, "application/*");
}

static const char *body_of(const char *req, size_t len, size_t *blen)
{
    for (size_t i = 0; i + 3 < len; ++i)
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            *blen = len - (i + 4);
            return req + i + 4;
        }
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decode s[0..n) into out (NUL terminated). Returns the length.
static size_t pct_decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out[o++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return o;
}

// Is s one of the comma-separated values in list?
static int list_has(const char *list, size_t ll, const char *s, size_t sl, int icase)
{
    size_t p = 0;
    while (p <= ll) {
        size_t e = p;
        while (e < ll && list[e] != ',')
            e++;
        if (e - p == sl && (icase ? strncasecmp(list + p, s, sl) : memcmp(list + p, s, sl)) == 0)
            return 1;
        p = e + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Query string: paging and filters (manual chapter 6)
// ---------------------------------------------------------------------------
enum { F_ID, F_PATH, F_DEVICE, F_BETA, F_DEPRECATED, F_WRITEABLE, F_USERSETTING, F_COUNT };

static const char *const filter_names_param[F_COUNT] = {
    "id", "path", "device", "beta", "deprecated", "definition.writeable", "definition.userSetting" };
static const char *const filter_names_def[F_COUNT] = {
    "id", "path", "device", "beta", "deprecated", "writeable", "userSetting" };

typedef struct {
    long long   offset, limit;
    int         set[F_COUNT];
    const char *val[F_COUNT];       // decoded, in dec[]
    size_t      len[F_COUNT];
    int         flag[F_COUNT];      // boolean filters: the requested value
    char        dec[REQ_MAX];
    size_t      decn;
} query_t;

static int parse_bool(const char *s, size_t n, int *out)
{
    if (n == 4 && strncasecmp(s, "true", 4) == 0)  { *out = 1; return 0; }
    if (n == 5 && strncasecmp(s, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

static int parse_page(const char *s, size_t n, long long *out)
{
    if (n == 0 || n > 10)
        return -1;
    long long v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 2147483647LL)
        return -1;
    *out = v;
    return 0;
}

// Returns 0 or an HTTP status (error body set).
static int parse_query(query_t *q, const char *s, size_t n, int defs, wbuf_t *b)
{
    const char *const *names = defs ? filter_names_def : filter_names_param;
    q->offset = 0;
    q->limit = PAGE_LIMIT_DEFAULT;
    memset(q->set, 0, sizeof(q->set));
    q->decn = 0;
    size_t p = 0;
    while (p < n) {
        size_t e = p;
        while (e < n && s[e] != '&')
            e++;
        size_t eq = p;
        while (eq < e && s[eq] != '=')
            eq++;
        char key[64];
        pct_decode(s + p, eq - p, key, sizeof(key));
        const char *raw = eq < e ? s + eq + 1 : s + e;
        const size_t rawlen = eq < e ? e - eq - 1 : 0;
        char *val = q->dec + q->decn;
        const size_t vlen = pct_decode(raw, rawlen, val, sizeof(q->dec) - q->decn);
        q->decn += vlen + 1;
        char detail[128];

        if (strcmp(key, "page[offset]") == 0 || strcmp(key, "page[limit]") == 0) {
            long long *dst = key[5] == 'o' ? &q->offset : &q->limit;
            if (parse_page(val, vlen, dst) != 0) {
                snprintf(detail, sizeof(detail), "%s must be an integer 0..2147483647", key);
                return error(b, 400, WC_NONE, NULL, detail);
            }
        } else if (strncmp(key, "filter[", 7) == 0) {
            const size_t kl = strlen(key);
            int f = -1;
            if (key[kl - 1] == ']')
                for (int i = 0; i < F_COUNT; ++i)
                    if (kl - 8 == strlen(names[i]) && strncmp(key + 7, names[i], kl - 8) == 0)
                        f = i;
            if (f < 0) {
                snprintf(detail, sizeof(detail), "filter %.60s is not supported by this endpoint", key);
                return error(b, 400, WC_NONE, NULL, detail);
            }
            if (f >= F_BETA && parse_bool(val, vlen, &q->flag[f]) != 0) {
                snprintf(detail, sizeof(detail), "%.60s must be true or false", key);
                return error(b, 400, WC_NONE, NULL, detail);
            }
            q->set[f] = 1;
            q->val[f] = val;
            q->len[f] = vlen;
        }
        // other query parameters (e.g. parameter-errors-as-data-attributes) change
        // nothing here: no value of this gateway can fail on its own
        p = e + 1;
    }
    return 0;
}

static int matches(const query_t *q, size_t i, const gw_config_t *cfg)
{
    const param_t *p = &plist[i];
    if (q->set[F_ID] && !list_has(q->val[F_ID], q->len[F_ID], pids[i], pidlen[i], 0))
        return 0;
    if (q->set[F_PATH]) {
        char path[96];
        path_of(p, path, sizeof(path));
        if (!list_has(q->val[F_PATH], q->len[F_PATH], path, strlen(path), 1))
            return 0;
    }
    if (q->set[F_DEVICE] && !list_has(q->val[F_DEVICE], q->len[F_DEVICE], "0-0", 3, 1) &&
        !list_has(q->val[F_DEVICE], q->len[F_DEVICE], "headstation", 11, 1))
        return 0;                                          // everything belongs to the head station
    if ((q->set[F_BETA] && q->flag[F_BETA]) || (q->set[F_DEPRECATED] && q->flag[F_DEPRECATED]))
        return 0;                                          // nothing is beta or deprecated
    if (q->set[F_WRITEABLE] && q->flag[F_WRITEABLE] != writeable(p))
        return 0;
    if (q->set[F_USERSETTING] && q->flag[F_USERSETTING] != user_setting(p, cfg))
        return 0;
    return 1;
}

// One paging link: base?<the other query parameters>&page[offset]=o&page[limit]=l
static void write_link(wbuf_t *b, const char *name, const char *base, const char *qs, size_t qn,
                       long long offset, long long limit)
{
    wbuf_t l = { NULL, 0, 0, "" };
    wb_printf(&l, "%s?", base);
    size_t p = 0;
    while (p < qn) {
        size_t e = p;
        while (e < qn && qs[e] != '&')
            e++;
        size_t eq = p;
        while (eq < e && qs[eq] != '=')
            eq++;
        char key[64];
        pct_decode(qs + p, eq - p, key, sizeof(key));
        if (e > p && strncmp(key, "page[", 5) != 0)
            wb_printf(&l, "%.*s&", (int)(e - p), qs + p);
        p = e + 1;
    }
    wb_printf(&l, "page[offset]=%lld&page[limit]=%lld", offset, limit);
    wb_printf(b, "\"%s\":", name);
    wb_json_string(b, l.data ? l.data : "");
    wbuf_free(&l);
}

// GET on a collection: /wda/parameters or /wda/parameter-definitions
static int do_collection(const char *base, const char *qs, size_t qn, int defs,
                         const gw_config_t *cfg, wbuf_t *b)
{
    query_t *q = malloc(sizeof(*q));
    if (q == NULL)
        return error(b, 500, WC_INTERNAL, NULL, "out of memory");
    int st = parse_query(q, qs, qn, defs, b);
    if (st) {
        free(q);
        return st;
    }
    values_t *v = NULL;
    if (!defs && (v = values_take()) == NULL) {
        free(q);
        return error(b, 500, WC_INTERNAL, NULL, "out of memory");
    }
    long long total = 0, count = 0;
    wb_printf(b, "{\"data\":[");
    for (size_t i = 0; i < pcount; ++i) {
        if (!matches(q, i, cfg))
            continue;
        if (total >= q->offset && total - q->offset < q->limit) {
            if (count++)
                wb_printf(b, ",");
            if (defs)
                write_definition(b, &plist[i], cfg);
            else
                write_resource(b, &plist[i], v);
        }
        total++;
    }
    const long long lim = q->limit, off = q->offset;
    const long long last = (lim > 0 && total > 0) ? ((total - 1) / lim) * lim : 0;
    wb_printf(b, "],\"links\":{");
    write_link(b, "self", base, qs, qn, off, lim);
    wb_printf(b, ",");
    write_link(b, "first", base, qs, qn, 0, lim);
    wb_printf(b, ",");
    write_link(b, "last", base, qs, qn, last, lim);
    if (lim > 0 && off + lim < total) {
        wb_printf(b, ",");
        write_link(b, "next", base, qs, qn, off + lim, lim);
    }
    if (off > 0) {
        wb_printf(b, ",");
        write_link(b, "prev", base, qs, qn, off > lim ? off - lim : 0, lim);
    }
    wb_printf(b, "},\"meta\":{\"total\":%lld,\"offset\":%lld,\"count\":%lld}}", total, off, count);
    free(v);
    free(q);
    return 200;
}

static int do_service_identity(wbuf_t *b)
{
    wb_printf(b, "{\"data\":{\"type\":\"service-identity\",\"id\":\"wda\",\"attributes\":{"
                 "\"serviceName\":\"kbus-gateway\",\"serviceVersion\":\"%s\",\"apiVersion\":\"%s\","
                 "\"coreVersion\":\"%s\"},\"relationships\":{"
                 "\"parameters\":{\"links\":{\"related\":\"/wda/parameters\"}},"
                 "\"parameter-definitions\":{\"links\":{\"related\":\"/wda/parameter-definitions\"}}}}}",
              GW_VERSION, WDA_API_VERSION, GW_VERSION);
    return 200;
}

// ---------------------------------------------------------------------------
// Writes. Every value of a request is checked and staged first; commit() then
// writes all of them: the KBUS outputs under one lock (same bus cycle), the
// registers under one lock. A rejected request writes nothing.
// ---------------------------------------------------------------------------
#define LOG_ITEMS 32
#define MAX_ITEMS 512                  // parameters in one batch (a 16 KB body holds ~230)

typedef struct { int status, code; char detail[200]; } verr_t;

typedef struct {
    int32_t     kb[IO_KIND_COUNT][GW_MAX_CHANNELS];   // -1 = unchanged
    size_t      kbn[IO_KIND_COUNT];
    int         rescan;
    int         reset_stats;
    int         cycle_ms;                                  // 0 = unchanged
    int         local_ctrl;                                // -1 = unchanged
    size_t      ridx[REG_MAX];
    reg_value_t rval[REG_MAX];
    size_t      rn;
    struct { const param_t *p; const char *v; size_t vl; } log[LOG_ITEMS];   // KBUS writes, for the log
    size_t      nlog;
    char        sv[REQ_MAX];                               // values converted from stringValue
    size_t      svn;
} batch_t;

static batch_t *batch_new(void)
{
    batch_t *bt = malloc(sizeof(*bt));
    if (bt == NULL)
        return NULL;
    memset(bt->kb, 0xFF, sizeof(bt->kb));                 // all -1
    memset(bt->kbn, 0, sizeof(bt->kbn));
    bt->rescan = 0;
    bt->reset_stats = 0;
    bt->cycle_ms = 0;
    bt->local_ctrl = -1;
    bt->rn = 0;
    bt->nlog = 0;
    bt->svn = 0;
    return bt;
}

static int verr(verr_t *e, int status, int code, const param_t *p, const char *msg)
{
    char path[96];
    path_of(p, path, sizeof(path));
    e->status = status;
    e->code = code;
    snprintf(e->detail, sizeof(e->detail), "%s: %s", path, msg);
    return status;
}

// stringValue (manual chapter 6): the string representation of a numeric value,
// "123" or ["1","2"]. Converted into the plain JSON value in bt->sv.
static int from_string_value(batch_t *bt, const param_t *p, const char *s, size_t n,
                             const char **v, size_t *vl, verr_t *e)
{
    char msg[96];
    const dtype_t t = dtype_of(p);
    if (!is_numeric(t)) {
        snprintf(msg, sizeof(msg), "dataType %s has no stringValue; use value", dtype_name(t));
        return verr(e, 400, WC_WRONG_REPR, p, msg);
    }
    char *out = bt->sv + bt->svn;
    const size_t cap = sizeof(bt->sv) - bt->svn;
    size_t o = 0;
    jr_t j = { s, n, 0 };
    jr_ws(&j);
    const int arr = j.i < j.n && j.s[j.i] == '[';
    if (arr != is_array(p))
        return verr(e, 400, WC_WRONG_REPR, p, arr ? "stringValue must be one string"
                                                  : "stringValue must be an array of strings");
    if (arr) {
        j.i++;
        out[o++] = '[';
    }
    for (int first = 1;; first = 0) {
        jr_ws(&j);
        if (arr && first && j.i < j.n && j.s[j.i] == ']')
            break;
        const char *str; size_t sl;
        if (jr_string(&j, &str, &sl) != 0 || sl == 0 || sl > 32)
            return verr(e, 400, WC_WRONG_REPR, p, "stringValue must contain numbers in double quotes");
        for (size_t i = 0; i < sl; ++i)
            if (!strchr("0123456789+-.eE", str[i]))
                return verr(e, 400, WC_WRONG_REPR, p, "stringValue must contain numbers in double quotes");
        if (o + sl + 2 >= cap)
            return verr(e, 400, WC_WRONG_REPR, p, "stringValue too long");
        if (!first)
            out[o++] = ',';
        memcpy(out + o, str, sl);
        o += sl;
        if (!arr)
            break;
        jr_ws(&j);
        if (j.i < j.n && j.s[j.i] == ',') { j.i++; continue; }
        if (j.i < j.n && j.s[j.i] == ']') break;
        return verr(e, 400, WC_WRONG_REPR, p, "stringValue must be an array of strings");
    }
    if (arr)
        out[o++] = ']';
    *v = out;
    *vl = o;
    bt->svn += o;
    return 0;
}

// Check one value and add it to the batch. Returns 0 or an HTTP status (e set).
// control: the request came through the control socket (the dashboard).
static int stage(batch_t *bt, const gw_config_t *cfg, int control, const param_t *p, const char *v, size_t vl,
                 verr_t *e)
{
    if (!writeable(p))
        return verr(e, 400, WC_NOT_WRITEABLE, p, "parameter is read-only");
    if ((p->kind == P_LOCALCTRL || p->kind == P_RESETSTATS) && !control)
        return verr(e, 403, WC_UNAUTHORIZED, p, "only through the dashboard, after its login");
    if (p->kind != P_REG_VALUE && !control && __atomic_load_n(&local_control, __ATOMIC_RELAXED))
        return verr(e, 403, WC_UNAUTHORIZED, p, "local control: only the dashboard may write the KBUS");
    if (p->kind != P_REG_VALUE && p->kind != P_LOCALCTRL && p->kind != P_RESETSTATS && !cfg->http_write_enable)
        return verr(e, 403, WC_UNAUTHORIZED, p, "KBUS writes are disabled (HTTP_WRITE_ENABLE=0)");

    const io_kind_t k = (io_kind_t)p->cls;
    const int digital = is_digital(k);
    switch ((pkind_t)p->kind) {
    case P_LOCALCTRL: {
        int32_t x;
        const int c = parse_scalar(v, vl, 1, &x);
        if (c)
            return verr(e, 400, c, p, "value must be true or false");
        bt->local_ctrl = x;
        return 0;
    }
    case P_RESCAN:
    case P_RESETSTATS: {
        // true = rescan the bus / reset the statistics after this request; false does nothing
        int32_t x;
        const int c = parse_scalar(v, vl, 1, &x);
        if (c)
            return verr(e, 400, c, p, "value must be true or false");
        if (p->kind == P_RESCAN)
            bt->rescan |= x;
        else
            bt->reset_stats |= x;
        return 0;
    }
    case P_CYCLETIME: {
        int32_t x;
        const int c = parse_scalar(v, vl, 0, &x);
        if (c == WC_WRONG_TYPE || c == WC_VALUE_NULL)
            return verr(e, 400, c, p, "value must be an integer 1..40 (ms)");
        if (c || x < GW_CYCLE_MS_MIN || x > GW_CYCLE_MS_MAX)
            return verr(e, 400, WC_INVALID_VALUE, p, "cycle time must be 1..40 ms");
        bt->cycle_ms = x;
        return 0;
    }
    case P_CH_VALUE: {
        int32_t x;
        const int c = parse_scalar(v, vl, digital, &x);
        if (c)
            return verr(e, 400, c, p, digital ? "value must be true or false" : "value must be an integer 0..65535");
        bt->kb[k][p->idx - 1] = x;
        if (bt->kbn[k] < p->idx)
            bt->kbn[k] = p->idx;
        break;
    }
    case P_CLS_VALUES: {                                  // array of a whole class
        size_t n = 0;
        int32_t tmp[GW_MAX_CHANNELS];
        jr_t j = { v, vl, 0 };
        jr_ws(&j);
        if (span_eq(v, vl, "null"))
            return verr(e, 400, WC_VALUE_NULL, p, "value must be an array");
        if (j.i >= j.n || j.s[j.i] != '[')
            return verr(e, 400, WC_WRONG_TYPE, p, "value must be an array");
        j.i++;
        jr_ws(&j);
        if (j.i < j.n && j.s[j.i] == ']')
            return verr(e, 400, WC_INVALID_VALUE, p, "empty array");
        for (;;) {
            jr_ws(&j);
            const size_t st = j.i;
            if (jr_skip(&j, 0) != 0)
                return verr(e, 400, WC_WRONG_TYPE, p, "malformed array");
            if (n >= gateway_channel_count(k))
                return verr(e, 400, WC_INVALID_VALUE, p, "more values than channels");
            const int c = parse_scalar(v + st, j.i - st, digital, &tmp[n]);
            if (c)
                return verr(e, 400, c, p, digital ? "values must be true or false"
                                                  : "values must be integers 0..65535");
            n++;
            jr_ws(&j);
            if (j.i < j.n && j.s[j.i] == ',') { j.i++; continue; }
            if (j.i < j.n && j.s[j.i] == ']') break;
            return verr(e, 400, WC_WRONG_TYPE, p, "malformed array");
        }
        memcpy(bt->kb[k], tmp, n * sizeof(tmp[0]));       // only a valid array is staged
        if (bt->kbn[k] < n)
            bt->kbn[k] = n;
        break;
    }
    case P_REG_VALUE: {
        const reg_def_t *d = registers_def(p->idx);
        reg_value_t rv;
        jr_t j = { v, vl, 0 };
        jr_ws(&j);
        if (span_eq(v + j.i, vl - j.i, "null"))
            return verr(e, 400, WC_VALUE_NULL, p, "null can not be written");
        const int r = registers_parse(d->type, v, vl, &rv);
        if (r) {
            char msg[96];
            snprintf(msg, sizeof(msg), r == 400 ? "value must be a JSON %s" : "%s value out of range or too long",
                     reg_type_name(d->type));
            return verr(e, 400, r == 400 ? WC_WRONG_TYPE : WC_INVALID_VALUE, p, msg);
        }
        size_t i = 0;
        while (i < bt->rn && bt->ridx[i] != p->idx)       // the same register twice: the last value wins
            i++;
        if (i == bt->rn) {
            if (bt->rn >= REG_MAX)
                return verr(e, 400, WC_INVALID_VALUE, p, "too many registers");
            bt->rn++;
        }
        bt->ridx[i] = p->idx;
        bt->rval[i] = rv;
        return 0;
    }
    default:
        return verr(e, 400, WC_NOT_WRITEABLE, p, "parameter is read-only");
    }
    if (bt->nlog < LOG_ITEMS) {
        bt->log[bt->nlog].p = p;
        bt->log[bt->nlog].v = v;
        bt->log[bt->nlog].vl = vl;
        bt->nlog++;
    }
    return 0;
}

static int commit(batch_t *bt, const gw_config_t *cfg, wbuf_t *b)
{
    int kchanged = 0;
    int kbus = 0;
    for (int k = 0; k < IO_KIND_COUNT; ++k)
        kbus |= bt->kbn[k] > 0;
    if (kbus) {                                           // first: the only step that can still fail
        const int32_t *all[IO_KIND_COUNT];
        for (int k = 0; k < IO_KIND_COUNT; ++k)
            all[k] = bt->kb[k];
        if (gateway_write_multi(all, bt->kbn, &kchanged) != GW_OK)
            return error(b, 500, WC_COULD_NOT_SET, NULL, "the KBUS outputs could not be set");
    }
    if (bt->rn)
        registers_write(bt->ridx, bt->rval, bt->rn, NULL);
    if (bt->local_ctrl >= 0 && bt->local_ctrl != __atomic_load_n(&local_control, __ATOMIC_RELAXED)) {
        __atomic_store_n(&local_control, bt->local_ctrl, __ATOMIC_RELAXED);
        LOG_INFO("local control %s via the dashboard: %s", bt->local_ctrl ? "ON" : "OFF",
                 bt->local_ctrl ? "only the dashboard may write the KBUS" : "all clients may write the KBUS");
        config_store_local_control(cfg, bt->local_ctrl);
    }
    if (bt->reset_stats) {
        gateway_reset_stats();
        LOG_INFO("KBUS statistics reset via the dashboard");
    }
    if (bt->cycle_ms) {
        const int old = gateway_set_cycle_ms(bt->cycle_ms);
        if (old != bt->cycle_ms) {
            LOG_INFO("KBUS cycle time %d -> %d ms via KBus/CycleTime", old, bt->cycle_ms);
            config_store_cycle(cfg, bt->cycle_ms);
        }
    }
    if (bt->rescan) {
        gateway_request_rescan();
        LOG_INFO("KBUS rescan requested via KBus/Rescan");
    }
    if (cfg->http_log >= 2 || (cfg->http_log >= 1 && kchanged)) {
        for (size_t i = 0; i < bt->nlog; ++i) {
            char path[96];
            path_of(bt->log[i].p, path, sizeof(path));
            LOG_INFO("write %s = %.*s", path, (int)(bt->log[i].vl > 200 ? 200 : bt->log[i].vl), bt->log[i].v);
        }
    }
    return 0;
}

// The value of one parameter object: attributes.value, or attributes.stringValue
// for numeric types. Returns 0, -1 (neither present) or an HTTP status (e set).
static int value_of(batch_t *bt, const param_t *p, const char *obj, size_t ol,
                    const char **v, size_t *vl, const char **member, verr_t *e)
{
    static const char *const k_value[] = { "attributes", "value" };
    static const char *const k_sval[]  = { "attributes", "stringValue" };
    if (jr_get(obj, ol, k_value, 2, v, vl) == 0) {
        *member = "value";
        return 0;
    }
    const char *s; size_t sl;
    if (jr_get(obj, ol, k_sval, 2, &s, &sl) != 0)
        return -1;
    *member = "stringValue";
    return from_string_value(bt, p, s, sl, v, vl, e);
}

// PATCH /wda/parameters/{id}: one parameter
static int do_patch(const char *req, size_t len, const gw_config_t *cfg, int control, const param_t *p,
                    const char *id, size_t idlen, wbuf_t *b)
{
    if (!has_jsonapi_content_type(req, len))
        return error(b, 415, WC_NONE, NULL, "Content-Type must be application/vnd.api+json");
    size_t blen = 0;
    const char *body = body_of(req, len, &blen);
    if (body == NULL || blen == 0)
        return error(b, 400, WC_NONE, NULL, "no body");

    static const char *const k_data[] = { "data" };
    static const char *const k_type[] = { "type" };
    static const char *const k_id[]   = { "id" };
    const char *d, *v; size_t dl, vl;
    if (jr_get(body, blen, k_data, 1, &d, &dl) != 0 || d[0] != '{')
        return error(b, 400, WC_NONE, "/data", "data must be one parameter object");
    if (jr_get(d, dl, k_type, 1, &v, &vl) != 0 || !span_eq(v, vl, "\"parameters\""))
        return error(b, 400, WC_NONE, "/data/type", "data.type must be \"parameters\"");
    if (jr_get(d, dl, k_id, 1, &v, &vl) != 0 || vl != idlen + 2 || memcmp(v + 1, id, idlen) != 0)
        return error(b, 400, WC_NONE, "/data/id", "data.id must match the parameter id in the URL");

    batch_t *bt = batch_new();
    if (bt == NULL)
        return error(b, 500, WC_INTERNAL, NULL, "out of memory");
    verr_t e;
    const char *member = "value";
    int st = value_of(bt, p, d, dl, &v, &vl, &member, &e);
    if (st < 0)
        st = error(b, 400, WC_NONE, "/data/attributes", "data.attributes.value missing");
    else if (st == 0 && (st = stage(bt, cfg, control, p, v, vl, &e)) == 0)
        st = commit(bt, cfg, b);
    if (st > 0 && b->len == 0) {
        char ptr[48];
        snprintf(ptr, sizeof(ptr), "/data/attributes/%s", member);
        error(b, e.status, e.code, ptr, e.detail);
    }
    free(bt);
    if (st)
        return st;
    b->len = 0;
    return 204;
}

// PATCH /wda/parameters: {"data":[{"type":"parameters","id":...,"attributes":{"value":...}}, ...]}
// Every parameter is checked; each one that fails gets an error object, every
// other one an error object with code 41 (58 when only permissions failed).
static int do_patch_batch(const char *req, size_t len, const gw_config_t *cfg, int control, wbuf_t *b)
{
    if (!has_jsonapi_content_type(req, len))
        return error(b, 415, WC_NONE, NULL, "Content-Type must be application/vnd.api+json");
    size_t blen = 0;
    const char *body = body_of(req, len, &blen);
    if (body == NULL || blen == 0)
        return error(b, 400, WC_NONE, NULL, "no body");
    static const char *const k_data[] = { "data" };
    static const char *const k_type[] = { "type" };
    static const char *const k_id[]   = { "id" };
    const char *d; size_t dl;
    if (jr_get(body, blen, k_data, 1, &d, &dl) != 0)
        return error(b, 400, WC_NONE, "/data", "data missing");
    jr_t j = { d, dl, 0 };
    jr_ws(&j);
    if (j.i >= j.n || j.s[j.i] != '[')
        return error(b, 400, WC_NONE, "/data",
                     "data must be an array of parameters (one parameter: PATCH /wda/parameters/{id})");
    j.i++;
    jr_ws(&j);
    if (j.i < j.n && j.s[j.i] == ']')
        return error(b, 400, WC_NONE, "/data", "data is empty");

    batch_t *bt = batch_new();
    struct item { const param_t *p; uint16_t ok; } *items = malloc(MAX_ITEMS * sizeof(*items));
    if (bt == NULL || items == NULL) {
        free(bt);
        free(items);
        return error(b, 500, WC_INTERNAL, NULL, "out of memory");
    }
    wbuf_t eb = { NULL, 0, 0, "" };                       // error objects of single parameters
    int nerr = 0, status = 0, st = 0, only403 = 1;
    size_t n = 0;
    char ptr[64];
    for (;; ++n) {
        jr_ws(&j);
        const size_t es = j.i;
        if (n >= MAX_ITEMS) {
            st = error(b, 400, WC_NONE, "/data", "too many parameters in one request");
            break;
        }
        if (jr_skip(&j, 0) != 0 || d[es] != '{') {
            snprintf(ptr, sizeof(ptr), "/data/%zu", n);
            st = error(b, 400, WC_NONE, ptr, "malformed data array");
            break;
        }
        const char *e = d + es;
        const size_t el = j.i - es;
        const char *v; size_t vl;
        if (jr_get(e, el, k_type, 1, &v, &vl) != 0 || !span_eq(v, vl, "\"parameters\"")) {
            snprintf(ptr, sizeof(ptr), "/data/%zu/type", n);
            st = error(b, 400, WC_NONE, ptr, "every data[].type must be \"parameters\"");
            break;
        }
        if (jr_get(e, el, k_id, 1, &v, &vl) != 0 || vl < 2 || v[0] != '"') {
            snprintf(ptr, sizeof(ptr), "/data/%zu/id", n);
            st = error(b, 400, WC_NONE, ptr, "data[].id missing");
            break;
        }
        const param_t *p = find_id(v + 1, vl - 2);
        items[n].p = p;
        items[n].ok = 0;
        verr_t er;
        const char *member = "value";
        int r;
        if (p == NULL) {
            snprintf(er.detail, sizeof(er.detail), "unknown parameter id %.*s",
                     (int)(vl - 2 > ID_MAX ? ID_MAX : vl - 2), v + 1);
            er.status = 404;
            er.code = WC_UNKNOWN_PATH;
            snprintf(ptr, sizeof(ptr), "/data/%zu/id", n);
            r = 404;
        } else if ((r = value_of(bt, p, e, el, &v, &vl, &member, &er)) < 0) {
            snprintf(ptr, sizeof(ptr), "/data/%zu/attributes", n);
            st = error(b, 400, WC_NONE, ptr, "data[].attributes.value missing");
            break;
        } else {
            if (r == 0)
                r = stage(bt, cfg, control, p, v, vl, &er);
            snprintf(ptr, sizeof(ptr), "/data/%zu/attributes/%s", n, member);
        }
        if (r) {
            wb_printf(&eb, nerr++ ? "," : "");
            error_object(&eb, er.status, er.code, ptr, er.detail);
            status = (status == 0 || status == er.status) ? er.status : 400;
            only403 &= er.status == 403;
        } else {
            items[n].ok = 1;
        }
        jr_ws(&j);
        if (j.i < j.n && j.s[j.i] == ',') { j.i++; continue; }
        if (j.i < j.n && j.s[j.i] == ']') { n++; break; }
        snprintf(ptr, sizeof(ptr), "/data/%zu", n + 1);
        st = error(b, 400, WC_NONE, ptr, "malformed data array");
        break;
    }
    if (st == 0 && nerr) {
        // the valid parameters were not written either (all or nothing)
        for (size_t i = 0; i < n; ++i) {
            if (!items[i].ok)
                continue;
            char path[96], detail[160];
            path_of(items[i].p, path, sizeof(path));
            snprintf(detail, sizeof(detail), "%s: not written, another value of the request was rejected", path);
            snprintf(ptr, sizeof(ptr), "/data/%zu", i);
            wb_printf(&eb, ",");
            error_object(&eb, only403 ? 403 : 400, only403 ? WC_OTHER_UNAUTH : WC_OTHER_INVALID, ptr, detail);
            status = (status == (only403 ? 403 : 400)) ? status : 400;
        }
        b->len = 0;
        wb_printf(b, "{\"errors\":[%s]}", eb.data ? eb.data : "");
        st = status;
    }
    if (st == 0)
        st = commit(bt, cfg, b);
    wbuf_free(&eb);
    free(items);
    free(bt);
    if (st)
        return st;
    b->len = 0;
    return 204;
}

// Build the parameter list and the id table for the current bus
// configuration. Cheap when nothing changed; after a rescan (new
// KBus/ConfigurationVersion) the tree is rebuilt with the new modules.
// Only call while no request is being handled (http_server.c: tree lock).
void wda_config(const gw_config_t *cfg)
{
    __atomic_store_n(&local_control, cfg->local_control ? 1 : 0, __ATOMIC_RELAXED);
}

void wda_init(void)
{
    const uint32_t v = gateway_config_version();
    if (v == built_version)
        return;
    build_list();
    build_ids();
    built_version = v;
}

static int do_get_one(const param_t *p, wbuf_t *b)
{
    values_t *v = values_take();
    if (v == NULL)
        return error(b, 500, WC_INTERNAL, NULL, "out of memory");
    wb_printf(b, "{\"data\":");
    write_resource(b, p, v);
    wb_printf(b, "}");
    free(v);
    return 200;
}

static int not_found(wbuf_t *b, const char *id, size_t idlen)
{
    char detail[ID_MAX + 40];
    snprintf(detail, sizeof(detail), "unknown parameter id %.*s", (int)(idlen > ID_MAX ? ID_MAX : idlen), id);
    return error(b, 404, WC_UNKNOWN_PATH, NULL, detail);
}

static int starts(const char *s, size_t n, const char *lit)
{
    const size_t l = strlen(lit);
    return n >= l && memcmp(s, lit, l) == 0;
}

int wda_handle(const char *req, size_t len, const gw_config_t *cfg, int control, wbuf_t *b)
{
    const int patch = (len >= 6 && memcmp(req, "PATCH ", 6) == 0);
    const int get   = (len >= 4 && memcmp(req, "GET ", 4) == 0);
    if (!patch && !get)
        return error(b, 405, WC_NONE, NULL, "use GET or PATCH on /wda/parameters");
    size_t pos = patch ? 6 : 4;
    size_t end = pos;
    while (end < len && req[end] != ' ' && req[end] != '\r' && req[end] != '\n')
        end++;
    const char *path = req + pos;
    size_t plen = end - pos;
    const char *query = "";
    size_t qlen = 0;
    const char *qm = memchr(path, '?', plen);
    if (qm) {
        query = qm + 1;
        qlen = (size_t)(path + plen - query);
        plen = (size_t)(qm - path);
    }
    if (plen > 1 && path[plen - 1] == '/')               // trailing slash: the same resource
        plen--;
    if (!accepts_jsonapi(req, len))
        return error(b, 406, WC_NONE, NULL, "the answer is application/vnd.api+json");

    // GET /wda: service identity
    if (span_eq(path, plen, "/wda"))
        return patch ? error(b, 405, WC_NONE, NULL, "use GET on /wda") : do_service_identity(b);

    // /wda/parameter-definitions[/{id}]: read only
    static const char defs[] = "/wda/parameter-definitions";
    if (span_eq(path, plen, defs) || starts(path, plen, "/wda/parameter-definitions/")) {
        if (patch)
            return error(b, 405, WC_NONE, NULL, "definitions are read-only");
        if (plen == sizeof(defs) - 1)
            return do_collection(defs, query, qlen, 1, cfg, b);
        const char *id = path + sizeof(defs);
        const size_t idlen = plen - sizeof(defs);
        const param_t *p = find_id(id, idlen);
        if (p == NULL)
            return not_found(b, id, idlen);
        wb_printf(b, "{\"data\":");
        write_definition(b, p, cfg);
        wb_printf(b, "}");
        return 200;
    }

    static const char base[] = "/wda/parameters";
    const size_t blen = sizeof(base) - 1;
    if (!starts(path, plen, base) || (plen > blen && path[blen] != '/'))
        return error(b, 404, WC_NONE, NULL, "unknown resource; use /wda/parameters");

    // Collection
    if (plen == blen)
        return patch ? do_patch_batch(req, len, cfg, control, b) : do_collection(base, query, qlen, 0, cfg, b);

    // One parameter, or /{id}/definition[/{redirect_path}]
    const char *id = path + blen + 1;
    size_t idlen = plen - blen - 1;
    const char *slash = memchr(id, '/', idlen);
    if (slash) {
        const char *rest = slash + 1;
        const size_t rl = (size_t)(id + idlen - rest);
        idlen = (size_t)(slash - id);
        if (!(starts(rest, rl, "definition") && (rl == 10 || rest[10] == '/')))
            return error(b, 404, WC_NONE, NULL, "unknown resource");
        if (find_id(id, idlen) == NULL)
            return not_found(b, id, idlen);
        if (patch)
            return error(b, 405, WC_NONE, NULL, "definitions are read-only");
        // 301 to the definition; redirect_path (and the query) are appended
        snprintf(b->location, sizeof(b->location), "/wda/parameter-definitions/%.*s%.*s%s%.*s",
                 (int)idlen, id, (int)(rl - 10), rest + 10, qlen ? "?" : "", (int)qlen, query);
        b->len = 0;
        return 301;
    }
    const param_t *p = find_id(id, idlen);
    if (p == NULL)
        return not_found(b, id, idlen);
    return patch ? do_patch(req, len, cfg, control, p, id, idlen, b) : do_get_one(p, b);
}
