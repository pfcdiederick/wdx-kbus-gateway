// Registers: named values shared between containers (see registers.h).
#include "registers.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct {
    reg_def_t       def[REG_MAX];
    reg_value_t     val[REG_MAX];
    size_t          count;
    uint64_t        changes;        // Registers/ChangeCounter
    int             dirty;          // a persistent register changed since the last save
    int             save_failed;    // log a failing save only once
    struct timespec last_save;
    const char     *store;          // REGISTER_STORE ("" = no persistence)
    int             save_s;         // REGISTER_SAVE_S
    pthread_mutex_t lock;
} rg = { .lock = PTHREAD_MUTEX_INITIALIZER };

static const char *const type_names[] = { "boolean", "int32", "uint32", "float64", "string" };

const char *reg_type_name(reg_type_t type) { return type_names[type]; }

static int type_parse(const char *s, size_t n, reg_type_t *t)
{
    for (int i = 0; i < (int)(sizeof(type_names) / sizeof(type_names[0])); ++i)
        if (strlen(type_names[i]) == n && strncasecmp(s, type_names[i], n) == 0) {
            *t = (reg_type_t)i;
            return 0;
        }
    return -1;
}

size_t registers_count(void) { return rg.count; }

const reg_def_t *registers_def(size_t index) { return index < rg.count ? &rg.def[index] : NULL; }

// ---------------------------------------------------------------------------
// JSON values
// ---------------------------------------------------------------------------
static void trim(const char **s, size_t *n)
{
    while (*n && isspace((unsigned char)**s))
        (*s)++, (*n)--;
    while (*n && isspace((unsigned char)(*s)[*n - 1]))
        (*n)--;
}

static int parse_int(const char *s, size_t n, int64_t min, int64_t max, int64_t *out)
{
    size_t i = 0;
    int neg = 0;
    if (n && s[0] == '-')
        neg = 1, i = 1;
    if (i >= n)
        return 400;
    int64_t v = 0;
    for (; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return 400;                       // also rejects 1.5, 1e3, true, "1"
        if (v > 10000000000LL)
            return 409;                       // far beyond 32 bits
        v = v * 10 + (s[i] - '0');
    }
    if (neg)
        v = -v;
    if (v < min || v > max)
        return 409;
    *out = v;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// JSON string literal (with quotes) -> UTF-8 in out (NUL terminated).
static int parse_string(const char *s, size_t n, char *out, size_t cap)
{
    if (n < 2 || s[0] != '"' || s[n - 1] != '"')
        return 400;
    size_t o = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        if (c == '"' || c < 0x20)
            return 400;
        if (c != '\\') {
            if (o + 1 >= cap)
                return 409;
            out[o++] = (char)c;
            continue;
        }
        if (++i + 1 >= n)
            return 400;
        switch (s[i]) {
        case '"':  cp = '"';  break;
        case '\\': cp = '\\'; break;
        case '/':  cp = '/';  break;
        case 'b':  cp = '\b'; break;
        case 'f':  cp = '\f'; break;
        case 'n':  cp = '\n'; break;
        case 'r':  cp = '\r'; break;
        case 't':  cp = '\t'; break;
        case 'u': {
            if (i + 4 >= n)
                return 400;
            cp = 0;
            for (int k = 1; k <= 4; ++k) {
                const int h = hexval(s[i + k]);
                if (h < 0)
                    return 400;
                cp = cp * 16 + (unsigned)h;
            }
            i += 4;
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
                return 400;                   // no NUL, no surrogate pairs
            break;
        }
        default:
            return 400;
        }
        // UTF-8 encode
        char u[3];
        size_t ul;
        if (cp < 0x80)       { u[0] = (char)cp; ul = 1; }
        else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); ul = 2; }
        else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
               u[2] = (char)(0x80 | (cp & 0x3F)); ul = 3; }
        if (o + ul >= cap)
            return 409;
        memcpy(out + o, u, ul);
        o += ul;
    }
    out[o] = '\0';
    return 0;
}

