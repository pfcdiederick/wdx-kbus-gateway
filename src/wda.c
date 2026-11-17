// wdx-style parameter interface for the KBUS data.
//
// Follows the conventions of the WAGO wdx model / WAGO Device Access (WDA):
//   - one flat list of parameters, JSON:API 1.0 (application/vnd.api+json)
//   - path  = hierarchical name, e.g. KBus/DigitalOutputs/3/Value
//   - id    = "0-0-" + lower-case path with '/' -> '-',
//             e.g. 0-0-kbus-digitaloutputs-3-value
//   - dataType (boolean, uint16, uint64, string, instantiations), dataRank
//     (scalar | array), value
//   - GET reads, PATCH writes exactly one parameter; the JSON:API body's type
//     and id must match the target resource
//   - write rights are not in the value payload; GET .../{id}/definition
//
// Requests
//   GET   /wda/parameters                     all parameters (page[limit], page[offset])
//   GET   /wda/parameters?filter[id]=a,b      exactly these parameters, one snapshot
//   GET   /wda/parameters/{id}                one parameter
//   GET   /wda/parameters/{id}/definition     path, type, rank, writeable
//   PATCH /wda/parameters/{id}                body {"data":{"type":"parameters","id":"{id}",
//                                                   "attributes":{"value":...}}}  -> 204
// Status: 200, 204, 400 malformed body, 403 writes disabled, 404 unknown id,
//         405 wrong method, 409 read-only / value rejected, 415 wrong Content-Type.
//
// Parameter tree
//   KBus/Healthy, CycleTime (ms), CycleCount, ErrorCount
//   KBus/Modules                 instantiations [1..n]
//   KBus/Modules/{m}/Name, TypeCode, ChannelCount
//   KBus/DigitalInputs|DigitalOutputs|AnalogInputs|AnalogOutputs   instantiations
//   KBus/<Class>/{n}/Value (DO/AO writable), Module, ModuleChannel
//   KBus/DigitalInputValues|DigitalOutputValues|AnalogInputValues|AnalogOutputValues
//                                 all values of a class as one array (DO/AO writable)
#include "wda.h"
#include "gateway.h"
#include "log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static int error(wbuf_t *b, int status, const char *title, const char *detail)
{
    b->len = 0;
    wb_printf(b, "{\"errors\":[{\"status\":\"%d\",\"title\":", status);
    wb_json_string(b, title);
    wb_printf(b, ",\"detail\":");
    wb_json_string(b, detail);
    wb_printf(b, "}]}");
    return status;
}

// ---------------------------------------------------------------------------
// Parameter list
// ---------------------------------------------------------------------------
typedef enum { DT_BOOL, DT_UINT16, DT_UINT64, DT_STRING, DT_INST } dtype_t;

typedef enum {
    P_HEALTHY, P_CYCLETIME, P_CYCLES, P_ERRORS, P_MODULES,
    P_MOD_NAME, P_MOD_TYPE, P_MOD_CHCOUNT,
    P_CLS_INST, P_CH_VALUE, P_CH_MODULE, P_CH_MODCH, P_CLS_VALUES
} pkind_t;

typedef struct {
    uint8_t  kind;    // pkind_t
    uint8_t  cls;     // io_kind_t for class parameters
    uint16_t idx;     // module or channel number (1-based)
} param_t;

#define MAX_PARAMS (8 + KBUS_MAX_MODULES * 3 + IO_KIND_COUNT * (2 + GW_MAX_CHANNELS * 3))

#define ID_MAX 48                    // longest: 0-0-kbus-digitaloutputs-250-modulechannel

static param_t  plist[MAX_PARAMS];
static char     pids[MAX_PARAMS][ID_MAX];   // ids precomputed once: lookups stay cheap
static uint8_t  pidlen[MAX_PARAMS];
static size_t   pcount;
static uint16_t mod_channels[KBUS_MAX_MODULES];
static int      built;

static const char *const cls_name[IO_KIND_COUNT]   = { "DigitalInputs", "DigitalOutputs",
                                                       "AnalogInputs", "AnalogOutputs" };
static const char *const cls_values[IO_KIND_COUNT] = { "DigitalInputValues", "DigitalOutputValues",
                                                       "AnalogInputValues", "AnalogOutputValues" };

