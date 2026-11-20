// Server for the wdx data layer (/wda/parameters, see wda.c).
//
// Listeners (all optional, the same requests on all):
//   - Unix socket WDX_SOCKET (default /run/kbus-gateway/wdx.sock): the
//     interface for containers. Bind-mount the DIRECTORY into a container
//     (-v /run/kbus-gateway:/run/kbus-gateway); access by file permissions
//     (WDX_SOCKET_MODE, WDX_SOCKET_GROUP).
//   - Unix socket CONTROL_SOCKET (default /run/kbus-gateway-control/control.sock,
//     mode 0600, directory 0700): only for the dashboard (kbus-manager). Requests
//     on it may switch KBus/LocalControl and KBus/ResetStatistics, and write the
//     KBUS while local control is on (wda.c). Mount its directory ONLY into the
//     manager container.
//   - TCP HTTP_BIND:HTTP_PORT (default 127.0.0.1:8000), HTTP_ENABLE=0 = off:
//     for host-network containers (CODESYS) and, with HTTP_BIND=0.0.0.0, the
//     web page from another PC.
//
// Connections: up to WDX_MAX_CLIENTS are served at the same time, one thread
// each. A client has WDX_CLIENT_TIMEOUT_MS to send its request (max RX_MAX
// bytes, Content-Length is honoured); one request per connection, the server
// closes it after the answer. When all slots are busy: 503.
//
// Bus rescans (KBus/Rescan, or persistent cycle errors) and the rebuild of the
// parameter tree run in the main thread while no request is being handled
// (tree lock); requests wait for them.
//
// GET / (and /index.html) returns the built-in web page (web/index.html).
// HTTP_PRIORITY: optional SCHED_FIFO priority, inherited by the client threads.
// HTTP_LOG: 0 = nothing, 1 = writes that change an output (wda.c), 2 = every request.
#define _GNU_SOURCE  // accept4, SOCK_NONBLOCK, struct ucred, pthread_rwlockattr_setkind_np
#include "http_server.h"
#include "gateway.h"
#include "log.h"
#include "registers.h"
#include "wda.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RX_MAX         16384  // one request (batch PATCH bodies included)
#define LISTEN_BACKLOG 16
#define POLL_MS        100    // main loop: rescans, register saves, stop flag
#define WORKER_STACK   (256 * 1024)

extern const char web_index_html[];   // web/index.html, embedded by src/web_asset.S

static const gw_config_t     *g_cfg;
static volatile sig_atomic_t *g_stop;
static pthread_rwlock_t       tree_lock;     // read: a request; write: rescan + rebuild
static pthread_mutex_t        active_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t         active_cond = PTHREAD_COND_INITIALIZER;
static int                    active;        // client threads running

typedef struct {
    int  fd;
    int  control;                // accepted on the control socket
    char peer[64];
} conn_t;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Wait until fd is ready for `events`; -1 at the deadline or on stop.
static int wait_fd(int fd, short events, int64_t deadline)
{
    while (!*g_stop) {
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return -1;
        struct pollfd p = { .fd = fd, .events = events, .revents = 0 };
        const int r = poll(&p, 1, left > POLL_MS ? POLL_MS : (int)left);
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
    return -1;
}

static int send_all(int cfd, const char *p, size_t len, int64_t deadline)
{
    size_t sent = 0;
    while (sent < len) {
        const ssize_t s = send(cfd, p + sent, len - sent, MSG_NOSIGNAL);
        if (s > 0) {
            sent += (size_t)s;
        } else if (s < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(cfd, POLLOUT, deadline) != 0)
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
    case 301: return "Moved Permanently";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

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

// Size of the whole request (header + Content-Length), 0 while the header is incomplete.
static size_t request_size(const char *rx, size_t n)
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
                    return i + 4 + cl;
                }
            }
            return i + 4;
        }
    }
    return 0;
}

