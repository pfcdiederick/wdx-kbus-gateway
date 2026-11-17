// HTTP server for the wdx data layer (/wda/parameters, see wda.c).
//
// - One client at a time: accept, read the request (max 4096 bytes; a body that
//   Content-Length announces is awaited up to 100 ms), handle, send the response
//   (with Content-Length), close. No time-out for the first data: clients must
//   close on their own time-out.
// - GET / (and /index.html) returns the built-in web page (web/index.html),
//   which itself only uses the wdx data layer.
// - Every other request goes to the wdx data layer; anything that is not
//   GET/PATCH /wda/parameters... gets a JSON:API error (404 / 405).
// - HTTP_PRIORITY: optional SCHED_FIFO priority of this thread.
// - HTTP_LOG: 0 = nothing, 1 = writes that change an output (wda.c), 2 = every request.
#define _GNU_SOURCE  // accept4, SOCK_NONBLOCK
#include "http_server.h"
#include "log.h"
#include "wda.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define RX_MAX         4096   // one read per request

extern const char web_index_html[];   // web/index.html, embedded by src/web_asset.S

// "GET / ..." or "GET /index.html ..." (path followed by a space or the end).
static int is_page_request(const char *rx, size_t n)
{
    static const char *const paths[] = { "GET / ", "GET /index.html " };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        const size_t l = strlen(paths[i]);
        if ((n >= l && memcmp(rx, paths[i], l) == 0) ||
            (n == l - 1 && memcmp(rx, paths[i], l - 1) == 0))
            return 1;
    }
    return 0;
}
#define LISTEN_BACKLOG 1
#define POLL_MS        100    // only to notice the stop flag; not a client time-out

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

// Expected total request size from the header: header length + Content-Length.
// 0 while the header is incomplete or has no Content-Length.
static size_t expected_size(const char *rx, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i) {
        if (rx[i] == '\r' && rx[i + 1] == '\n' && rx[i + 2] == '\r' && rx[i + 3] == '\n') {
            for (size_t p = 0; p + 16 < i; ++p) {
                if ((p == 0 || rx[p - 1] == '\n') && strncasecmp(rx + p, "content-length:", 15) == 0) {
                    size_t q = p + 15, cl = 0, digits = 0;
                    while (q < i && rx[q] == ' ')
                        q++;
                    while (q < i && rx[q] >= '0' && rx[q] <= '9' && digits < 7)
                        cl = cl * 10 + (size_t)(rx[q++] - '0'), digits++;
                    return digits ? i + 4 + cl : 0;
                }
            }
            return 0;
        }
    }
    return 0;
}

static void serve_client(int cfd, const char *peer, const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    // Receive: the first read normally holds the whole request. Browsers may
    // send the body of a PATCH in a second TCP segment: when Content-Length
    // announces more, wait up to 100 ms for the rest (never longer).
    static char rx[RX_MAX];
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
    const size_t want = expected_size(rx, (size_t)n);
    for (int waits = 0; want > (size_t)n && (size_t)n < sizeof(rx) && waits < 10; ++waits) {
        struct pollfd pf = { .fd = cfd, .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 10) <= 0)
            continue;
        const ssize_t m = recv(cfd, rx + n, sizeof(rx) - (size_t)n, 0);
        if (m <= 0)
            break;
        n += m;
    }

    if (is_page_request(rx, (size_t)n)) {
        const size_t len = strlen(web_index_html);
        char head[192];
        const int hl = snprintf(head, sizeof(head),
                                "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                "Content-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", len);
        if (cfg->http_log >= 2)
            LOG_INFO("%s \"GET /\" -> 200 web page (%zu bytes)", peer, len);
        if (send_all(cfd, head, (size_t)hl, stop) == 0)
            send_all(cfd, web_index_html, len, stop);
        return;
    }

    wbuf_t body = { NULL, 0, 0 };
    const int status = wda_handle(rx, (size_t)n, cfg, &body);

    if (cfg->http_log >= 2) {
        size_t rl = 0;                          // request line = up to the first CR/LF
        while (rl < (size_t)n && rl < 100 && rx[rl] != '\r' && rx[rl] != '\n')
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

int http_server_run(const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    if (!cfg->http_enable) {
        LOG_WARN("HTTP server disabled (HTTP_ENABLE=0)");
        while (!*stop)
            pause();
        return 0;
    }

    wda_init();          // parameter/id tables of the wdx data layer, built once

    // Real-time priority for the HTTP server thread (this thread only; the KBUS
    // cycle and the WAGO library threads already exist and keep their own).
    // Keeps request latency short when other programs load the CPU.
    if (cfg->http_priority > 0) {
        struct sched_param sp = { .sched_priority = cfg->http_priority };
        const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (r == 0)
            LOG_INFO("HTTP server thread: SCHED_FIFO priority %d", cfg->http_priority);
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
            LOG_INFO("wdx data layer listening on port %d (GET /wda/parameters)", cfg->http_port);
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