static void add(pkind_t k, io_kind_t cls, size_t idx)
{
    if (pcount < MAX_PARAMS)
        plist[pcount++] = (param_t){ (uint8_t)k, (uint8_t)cls, (uint16_t)idx };
}

// The module list and channel map are fixed after gateway_init(), so the
// parameter list is built once, on the first request.
static void build_list(void)
{
    if (built)
        return;
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
    built = 1;
}

static void id_of(const param_t *p, char *buf, size_t len);

static void build_ids(void)
{
    static int done;
    if (done)
        return;
    for (size_t i = 0; i < pcount; ++i) {
        id_of(&plist[i], pids[i], ID_MAX);
        pidlen[i] = (uint8_t)strlen(pids[i]);
    }
    done = 1;
}

static int is_digital(io_kind_t k) { return k == IO_DI || k == IO_DO; }

static void path_of(const param_t *p, char *buf, size_t len)
{
    switch ((pkind_t)p->kind) {
    case P_HEALTHY:     snprintf(buf, len, "KBus/Healthy"); break;
    case P_CYCLETIME:   snprintf(buf, len, "KBus/CycleTime"); break;
    case P_CYCLES:      snprintf(buf, len, "KBus/CycleCount"); break;
    case P_ERRORS:      snprintf(buf, len, "KBus/ErrorCount"); break;
    case P_MODULES:     snprintf(buf, len, "KBus/Modules"); break;
    case P_MOD_NAME:    snprintf(buf, len, "KBus/Modules/%u/Name", p->idx); break;
    case P_MOD_TYPE:    snprintf(buf, len, "KBus/Modules/%u/TypeCode", p->idx); break;
    case P_MOD_CHCOUNT: snprintf(buf, len, "KBus/Modules/%u/ChannelCount", p->idx); break;
    case P_CLS_INST:    snprintf(buf, len, "KBus/%s", cls_name[p->cls]); break;
    case P_CH_VALUE:    snprintf(buf, len, "KBus/%s/%u/Value", cls_name[p->cls], p->idx); break;
    case P_CH_MODULE:   snprintf(buf, len, "KBus/%s/%u/Module", cls_name[p->cls], p->idx); break;
    case P_CH_MODCH:    snprintf(buf, len, "KBus/%s/%u/ModuleChannel", cls_name[p->cls], p->idx); break;
    case P_CLS_VALUES:  snprintf(buf, len, "KBus/%s", cls_values[p->cls]); break;
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
    case P_HEALTHY:   return DT_BOOL;
    case P_CYCLES:
    case P_ERRORS:    return DT_UINT64;
    case P_MODULES:
    case P_CLS_INST:  return DT_INST;
    case P_MOD_NAME:  return DT_STRING;
    case P_CH_VALUE:
    case P_CLS_VALUES: return is_digital((io_kind_t)p->cls) ? DT_BOOL : DT_UINT16;
    default:          return DT_UINT16;
    }
}

static int is_array(const param_t *p)  { return p->kind == P_CLS_VALUES; }
static int writeable(const param_t *p)
{
    return (p->kind == P_CH_VALUE || p->kind == P_CLS_VALUES) && io_kind_writable((io_kind_t)p->cls);
}

static const char *dtype_name(dtype_t t)
{
    switch (t) {
    case DT_BOOL:   return "boolean";
    case DT_UINT16: return "uint16";
    case DT_UINT64: return "uint64";
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

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t   snap[IO_KIND_COUNT][GW_MAX_CHANNELS];
    gw_stats_t stats;
} values_t;

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
    }
}