// location: Location header of a 301 answer ("" = none)
static void send_response_loc(int cfd, int status, const char *ctype, const char *body, size_t blen,
                              const char *location, int64_t deadline)
{
    char head[512], loc[300] = "";
    int hl;
    if (location && *location)
        snprintf(loc, sizeof(loc), "Location: %.256s\r\n", location);
    if (blen > 0)
        hl = snprintf(head, sizeof(head),
                      "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                      status, reason_of(status), ctype, blen, loc);
    else
        hl = snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\n%sContent-Length: 0\r\nConnection: close\r\n\r\n",
                      status, reason_of(status), loc);
    if (send_all(cfd, head, (size_t)hl, deadline) == 0 && blen > 0)
        send_all(cfd, body, blen, deadline);
}

static void send_response(int cfd, int status, const char *ctype, const char *body, size_t blen,
                          int64_t deadline)
{
    send_response_loc(cfd, status, ctype, body, blen, "", deadline);
}

static void send_error(int cfd, int status, const char *detail, int64_t deadline)
{
    char body[256];
    const int n = snprintf(body, sizeof(body), "{\"errors\":[{\"status\":\"%d\",\"title\":\"%s\",\"detail\":\"%s\"}]}",
                           status, reason_of(status), detail);
    send_response(cfd, status, "application/vnd.api+json", body, (size_t)n, deadline);
}

// ---------------------------------------------------------------------------
// One client
// ---------------------------------------------------------------------------
static void serve_client(int cfd, const char *peer, int control)
{
    const gw_config_t *cfg = g_cfg;
    const int64_t deadline = now_ms() + cfg->client_timeout_ms;
    char *rx = malloc(RX_MAX);
    if (rx == NULL)
        return;

    // Receive until the header and the announced body are complete.
    size_t n = 0, want = 0;
    for (;;) {
        if (wait_fd(cfd, POLLIN, deadline) != 0) {
            if (n > 0 && !*g_stop)
                send_error(cfd, 408, "request incomplete", now_ms() + 500);
            free(rx);
            return;
        }
        const ssize_t m = recv(cfd, rx + n, RX_MAX - n, 0);
        if (m < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (m <= 0) {
            free(rx);
            return;                              // closed by the client: no answer
        }
        n += (size_t)m;
        want = request_size(rx, n);
        if (want > RX_MAX) {
            send_error(cfd, 413, "request larger than 16384 bytes", deadline);
            free(rx);
            return;
        }
        if ((want > 0 && n >= want) || n >= RX_MAX)
            break;
    }

    if (is_page_request(rx, n)) {
        const size_t len = strlen(web_index_html);
        if (cfg->http_log >= 2)
            LOG_INFO("%s \"GET /\" -> 200 web page (%zu bytes)", peer, len);
        send_response(cfd, 200, "text/html; charset=utf-8", web_index_html, len, deadline);
        free(rx);
        return;
    }

    wbuf_t body = { NULL, 0, 0, "" };
    pthread_rwlock_rdlock(&tree_lock);
    const int status = wda_handle(rx, n, cfg, control, &body);
    pthread_rwlock_unlock(&tree_lock);

    if (cfg->http_log >= 2) {
        size_t rl = 0;                          // request line = up to the first CR/LF
        while (rl < n && rl < 100 && rx[rl] != '\r' && rx[rl] != '\n')
            rl++;
        LOG_INFO("%s \"%.*s\" -> %d %.*s", peer, (int)rl, rx, status,
                 (int)(body.len > 120 ? 120 : body.len), body.data ? body.data : "");
    }
    send_response_loc(cfd, status, "application/vnd.api+json", body.data, body.len, body.location, deadline);
    wbuf_free(&body);
    free(rx);
}

static void *client_thread(void *arg)
{
    conn_t *c = arg;
    serve_client(c->fd, c->peer, c->control);
    close(c->fd);
    free(c);
    pthread_mutex_lock(&active_lock);
    active--;
    pthread_cond_signal(&active_cond);
    pthread_mutex_unlock(&active_lock);
    return NULL;
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------
static void set_nonblocking(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int open_tcp(const char *bind_addr, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

// group "" = keep root; dir_mode: mode of a directory created here
static int open_unix(const char *path, int mode, const char *group, int dir_mode)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);

    // directory (one level, e.g. /run/kbus-gateway)
    char dir[sizeof(addr.sun_path)];
    strcpy(dir, path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, (mode_t)dir_mode) != 0 && errno != EEXIST)
            return -1;
    }

    // An old socket file: remove it, unless another gateway still answers on it.
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe >= 0) {
            const int alive = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            close(probe);
            if (alive) {
                errno = EADDRINUSE;
                return -1;
            }
        }
        unlink(path);
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (*group) {
        const struct group *g = getgrnam(group);
        if (g == NULL || chown(path, (uid_t)-1, g->gr_gid) != 0)
            LOG_WARN("socket group '%s' of %s not applied (%s)", group, path,
                     g == NULL ? "no such group" : strerror(errno));
    }
    if (chmod(path, (mode_t)mode) != 0)
        LOG_WARN("chmod %04o %s failed: %s", (unsigned)mode, path, strerror(errno));
    set_nonblocking(fd);
    return fd;
}

