// REST I/O server.
//
// 1. Single channel - behaviour identical to the CODESYS "REST I/O server on
//    PFC200" reference (FbREST, version 1.0 of 11-11-2026):
//
//   GET  /DI/n   ->  DIn=TRUE | DIn=FALSE
//   GET  /DO/n   ->  DOn=TRUE | DOn=FALSE     (value last written)
//   GET  /AI/n   ->  AIn=<raw 16-bit value>
//   GET  /AO/n   ->  AOn=<value>              (value last written)
//   POST /DO/n   body "1"|"TRUE"|"0"|"FALSE"
//   POST /AO/n   body 0..65535 (decimal digits only)
//
// 2. Bulk (extension, not in the reference) - all channels in one request:
//
//   GET  /DI | /DO        ->  DI=0110...   one char per channel, channel 1 first
//   GET  /AI | /AO        ->  AI=0,16384,...  decimal values, comma separated
//   GET  /ALL             ->  DI=...;DO=...;AI=...;AO=...
//   POST /DO   body 0110-1     '0'/'1' per channel from channel 1, '-' = unchanged
//   POST /AO   body 0,16384,-  values from channel 1, '-' = unchanged
//   POST /ALL  body DO=...;AO=...   (either part optional) -> answer as GET /ALL
//
//   Bulk writes are checked completely first and then applied under one lock,
//   so the KBUS cycle sends them together; nothing is written on an error.
//
// - One client at a time: accept, ONE read, decode, send the response, close.
//   Data arriving later is not read. No time-out. Single-channel requests
//   decode at most the first 512 bytes (as the reference); bulk requests up to
//   4096 bytes.
// - Headers are not evaluated; the body starts after the first CRLFCRLF (or at
//   the start of the request when there is none) and ends at the first CR/LF.
// - Status codes: 200, 403 (writes disabled) and 404 (everything else).
// - HTTP_LOG: 0 = nothing, 1 = writes (default), 2 = every request.
#define _GNU_SOURCE  // accept4, SOCK_NONBLOCK
#include "http_server.h"
#include "gateway.h"
#include "log.h"
#include "wda.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RX_MAX         512    // single-channel requests: as the reference
#define RX_BULK_MAX    4096   // receive buffer; bulk requests may use all of it
#define OUT_MAX        4096   // response body (GET /ALL with 4 x 250 channels fits)
#define LISTEN_BACKLOG 1
#define POLL_MS        100    // only to notice the stop flag; not a client time-out

static const char BODY_404[] = "Unknown endpoint, channel, or value";
static const char BODY_403[] = "Output writes are disabled";

// ---------------------------------------------------------------------------
// Decoding helpers
// ---------------------------------------------------------------------------

// Number built like the CODESYS code: a 32-bit signed variable (DINT) without
// an overflow check, i.e. two's complement wrap-around.
static int32_t dint_append_digit(int32_t n, char c)
{
    return (int32_t)((uint32_t)n * 10u + (uint32_t)(c - '0'));
}

static int starts_with(const char *buf, size_t len, const char *prefix)
{
    const size_t n = strlen(prefix);
    return len >= n && memcmp(buf, prefix, n) == 0;
}

// Body of a POST: from after the first CR LF CR LF (or the start of the
// request when there is none) up to the first CR or LF or the end.
static void find_body(const char *buf, size_t len, const char **body, size_t *body_len)
{
    size_t start = 0;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            start = i + 4;
            break;
        }
    }
    size_t end = start;
    while (end < len && buf[end] != '\r' && buf[end] != '\n')
        end++;
    *body = buf + start;
    *body_len = end - start;
}

static int body_equals(const char *body, size_t len, const char *s)
{
    return len == strlen(s) && memcmp(body, s, len) == 0;
}