int registers_parse(reg_type_t type, const char *s, size_t n, reg_value_t *out)
{
    trim(&s, &n);
    memset(out, 0, sizeof(*out));
    switch (type) {
    case REG_BOOL:
        if (n == 4 && memcmp(s, "true", 4) == 0)  { out->i = 1; return 0; }
        if (n == 5 && memcmp(s, "false", 5) == 0) { out->i = 0; return 0; }
        return 400;
    case REG_INT32:
        return parse_int(s, n, INT32_MIN, INT32_MAX, &out->i);
    case REG_UINT32:
        return parse_int(s, n, 0, UINT32_MAX, &out->i);
    case REG_FLOAT64: {
        char tmp[64];
        if (n == 0 || n >= sizeof(tmp))
            return 400;
        for (size_t i = 0; i < n; ++i)        // JSON number characters only (no nan, inf, hex)
            if (!strchr("0123456789+-.eE", s[i]))
                return 400;
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        char *end = NULL;
        errno = 0;
        const double d = strtod(tmp, &end);
        if (end != tmp + n)
            return 400;
        if (errno == ERANGE || !isfinite(d))
            return 409;
        out->f = d;
        return 0;
    }
    case REG_STRING:
        return parse_string(s, n, out->s, sizeof(out->s));
    }
    return 400;
}

size_t registers_format(reg_type_t type, const reg_value_t *v, char *buf, size_t len)
{
    int n = 0;
    switch (type) {
    case REG_BOOL:    n = snprintf(buf, len, v->i ? "true" : "false"); break;
    case REG_INT32:
    case REG_UINT32:  n = snprintf(buf, len, "%lld", (long long)v->i); break;
    case REG_FLOAT64:
        // shortest of %.15g / %.17g that reads back exactly
        n = snprintf(buf, len, "%.15g", v->f);
        if (strtod(buf, NULL) != v->f)
            n = snprintf(buf, len, "%.17g", v->f);
        break;
    case REG_STRING: {
        size_t o = 0;
        if (len < 3)
            break;
        buf[o++] = '"';
        for (const unsigned char *p = (const unsigned char *)v->s; *p && o + 8 < len; ++p) {
            if (*p == '"' || *p == '\\')
                o += (size_t)snprintf(buf + o, len - o, "\\%c", *p);
            else if (*p < 0x20)
                o += (size_t)snprintf(buf + o, len - o, "\\u%04x", *p);
            else
                buf[o++] = (char)*p;
        }
        buf[o++] = '"';
        buf[o] = '\0';
        return o;
    }
    }
    return n < 0 ? 0 : ((size_t)n < len ? (size_t)n : len - 1);
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------
void registers_snapshot(reg_value_t *out, uint64_t *change_counter)
{
    pthread_mutex_lock(&rg.lock);
    memcpy(out, rg.val, rg.count * sizeof(rg.val[0]));
    if (change_counter)
        *change_counter = rg.changes;
    pthread_mutex_unlock(&rg.lock);
}

static int value_equal(reg_type_t t, const reg_value_t *a, const reg_value_t *b)
{
    switch (t) {
    case REG_FLOAT64: return memcmp(&a->f, &b->f, sizeof(a->f)) == 0;
    case REG_STRING:  return strcmp(a->s, b->s) == 0;
    default:          return a->i == b->i;
    }
}

void registers_write(const size_t *idx, const reg_value_t *vals, size_t n, int *changed)
{
    int ch = 0;
    pthread_mutex_lock(&rg.lock);
    for (size_t k = 0; k < n; ++k) {
        const size_t i = idx[k];
        if (i >= rg.count)
            continue;
        if (!value_equal(rg.def[i].type, &rg.val[i], &vals[k])) {
            rg.val[i] = vals[k];
            ch = 1;
            if (rg.def[i].persistent)
                rg.dirty = 1;
        }
    }
    if (ch)
        rg.changes++;
    pthread_mutex_unlock(&rg.lock);
    if (changed)
        *changed = ch;
}

// ---------------------------------------------------------------------------
// Declarations file
// ---------------------------------------------------------------------------
static int name_valid(const char *s, size_t n)
{
    if (n == 0 || n > REG_NAME_MAX)
        return 0;
    int seg_start = 1;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c == '/') {
            if (seg_start)
                return 0;                         // empty segment or leading '/'
            seg_start = 1;
            continue;
        }
        if (seg_start ? !isalpha((unsigned char)c) : !(isalnum((unsigned char)c) || c == '_'))
            return 0;
        seg_start = 0;
    }
    return !seg_start;                            // no trailing '/'
}