static void peer_name(int cfd, int is_unix, const struct sockaddr_storage *ss, char *buf, size_t len)
{
    if (is_unix) {
        struct ucred cr;
        socklen_t cl = sizeof(cr);
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0)
            snprintf(buf, len, "unix(pid %d, uid %u)", (int)cr.pid, (unsigned)cr.uid);
        else
            snprintf(buf, len, "unix");
        return;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)ss;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    snprintf(buf, len, "%s", ip);
}

static void accept_one(int lfd, int is_unix, int control, pthread_attr_t *attr)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    const int cfd = accept4(lfd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0)
        return;

    pthread_mutex_lock(&active_lock);
    const int busy = active >= g_cfg->max_clients;
    if (!busy)
        active++;
    pthread_mutex_unlock(&active_lock);
    if (busy) {
        send_error(cfd, 503, "too many connections (WDX_MAX_CLIENTS)", now_ms() + 100);
        close(cfd);
        return;
    }

    conn_t *c = malloc(sizeof(*c));
    pthread_t t;
    if (c != NULL) {
        c->fd = cfd;
        c->control = control;
        peer_name(cfd, is_unix, &ss, c->peer, sizeof(c->peer));
        if (control) {
            char p[sizeof(c->peer)];
            memcpy(p, c->peer, sizeof(p));
            snprintf(c->peer, sizeof(c->peer), "control %.55s", p);
        }
        if (pthread_create(&t, attr, client_thread, c) == 0)
            return;
        free(c);
    }
    send_error(cfd, 503, "no resources for this connection", now_ms() + 100);
    close(cfd);
    pthread_mutex_lock(&active_lock);
    active--;
    pthread_mutex_unlock(&active_lock);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