static void write_resource(wbuf_t *b, const param_t *p, const values_t *v)
{
    char id[112], path[96];
    id_of(p, id, sizeof(id));
    path_of(p, path, sizeof(path));
    wb_printf(b, "{\"type\":\"parameters\",\"id\":\"%s\",\"attributes\":{\"path\":\"%s\","
                 "\"dataType\":\"%s\",\"dataRank\":\"%s\",\"value\":",
              id, path, dtype_name(dtype_of(p)), is_array(p) ? "array" : "scalar");
    write_value(b, p, v);
    wb_printf(b, "}}");
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
// Returns 0 ok, 400 wrong type, 409 out of range.
static int parse_scalar(const char *s, size_t n, int digital, int32_t *out)
{
    if (digital) {
        if (span_eq(s, n, "true"))  { *out = 1; return 0; }
        if (span_eq(s, n, "false")) { *out = 0; return 0; }
        return 400;
    }
    if (n == 0 || n > 10)
        return n ? 409 : 400;
    int64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == 0 && s[i] == '-' && n > 1)
            return 409;
        if (s[i] < '0' || s[i] > '9')
            return 400;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 65535)
        return 409;
    *out = (int32_t)v;
    return 0;
}

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------
// "page[limit]=N" or URL encoded "page%5Blimit%5D=N" in the query string.
static long query_int(const char *q, size_t qlen, const char *name, long def)
{
    char a[40], b[40];
    snprintf(a, sizeof(a), "page[%s]=", name);
    snprintf(b, sizeof(b), "page%%5b%s%%5d=", name);
    for (size_t i = 0; i < qlen; ++i) {
        size_t l = 0;
        if (qlen - i >= strlen(a) && memcmp(q + i, a, strlen(a)) == 0)
            l = strlen(a);
        else if (qlen - i >= strlen(b) && strncasecmp(q + i, b, strlen(b)) == 0)
            l = strlen(b);
        if (l && (i == 0 || q[i - 1] == '&')) {
            long v = 0;
            size_t k = i + l, d = 0;
            while (k < qlen && q[k] >= '0' && q[k] <= '9' && d < 9)
                v = v * 10 + (q[k++] - '0'), d++;
            return d ? v : def;
        }
    }
    return def;
}

// "filter[id]=a,b,c" or URL encoded "filter%5Bid%5D=a%2Cb": returns the raw
// value span (still with %2C separators) or NULL.
static const char *query_filter_ids(const char *q, size_t qlen, size_t *vlen)
{
    static const char a[] = "filter[id]=", b[] = "filter%5bid%5d=";
    for (size_t i = 0; i < qlen; ++i) {
        size_t l = 0;
        if (qlen - i >= sizeof(a) - 1 && memcmp(q + i, a, sizeof(a) - 1) == 0)
            l = sizeof(a) - 1;
        else if (qlen - i >= sizeof(b) - 1 && strncasecmp(q + i, b, sizeof(b) - 1) == 0)
            l = sizeof(b) - 1;
        if (l && (i == 0 || q[i - 1] == '&')) {
            size_t e = i + l;
            while (e < qlen && q[e] != '&')
                e++;
            *vlen = e - (i + l);
            return q + i + l;
        }
    }
    return NULL;
}

// Is pids[i] one of the ids in the filter value (separators ',' or %2C)?
static int filter_has(const char *f, size_t flen, size_t i)
{
    size_t p = 0;
    while (p < flen) {
        size_t e = p, next;
        for (;;) {
            if (e >= flen) { next = e; break; }
            if (f[e] == ',') { next = e + 1; break; }
            if (e + 2 < flen && f[e] == '%' && f[e + 1] == '2' && (f[e + 2] == 'C' || f[e + 2] == 'c')) {
                next = e + 3;
                break;
            }
            e++;
        }
        if (e - p == pidlen[i] && memcmp(f + p, pids[i], e - p) == 0)
            return 1;
        p = next;
    }
    return 0;
}

static int has_jsonapi_content_type(const char *req, size_t len)
{
    for (size_t i = 0; i + 1 < len; ++i) {
        if (req[i] == '\r' && req[i + 1] == '\n') {
            const char *h = req + i + 2;
            const size_t rest = len - (i + 2);
            if (rest >= 2 && h[0] == '\r' && h[1] == '\n')
                return 0;                                   // end of headers
            if (rest > 13 && strncasecmp(h, "content-type:", 13) == 0) {
                size_t e = 13;
                while (e < rest && h[e] != '\r' && h[e] != '\n')
                    e++;
                const char *needle = "application/vnd.api+json";
                for (size_t p = 13; p + strlen(needle) <= e; ++p)
                    if (strncasecmp(h + p, needle, strlen(needle)) == 0)
                        return 1;
                return 0;
            }
        }
    }
    return 0;
}