// Next token: a JSON string in quotes, or a run of non-space characters.
static int next_token(const char **p, const char **tok, size_t *len)
{
    const char *s = *p;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return 0;
    const char *st = s;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1])
                s++;
            s++;
        }
        if (*s != '"')
            return -1;
        s++;
    } else {
        while (*s && !isspace((unsigned char)*s))
            s++;
    }
    *tok = st;
    *len = (size_t)(s - st);
    *p = s;
    return 1;
}

// Strip a '#' comment that is not inside a JSON string.
static void strip_comment(char *line)
{
    int in_str = 0;
    for (char *s = line; *s; ++s) {
        if (in_str) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"')
                in_str = 0;
        } else if (*s == '"') {
            in_str = 1;
        } else if (*s == '#') {
            *s = '\0';
            return;
        }
    }
}

static int find_name(const char *s, size_t n)
{
    for (size_t i = 0; i < rg.count; ++i)
        if (strlen(rg.def[i].name) == n && strncasecmp(rg.def[i].name, s, n) == 0)
            return (int)i;
    return -1;
}

static void parse_decl_line(const char *file, int lineno, char *line)
{
    strip_comment(line);
    const char *p = line, *tok[5];
    size_t len[5];
    int nt = 0, r;
    while (nt < 5 && (r = next_token(&p, &tok[nt], &len[nt])) == 1)
        nt++;
    if (nt == 0)
        return;
    if (r < 0 || nt == 5 || nt < 2) {
        LOG_ERROR("%s:%d: expected '<Name> <type> [default] [persistent]', line ignored", file, lineno);
        return;
    }
    if (!name_valid(tok[0], len[0])) {
        LOG_ERROR("%s:%d: invalid name '%.*s' (segments A-Z a-z 0-9 _ separated by '/', "
                  "starting with a letter, max %d characters)", file, lineno, (int)len[0], tok[0], REG_NAME_MAX);
        return;
    }
    if (find_name(tok[0], len[0]) >= 0) {
        LOG_ERROR("%s:%d: register '%.*s' declared twice (names are not case sensitive)",
                  file, lineno, (int)len[0], tok[0]);
        return;
    }
    reg_def_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.name, tok[0], len[0]);
    if (type_parse(tok[1], len[1], &d.type) != 0) {
        LOG_ERROR("%s:%d: unknown type '%.*s' (boolean, int32, uint32, float64, string)",
                  file, lineno, (int)len[1], tok[1]);
        return;
    }
    int ti = 2;
    reg_value_t v;
    memset(&v, 0, sizeof(v));
    if (ti < nt && !(len[ti] == 10 && strncasecmp(tok[ti], "persistent", 10) == 0)) {
        r = registers_parse(d.type, tok[ti], len[ti], &v);
        if (r != 0) {
            LOG_ERROR("%s:%d: default '%.*s' is not a valid %s value", file, lineno,
                      (int)len[ti], tok[ti], reg_type_name(d.type));
            return;
        }
        ti++;
    }
    if (ti < nt) {
        if (!(len[ti] == 10 && strncasecmp(tok[ti], "persistent", 10) == 0) || ti + 1 != nt) {
            LOG_ERROR("%s:%d: unexpected '%.*s' (only 'persistent' may follow the default)",
                      file, lineno, (int)len[ti], tok[ti]);
            return;
        }
        d.persistent = 1;
    }
    if (rg.count >= REG_MAX) {
        LOG_ERROR("%s:%d: more than %d registers, line ignored", file, lineno, REG_MAX);
        return;
    }
    rg.def[rg.count] = d;
    rg.val[rg.count] = v;
    rg.count++;
}