int http_server_run(const gw_config_t *cfg, volatile sig_atomic_t *stop)
{
    g_cfg = cfg;
    g_stop = stop;

    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
    // a waiting rescan must not starve behind a stream of requests
    pthread_rwlockattr_setkind_np(&ra, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&tree_lock, &ra);
    pthread_rwlockattr_destroy(&ra);

    wda_config(cfg);     // KBus/LocalControl from the stored settings
    wda_init();          // parameter/id tables of the wdx data layer

    // Real-time priority for the server (this thread; the client threads
    // inherit it). The KBUS cycle thread keeps its own, higher priority.
    if (cfg->http_priority > 0) {
        struct sched_param sp = { .sched_priority = cfg->http_priority };
        const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (r == 0)
            LOG_INFO("server threads: SCHED_FIFO priority %d", cfg->http_priority);
        else
            LOG_WARN("HTTP_PRIORITY %d not applied: %s (needs root / CAP_SYS_NICE)",
                     cfg->http_priority, strerror(r));
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, WORKER_STACK);

    const int want_unix = *cfg->wdx_socket != '\0';
    const int want_ctrl = *cfg->control_socket != '\0';
    const int want_tcp  = cfg->http_enable;
    if (!want_unix && !want_tcp)
        LOG_WARN("no listener: WDX_SOCKET is empty and HTTP_ENABLE=0 (only the KBUS cycle runs)");

    int ufd = -1, tfd = -1, cfd = -1, uwarn = 0, twarn = 0, cwarn = 0;
    while (!*stop) {
        // Listen; on failure retry every POLL_MS (logged once).
        if (want_unix && ufd < 0) {
            ufd = open_unix(cfg->wdx_socket, cfg->wdx_socket_mode, cfg->wdx_socket_group, 0755);
            if (ufd < 0 && !uwarn)
                LOG_ERROR("cannot listen on %s: %s (retrying)", cfg->wdx_socket,
                          errno == EADDRINUSE ? "another gateway is running" : strerror(errno));
            if (ufd >= 0)
                LOG_INFO("wdx data layer listening on unix socket %s (mode %04o)", cfg->wdx_socket,
                         (unsigned)cfg->wdx_socket_mode);
            uwarn = ufd < 0;
        }
        if (want_ctrl && cfd < 0) {
            cfd = open_unix(cfg->control_socket, 0600, "", 0700);
            if (cfd < 0 && !cwarn)
                LOG_ERROR("cannot listen on %s: %s (retrying)", cfg->control_socket,
                          errno == EADDRINUSE ? "another gateway is running" : strerror(errno));
            if (cfd >= 0)
                LOG_INFO("control socket for the dashboard: %s", cfg->control_socket);
            cwarn = cfd < 0;
        }
        if (want_tcp && tfd < 0) {
            tfd = open_tcp(cfg->http_bind, cfg->http_port);
            if (tfd < 0 && !twarn)
                LOG_ERROR("cannot listen on %s:%d: %s (retrying)", cfg->http_bind, cfg->http_port, strerror(errno));
            if (tfd >= 0)
                LOG_INFO("wdx data layer listening on %s:%d (GET /wda/parameters)", cfg->http_bind, cfg->http_port);
            twarn = tfd < 0;
        }

        struct pollfd pf[3];
        int np = 0, iu = -1, it = -1, ic = -1;
        if (ufd >= 0) { iu = np; pf[np++] = (struct pollfd){ .fd = ufd, .events = POLLIN, .revents = 0 }; }
        if (tfd >= 0) { it = np; pf[np++] = (struct pollfd){ .fd = tfd, .events = POLLIN, .revents = 0 }; }
        if (cfd >= 0) { ic = np; pf[np++] = (struct pollfd){ .fd = cfd, .events = POLLIN, .revents = 0 }; }
        const int pr = poll(pf, (nfds_t)np, POLL_MS);

        // Between requests: a pending bus rescan (KBus/Rescan, or the cycle
        // thread after persistent errors), then the new parameter tree.
        if (gateway_rescan_due()) {
            pthread_rwlock_wrlock(&tree_lock);
            gateway_rescan();
            wda_init();
            pthread_rwlock_unlock(&tree_lock);
        }
        registers_save(0);

        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        if (iu >= 0 && (pf[iu].revents & POLLIN))
            accept_one(ufd, 1, 0, &attr);
        if (it >= 0 && (pf[it].revents & POLLIN))
            accept_one(tfd, 0, 0, &attr);
        if (ic >= 0 && (pf[ic].revents & POLLIN))
            accept_one(cfd, 1, 1, &attr);
    }

    if (ufd >= 0) {
        close(ufd);
        unlink(cfg->wdx_socket);
    }
    if (tfd >= 0)
        close(tfd);
    if (cfd >= 0) {
        close(cfd);
        unlink(cfg->control_socket);
    }

    // Let running requests finish (they notice the stop flag within POLL_MS).
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&active_lock);
    while (active > 0)
        if (pthread_cond_timedwait(&active_cond, &active_lock, &until) != 0)
            break;
    pthread_mutex_unlock(&active_lock);
    pthread_attr_destroy(&attr);
    return 0;
}