static int do_patch(const char *req, size_t len, const gw_config_t *cfg, const param_t *p,
                    const char *id, size_t idlen, wbuf_t *b)
{
    if (!has_jsonapi_content_type(req, len))
        return error(b, 415, "Unsupported Media Type", "Content-Type must be application/vnd.api+json");
    if (!cfg->http_write_enable)
        return error(b, 403, "Forbidden", "output writes are disabled (HTTP_WRITE_ENABLE=0)");

    const char *body = NULL;
    for (size_t i = 0; i + 3 < len; ++i)
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            body = req + i + 4;
            break;
        }
    if (body == NULL)
        return error(b, 400, "Bad Request", "no body");
    const size_t blen = (size_t)(req + len - body);

    static const char *const k_type[]  = { "data", "type" };
    static const char *const k_id[]    = { "data", "id" };
    static const char *const k_value[] = { "data", "attributes", "value" };
    const char *v; size_t vl;
    if (jr_get(body, blen, k_type, 2, &v, &vl) != 0 || !span_eq(v, vl, "\"parameters\""))
        return error(b, 400, "Bad Request", "data.type must be \"parameters\"");
    if (jr_get(body, blen, k_id, 2, &v, &vl) != 0 || vl != idlen + 2 || memcmp(v + 1, id, idlen) != 0)
        return error(b, 400, "Bad Request", "data.id must match the parameter id in the URL");
    if (jr_get(body, blen, k_value, 3, &v, &vl) != 0)
        return error(b, 400, "Bad Request", "data.attributes.value missing");
    if (!writeable(p))
        return error(b, 409, "Conflict", "parameter is read-only");

    const io_kind_t k = (io_kind_t)p->cls;
    const int digital = is_digital(k);
    if (p->kind == P_CH_VALUE) {
        int32_t x;
        const int r = parse_scalar(v, vl, digital, &x);
        if (r)
            return error(b, r, r == 400 ? "Bad Request" : "Conflict",
                         digital ? "value must be true or false" : "value must be an integer 0..65535");
        if (gateway_write(k, p->idx, x) != GW_OK)
            return error(b, 409, "Conflict", "value rejected");
    } else {                                                // array of a whole class
        static int32_t vals[GW_MAX_CHANNELS];
        size_t n = 0;
        jr_t j = { v, vl, 0 };
        jr_ws(&j);
        if (j.i >= j.n || j.s[j.i] != '[')
            return error(b, 400, "Bad Request", "value must be an array");
        j.i++;
        jr_ws(&j);
        if (j.i < j.n && j.s[j.i] == ']')
            return error(b, 409, "Conflict", "empty array");
        for (;;) {
            jr_ws(&j);
            const size_t st = j.i;
            if (jr_skip(&j, 0) != 0)
                return error(b, 400, "Bad Request", "malformed array");
            if (n >= gateway_channel_count(k))
                return error(b, 409, "Conflict", "more values than channels");
            const int r = parse_scalar(v + st, j.i - st, digital, &vals[n]);
            if (r)
                return error(b, r, r == 400 ? "Bad Request" : "Conflict",
                             digital ? "values must be true or false" : "values must be integers 0..65535");
            n++;
            jr_ws(&j);
            if (j.i < j.n && j.s[j.i] == ',') { j.i++; continue; }
            if (j.i < j.n && j.s[j.i] == ']') break;
            return error(b, 400, "Bad Request", "malformed array");
        }
        const int32_t *all[IO_KIND_COUNT] = { NULL, NULL, NULL, NULL };
        size_t cnt[IO_KIND_COUNT] = { 0, 0, 0, 0 };
        all[k] = vals;
        cnt[k] = n;
        if (gateway_write_multi(all, cnt) != GW_OK)
            return error(b, 409, "Conflict", "values rejected");
    }
    if (cfg->http_log >= 2 || (cfg->http_log >= 1 && gateway_write_changed())) {
        char path[96];
        path_of(p, path, sizeof(path));
        LOG_INFO("write %s = %.*s", path, (int)(vl > 200 ? 200 : vl), v);
    }
    b->len = 0;
    return 204;
}

// Build the parameter list and the id table (call once after gateway_init(),
// so the first request is not slower than the others).
void wda_init(void)
{
    build_list();
    build_ids();
}