// Stored values: "<Name> <type> <JSON value>" per line; only names that are
// still declared with the same type and as persistent are taken over.
static void load_store(void)
{
    FILE *f = fopen(rg.store, "r");
    if (f == NULL) {
        if (errno != ENOENT)
            LOG_WARN("cannot read %s: %s", rg.store, strerror(errno));
        return;
    }
    char line[REG_JSON_MAX + REG_NAME_MAX + 32];
    size_t restored = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line, *name, *type, *val;
        size_t nl, tl, vl;
        if (next_token(&p, &name, &nl) != 1 || next_token(&p, &type, &tl) != 1 ||
            next_token(&p, &val, &vl) != 1)
            continue;
        const int i = find_name(name, nl);
        reg_type_t t;
        if (i < 0 || !rg.def[i].persistent || type_parse(type, tl, &t) != 0 || t != rg.def[i].type)
            continue;
        reg_value_t v;
        if (registers_parse(t, val, vl, &v) == 0) {
            rg.val[i] = v;
            restored++;
        }
    }
    fclose(f);
    LOG_INFO("registers: %zu persistent value(s) restored from %s", restored, rg.store);
}

void registers_load(const gw_config_t *cfg)
{
    rg.count = 0;
    rg.store = cfg->register_store;
    rg.save_s = cfg->register_save_s;
    clock_gettime(CLOCK_MONOTONIC, &rg.last_save);

    FILE *f = fopen(cfg->register_file, "r");
    if (f == NULL) {
        LOG_INFO("registers: none (%s: %s)", cfg->register_file, strerror(errno));
        return;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        parse_decl_line(cfg->register_file, lineno, line);
    }
    fclose(f);

    size_t npers = 0;
    for (size_t i = 0; i < rg.count; ++i)
        npers += rg.def[i].persistent;
    LOG_INFO("registers: %zu declared in %s, %zu persistent", rg.count, cfg->register_file, npers);
    if (npers && *rg.store)
        load_store();
    else if (npers)
        LOG_WARN("registers: REGISTER_STORE is empty, persistent values are not stored");
}

// ---------------------------------------------------------------------------
// Persistence: write a temporary file, fsync, rename (never a half file).
// ---------------------------------------------------------------------------
static int write_store(const reg_value_t *vals)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s", rg.store);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", rg.store);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return -1;
    char json[REG_JSON_MAX];
    for (size_t i = 0; i < rg.count; ++i) {
        if (!rg.def[i].persistent)
            continue;
        registers_format(rg.def[i].type, &vals[i], json, sizeof(json));
        fprintf(f, "%s %s %s\n", rg.def[i].name, reg_type_name(rg.def[i].type), json);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, rg.store) != 0) {
        unlink(tmp);
        return -1;
    }
    if (slash && slash != dir) {
        const int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return 0;
}

void registers_save(int force)
{
    if (!rg.dirty || !*rg.store)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && now.tv_sec - rg.last_save.tv_sec < rg.save_s)
        return;

    static reg_value_t copy[REG_MAX];             // only the server's main thread saves
    pthread_mutex_lock(&rg.lock);
    memcpy(copy, rg.val, rg.count * sizeof(rg.val[0]));
    rg.dirty = 0;
    pthread_mutex_unlock(&rg.lock);

    rg.last_save = now;
    if (write_store(copy) != 0) {
        const int err = errno;
        pthread_mutex_lock(&rg.lock);
        rg.dirty = 1;                             // try again at the next interval
        pthread_mutex_unlock(&rg.lock);
        if (!rg.save_failed)
            LOG_ERROR("registers: cannot write %s: %s", rg.store, strerror(err));
        rg.save_failed = 1;
        return;
    }
    if (rg.save_failed)
        LOG_INFO("registers: %s written again", rg.store);
    rg.save_failed = 0;
}