static int kind_from_letters(char k0, char k1, io_kind_t *kind)
{
    if      (k0 == 'D' && k1 == 'I') *kind = IO_DI;
    else if (k0 == 'D' && k1 == 'O') *kind = IO_DO;
    else if (k0 == 'A' && k1 == 'I') *kind = IO_AI;
    else if (k0 == 'A' && k1 == 'O') *kind = IO_AO;
    else return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Single channel (reference behaviour, unchanged)
// ---------------------------------------------------------------------------
static int decode_single(const char *buf, size_t len, int post, size_t pos,
                         const gw_config_t *cfg, char *out, size_t out_len)
{
    if (len > RX_MAX)
        len = RX_MAX;                        // the reference only ever sees 512 bytes
    if (len < (post ? 10u : 9u))
        return 404;

    // Path: "/" + two letters + "/"
    if (pos + 4 > len || buf[pos] != '/' || buf[pos + 3] != '/')
        return 404;
    io_kind_t kind;
    if (kind_from_letters(buf[pos + 1], buf[pos + 2], &kind) != 0)
        return 404;
    pos += 4;

    // Channel number: digits up to the next space or the end of the data.
    int32_t n = 0;
    size_t digits = 0;
    for (; pos < len && buf[pos] != ' '; ++pos, ++digits) {
        if (buf[pos] < '0' || buf[pos] > '9')
            return 404;
        n = dint_append_digit(n, buf[pos]);
    }
    if (digits == 0 || n <= 0)
        return 404;

    if (post) {
        if (!io_kind_writable(kind))
            return 404;
        // The 403 check comes before the check of the channel count.
        if (!cfg->http_write_enable) {
            snprintf(out, out_len, "%s", BODY_403);
            return 403;
        }
    }
    if ((size_t)n > gateway_channel_count(kind))
        return 404;

    if (post) {
        const char *body;
        size_t blen;
        find_body(buf, len, &body, &blen);
        int64_t v;
        if (kind == IO_DO) {
            if (body_equals(body, blen, "1") || body_equals(body, blen, "TRUE"))
                v = 1;
            else if (body_equals(body, blen, "0") || body_equals(body, blen, "FALSE"))
                v = 0;
            else
                return 404;
        } else {
            if (blen == 0)
                return 404;
            int32_t a = 0;
            for (size_t i = 0; i < blen; ++i) {
                if (body[i] < '0' || body[i] > '9')
                    return 404;
                a = dint_append_digit(a, body[i]);
            }
            if (a < 0 || a > 65535)
                return 404;
            v = a;
        }
        if (gateway_write(kind, (size_t)n, v) != GW_OK)
            return 404;
        if (cfg->http_log >= 2 || (cfg->http_log >= 1 && gateway_write_changed()))
            LOG_INFO("write %s%d = %lld", io_kind_name(kind), (int)n, (long long)v);
    }

    int64_t value = 0;
    if (gateway_read(kind, (size_t)n, &value) != GW_OK)
        return 404;
    if (kind == IO_DI || kind == IO_DO)
        snprintf(out, out_len, "%s%d=%s", io_kind_name(kind), (int)n, value ? "TRUE" : "FALSE");
    else
        snprintf(out, out_len, "%s%d=%lld", io_kind_name(kind), (int)n, (long long)value);
    return 200;
}

// ---------------------------------------------------------------------------
// Bulk (extension)
// ---------------------------------------------------------------------------

// Digital values: one char per channel from channel 1: '0', '1', '-' (unchanged).
static int parse_digital(const char *s, size_t n, int32_t *vals, size_t max, size_t *count)
{
    if (n == 0 || n > max)
        return -1;
    for (size_t i = 0; i < n; ++i) {
        if      (s[i] == '0') vals[i] = 0;
        else if (s[i] == '1') vals[i] = 1;
        else if (s[i] == '-') vals[i] = -1;
        else return -1;
    }
    *count = n;
    return 0;
}

// Analog values: decimal 0..65535 or '-' (unchanged), comma separated, from channel 1.
static int parse_analog(const char *s, size_t n, int32_t *vals, size_t max, size_t *count)
{
    if (n == 0)
        return -1;
    size_t c = 0, i = 0;
    for (;;) {
        const size_t st = i;
        while (i < n && s[i] != ',')
            i++;
        const size_t tl = i - st;
        if (c >= max)
            return -1;
        if (tl == 1 && s[st] == '-') {
            vals[c] = -1;
        } else {
            if (tl == 0 || tl > 5)
                return -1;
            int32_t v = 0;
            for (size_t j = st; j < i; ++j) {
                if (s[j] < '0' || s[j] > '9')
                    return -1;
                v = v * 10 + (s[j] - '0');
            }
            if (v > 65535)
                return -1;
            vals[c] = v;
        }
        c++;
        if (i >= n)
            break;
        i++;                      // skip ','
        if (i >= n)
            return -1;            // trailing comma
    }
    *count = c;
    return 0;
}

static size_t append(char *out, size_t cap, size_t off, const char *fmt, unsigned v, int use_v)
{
    if (off >= cap)
        return off;
    const int w = use_v ? snprintf(out + off, cap - off, fmt, v) : snprintf(out + off, cap - off, "%s", fmt);
    return (w > 0) ? off + (size_t)w : off;
}

// "DO=01100000" or "AI=0,16384,7"
static size_t format_kind(char *out, size_t cap, size_t off, io_kind_t k,
                          uint16_t snap[IO_KIND_COUNT][GW_MAX_CHANNELS])
{
    off = append(out, cap, off, io_kind_name(k), 0, 0);
    off = append(out, cap, off, "=", 0, 0);
    const size_t n = gateway_channel_count(k);
    for (size_t i = 0; i < n; ++i) {
        if (k == IO_DI || k == IO_DO)
            off = append(out, cap, off, snap[k][i] ? "1" : "0", 0, 0);
        else
            off = append(out, cap, off, i ? ",%u" : "%u", snap[k][i], 1);
    }
    return off;
}

// Returns 1 and sets *status when the path is a bulk path, 0 otherwise.
static int decode_bulk(const char *buf, size_t len, int post, size_t pos,
                       const gw_config_t *cfg, char *out, size_t out_len, int *status)
{
    // Path "/ALL" or "/XX", followed by a space or the end of the data.
    int all = 0;
    io_kind_t kind = IO_DI;
    if (pos + 4 <= len && memcmp(buf + pos, "/ALL", 4) == 0 && (pos + 4 == len || buf[pos + 4] == ' ')) {
        all = 1;
    } else if (pos + 3 <= len && buf[pos] == '/' && kind_from_letters(buf[pos + 1], buf[pos + 2], &kind) == 0
               && (pos + 3 == len || buf[pos + 3] == ' ')) {
        all = 0;
    } else {
        return 0;
    }

    *status = 404;
    snprintf(out, out_len, "%s", BODY_404);

    if (post) {
        if (!all && !io_kind_writable(kind))
            return 1;
        if (!cfg->http_write_enable) {
            snprintf(out, out_len, "%s", BODY_403);
            *status = 403;
            return 1;
        }

        const char *body;
        size_t blen;
        find_body(buf, len, &body, &blen);

        static int32_t v_do[GW_MAX_CHANNELS], v_ao[GW_MAX_CHANNELS];
        const int32_t *vals[IO_KIND_COUNT] = { NULL, v_do, NULL, v_ao };
        size_t n[IO_KIND_COUNT] = { 0, 0, 0, 0 };

        if (!all) {
            const int r = (kind == IO_DO)
                ? parse_digital(body, blen, v_do, gateway_channel_count(IO_DO), &n[IO_DO])
                : parse_analog(body, blen, v_ao, gateway_channel_count(IO_AO), &n[IO_AO]);
            if (r != 0)
                return 1;
        } else {
            // "DO=...;AO=..." - each part at most once, at least one part
            size_t i = 0;
            int seen_do = 0, seen_ao = 0;
            if (blen == 0)
                return 1;
            while (i < blen) {
                size_t st = i;
                while (i < blen && body[i] != ';')
                    i++;
                const char *p = body + st;
                const size_t pl = i - st;
                if (pl > 3 && memcmp(p, "DO=", 3) == 0 && !seen_do) {
                    if (parse_digital(p + 3, pl - 3, v_do, gateway_channel_count(IO_DO), &n[IO_DO]) != 0)
                        return 1;
                    seen_do = 1;
                } else if (pl > 3 && memcmp(p, "AO=", 3) == 0 && !seen_ao) {
                    if (parse_analog(p + 3, pl - 3, v_ao, gateway_channel_count(IO_AO), &n[IO_AO]) != 0)
                        return 1;
                    seen_ao = 1;
                } else {
                    return 1;
                }
                if (i < blen) {
                    i++;                       // skip ';'
                    if (i >= blen)
                        return 1;              // trailing ';'
                }
            }
        }
        if (gateway_write_multi(vals, n) != GW_OK)
            return 1;
        if (cfg->http_log >= 2 || (cfg->http_log >= 1 && gateway_write_changed()))
            LOG_INFO("write %s%.*s", all ? "" : (kind == IO_DO ? "DO=" : "AO="), (int)blen, body);
    }

    static uint16_t snap[IO_KIND_COUNT][GW_MAX_CHANNELS];
    gateway_snapshot(snap);
    size_t off = 0;
    if (all) {
        for (int k = 0; k < IO_KIND_COUNT; ++k) {
            if (k)
                off = append(out, out_len, off, ";", 0, 0);
            off = format_kind(out, out_len, off, (io_kind_t)k, snap);
        }
    } else {
        off = format_kind(out, out_len, off, kind, snap);
    }
    *status = 200;
    return 1;
}

// Decode the received bytes and build the response body. Returns the status.
static int decode(const char *buf, size_t len, const gw_config_t *cfg, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", BODY_404);

    int post;
    size_t pos;
    if (starts_with(buf, len, "GET ")) {
        post = 0;
        pos = 4;
    } else if (starts_with(buf, len, "POST ")) {
        post = 1;
        pos = 5;
    } else {
        return 404;
    }

    int status;
    if (decode_bulk(buf, len, post, pos, cfg, out, out_len, &status))
        return status;
    return decode_single(buf, len, post, pos, cfg, out, out_len);
}

// ---------------------------------------------------------------------------
// Socket handling
// ---------------------------------------------------------------------------
static void set_nonblocking(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int open_listener(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

// Wait until fd is ready for `events` or the stop flag is set (no time-out).
static int wait_fd(int fd, short events, volatile sig_atomic_t *stop)
{
    while (!*stop) {
        struct pollfd p = { .fd = fd, .events = events, .revents = 0 };
        const int r = poll(&p, 1, POLL_MS);
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
    return -1;
}

static int send_all(int cfd, const char *p, size_t len, volatile sig_atomic_t *stop)
{
    size_t sent = 0;
    while (sent < len) {
        const ssize_t s = send(cfd, p + sent, len - sent, MSG_NOSIGNAL);
        if (s > 0) {
            sent += (size_t)s;
        } else if (s < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(cfd, POLLOUT, stop) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static const char *reason_of(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 415: return "Unsupported Media Type";
    default:  return "Error";
    }
}

// wdx-style interface: JSON:API answer with Content-Length.
static void serve_wda(int cfd, const char *rx, size_t n, const char *peer,
                      const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    wbuf_t body = { NULL, 0, 0 };
    const int status = wda_handle(rx, n, cfg, &body);
    if (cfg->http_log >= 2) {
        size_t rl = 0;
        while (rl < n && rl < 100 && rx[rl] != '\r' && rx[rl] != '\n')
            rl++;
        LOG_INFO("%s \"%.*s\" -> %d %.*s", peer, (int)rl, rx, status,
                 (int)(body.len > 120 ? 120 : body.len), body.data ? body.data : "");
    }
    char head[192];
    int hl;
    if (body.len > 0)
        hl = snprintf(head, sizeof(head),
                      "HTTP/1.0 %d %s\r\nContent-Type: application/vnd.api+json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, reason_of(status), body.len);
    else
        hl = snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                      status, reason_of(status));
    if (send_all(cfd, head, (size_t)hl, stop) == 0 && body.len > 0)
        send_all(cfd, body.data, body.len, stop);
    wbuf_free(&body);
}

static void serve_client(int cfd, const char *peer, const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    // Receive: ONE read.
    static char rx[RX_BULK_MAX];
    ssize_t n;
    for (;;) {
        if (wait_fd(cfd, POLLIN, stop) != 0)
            return;
        n = recv(cfd, rx, sizeof(rx), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        break;
    }
    if (n <= 0)
        return;   // closed by the client or error: close without a response

    if (wda_match(rx, (size_t)n)) {
        serve_wda(cfd, rx, (size_t)n, peer, cfg, stop);
        return;
    }

    static char body[OUT_MAX];
    const int status = decode(rx, (size_t)n, cfg, body, sizeof(body));
    const char *reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : "Not Found";

    if (cfg->http_log >= 2) {
        size_t rl = 0;                          // request line = up to the first CR/LF
        while (rl < (size_t)n && rl < 80 && rx[rl] != '\r' && rx[rl] != '\n')
            rl++;
        LOG_INFO("%s \"%.*s\" -> %d %.120s", peer, (int)rl, rx, status, body);
    }

    static char tx[OUT_MAX + 128];
    const int len = snprintf(tx, sizeof(tx),
                             "HTTP/1.0 %d %s\r\n"
                             "Content-Type: text/plain\r\n"
                             "Connection: close\r\n"
                             "\r\n"
                             "%s",
                             status, reason, body);

    // Send: partial sends are continued until everything is out.
    size_t sent = 0;
    while (sent < (size_t)len) {
        const ssize_t s = send(cfd, tx + sent, (size_t)len - sent, MSG_NOSIGNAL);
        if (s > 0) {
            sent += (size_t)s;
        } else if (s < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(cfd, POLLOUT, stop) != 0)
                return;
        } else {
            return;
        }
    }
}

int http_server_run(const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    if (!cfg->http_enable) {
        LOG_WARN("REST server disabled (HTTP_ENABLE=0)");
        while (!*stop)
            pause();
        return 0;
    }

    wda_init();          // parameter/id tables of the wdx-style interface, built once

    // Real-time priority for the REST server thread (this thread only; the KBUS
    // cycle and the WAGO library threads already exist and keep their own).
    // Keeps request latency short when the CODESYS runtime loads the CPU.
    if (cfg->http_priority > 0) {
        struct sched_param sp = { .sched_priority = cfg->http_priority };
        const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (r == 0)
            LOG_INFO("REST server thread: SCHED_FIFO priority %d", cfg->http_priority);
        else
            LOG_WARN("HTTP_PRIORITY %d not applied: %s (needs root / CAP_SYS_NICE)",
                     cfg->http_priority, strerror(r));
    }

    int lfd = -1;
    int warned = 0;
    while (!*stop) {
        // Listen: on failure retry until it works.
        if (lfd < 0) {
            lfd = open_listener(cfg->http_port);
            if (lfd < 0) {
                if (!warned)
                    LOG_ERROR("cannot listen on port %d: %s (retrying)", cfg->http_port, strerror(errno));
                warned = 1;
                usleep(POLL_MS * 1000);
                continue;
            }
            warned = 0;
            LOG_INFO("REST server listening on port %d (e.g. GET /DI/1, bulk GET /ALL)", cfg->http_port);
        }

        // Accept one client.
        if (wait_fd(lfd, POLLIN, stop) != 0)
            break;
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0)
            continue;
        char peer_s[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, peer_s, sizeof(peer_s));
        serve_client(cfd, peer_s, cfg, stop);
        close(cfd);
    }
    if (lfd >= 0)
        close(lfd);
    return 0;
}