int wda_handle(const char *req, size_t len, const gw_config_t *cfg, wbuf_t *b)
{
    wda_init();

    // The wdx data layer is the only interface: GET and PATCH, nothing else.
    const int patch = (len >= 6 && memcmp(req, "PATCH ", 6) == 0);
    const int get   = (len >= 4 && memcmp(req, "GET ", 4) == 0);
    if (!patch && !get)
        return error(b, 405, "Method Not Allowed", "use GET or PATCH on /wda/parameters");
    size_t pos = patch ? 6 : 4;
    size_t end = pos;
    while (end < len && req[end] != ' ' && req[end] != '\r' && req[end] != '\n')
        end++;
    const char *path = req + pos;
    size_t plen = end - pos;
    const char *query = NULL;
    size_t qlen = 0;
    const char *qm = memchr(path, '?', plen);
    if (qm) {
        query = qm + 1;
        qlen = (size_t)(path + plen - query);
        plen = (size_t)(qm - path);
    }

    static const char base[] = "/wda/parameters";
    const size_t blen = sizeof(base) - 1;
    if (plen < blen || memcmp(path, base, blen) != 0 ||
        (plen > blen && path[blen] != '/'))
        return error(b, 404, "Not Found", "unknown resource; use /wda/parameters");

    static values_t v;
    gateway_snapshot(v.snap);
    gateway_stats(&v.stats);

    // Collection
    if (plen == blen || plen == blen + 1) {
        if (patch)
            return error(b, 405, "Method Not Allowed", "PATCH one parameter: /wda/parameters/{id}");
        size_t flen = 0;
        const char *filter = query_filter_ids(query, qlen, &flen);
        if (filter != NULL) {
            // filter[id]=a,b,c: exactly these parameters (in tree order), one snapshot
            size_t n = 0;
            wb_printf(b, "{\"data\":[");
            for (size_t i = 0; i < pcount; ++i) {
                if (!filter_has(filter, flen, i))
                    continue;
                if (n++)
                    wb_printf(b, ",");
                write_resource(b, &plist[i], &v);
            }
            wb_printf(b, "],\"meta\":{\"total\":%zu,\"offset\":0,\"count\":%zu}}", pcount, n);
            return 200;
        }
        long offset = query_int(query, qlen, "offset", 0);
        long limit  = query_int(query, qlen, "limit", (long)pcount);
        if (offset < 0 || (size_t)offset > pcount)
            offset = (long)pcount;
        size_t last = (size_t)offset + (size_t)(limit < 0 ? 0 : limit);
        if (last > pcount)
            last = pcount;
        wb_printf(b, "{\"data\":[");
        for (size_t i = (size_t)offset; i < last; ++i) {
            if (i > (size_t)offset)
                wb_printf(b, ",");
            write_resource(b, &plist[i], &v);
        }
        wb_printf(b, "],\"meta\":{\"total\":%zu,\"offset\":%ld,\"count\":%zu}}",
                  pcount, offset, last - (size_t)offset);
        return 200;
    }

    // One parameter, optionally /definition
    const char *id = path + blen + 1;
    size_t idlen = plen - blen - 1;
    int definition = 0;
    static const char def[] = "/definition";
    if (idlen > sizeof(def) - 1 && memcmp(id + idlen - (sizeof(def) - 1), def, sizeof(def) - 1) == 0) {
        definition = 1;
        idlen -= sizeof(def) - 1;
    }
    const param_t *p = find_id(id, idlen);
    if (p == NULL)
        return error(b, 404, "Not Found", "unknown parameter id");

    if (patch) {
        if (definition)
            return error(b, 405, "Method Not Allowed", "definitions are read-only");
        return do_patch(req, len, cfg, p, id, idlen, b);
    }
    if (definition) {
        char pid[112], ppath[96];
        id_of(p, pid, sizeof(pid));
        path_of(p, ppath, sizeof(ppath));
        wb_printf(b, "{\"data\":{\"type\":\"parameterdefinitions\",\"id\":\"%s\",\"attributes\":{"
                     "\"path\":\"%s\",\"dataType\":\"%s\",\"dataRank\":\"%s\",\"writeable\":%s,"
                     "\"persistent\":false}}}",
                  pid, ppath, dtype_name(dtype_of(p)), is_array(p) ? "array" : "scalar",
                  writeable(p) ? "true" : "false");
        return 200;
    }
    wb_printf(b, "{\"data\":");
    write_resource(b, p, &v);
    wb_printf(b, "}");
    return 200;
}
