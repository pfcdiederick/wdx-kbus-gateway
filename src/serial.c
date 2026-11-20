// Serial interface COM1: transparent forwarding (see serial.h).
#include "serial.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define REOPEN_MS   2000
#define WRITE_MS    2000

static struct {
    pthread_mutex_t lock;        // everything below
    pthread_mutex_t tx_lock;     // one transmit at a time
    pthread_t       thread;
    volatile int    stop;
    int             running;
    const gw_config_t *cfg;
    int             fd;
    uint32_t        baud;
    char            format[4];
    uint32_t        gap_ms;
    uint64_t        tx_count, rx_count, frame_count, err_base;
    uint8_t         ring[SERIAL_BUF];
    size_t          ring_pos, ring_len;
    uint8_t         cur[SERIAL_BUF];          // frame being received
    size_t          cur_len;
    int64_t         last_rx_ms;
    uint8_t         frame[SERIAL_BUF];
    size_t          frame_len;
} sp = { .lock = PTHREAD_MUTEX_INITIALIZER, .tx_lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static const struct { long baud; speed_t code; } bauds[] = {
    { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 },
    { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 }, { 230400, B230400 },
};

int serial_valid_baud(long baud)
{
    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); ++i)
        if (bauds[i].baud == baud)
            return 0;
    return -1;
}

// "8E1": data bits 5..8, parity N/E/O, stop bits 1/2
int serial_valid_format(const char *f, size_t n)
{
    if (n != 3 || f[0] < '5' || f[0] > '8' || !strchr("NEO", f[1]) || (f[2] != '1' && f[2] != '2'))
        return -1;
    return 0;
}

// Apply sp.baud / sp.format to the open port (lock held).
static int apply_termios(void)
{
    if (sp.fd < 0)
        return 0;
    struct termios t;
    if (tcgetattr(sp.fd, &t) != 0)
        return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= (sp.format[0] == '5') ? CS5 : (sp.format[0] == '6') ? CS6 : (sp.format[0] == '7') ? CS7 : CS8;
    if (sp.format[1] != 'N')
        t.c_cflag |= PARENB | (sp.format[1] == 'O' ? PARODD : 0);
    if (sp.format[2] == '2')
        t.c_cflag |= CSTOPB;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    speed_t code = B19200;
    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); ++i)
        if (bauds[i].baud == (long)sp.baud)
            code = bauds[i].code;
    cfsetispeed(&t, code);
    cfsetospeed(&t, code);
    return tcsetattr(sp.fd, TCSANOW, &t);
}

int serial_configure(uint32_t baud, const char *fmt, uint32_t gap_ms)
{
    pthread_mutex_lock(&sp.lock);
    if (baud)
        sp.baud = baud;
    if (fmt)
        memcpy(sp.format, fmt, 3), sp.format[3] = '\0';
    if (gap_ms)
        sp.gap_ms = gap_ms;
    const int rc = apply_termios();
    pthread_mutex_unlock(&sp.lock);
    if (rc != 0)
        LOG_WARN("COM1: settings %u %s not applied: %s", (unsigned)sp.baud, sp.format, strerror(errno));
    return rc;
}

// ---------------------------------------------------------------------------
// Receive thread
// ---------------------------------------------------------------------------
static uint64_t line_errors(int fd)
{
    struct serial_icounter_struct ic;
    if (fd < 0 || ioctl(fd, TIOCGICOUNT, &ic) != 0)
        return 0;
    return (uint64_t)ic.frame + (uint64_t)ic.parity + (uint64_t)ic.overrun + (uint64_t)ic.buf_overrun;
}

static void open_port(void)
{
    const int fd = open(sp.cfg->serial_device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    if (ioctl(fd, TIOCEXCL) != 0)                         // nobody else (e.g. CODESYS) at the same time
        LOG_WARN("COM1: exclusive access not set: %s", strerror(errno));
    pthread_mutex_lock(&sp.lock);
    sp.fd = fd;
    const int rc = apply_termios();
    tcflush(fd, TCIOFLUSH);
    sp.err_base = line_errors(fd);
    sp.cur_len = 0;
    pthread_mutex_unlock(&sp.lock);
    if (rc != 0)
        LOG_WARN("COM1: settings not applied: %s", strerror(errno));
    LOG_INFO("COM1 %s open: %u %s, frame gap %u ms", sp.cfg->serial_device, (unsigned)sp.baud, sp.format,
             (unsigned)sp.gap_ms);
}

static void close_port(const char *why)
{
    pthread_mutex_lock(&sp.lock);
    if (sp.fd >= 0) {
        sp.err_base = 0;
        close(sp.fd);
        sp.fd = -1;
    }
    pthread_mutex_unlock(&sp.lock);
    LOG_WARN("COM1 closed: %s (reopening)", why);
}

// A pause of gap_ms ends the frame being received (lock held).
static void end_frame(void)
{
    if (sp.cur_len == 0)
        return;
    memcpy(sp.frame, sp.cur, sp.cur_len);
    sp.frame_len = sp.cur_len;
    sp.frame_count++;
    sp.cur_len = 0;
}

static void *rx_thread(void *arg)
{
    (void)arg;
    int64_t next_open = 0, warned = 0;
    uint8_t buf[256];
    while (!sp.stop) {
        if (sp.fd < 0) {
            if (now_ms() >= next_open) {
                open_port();
                next_open = now_ms() + REOPEN_MS;
                if (sp.fd < 0 && !warned) {
                    LOG_WARN("COM1: cannot open %s: %s (retrying; WBM serial owner must be None)",
                             sp.cfg->serial_device, strerror(errno));
                    warned = 1;
                }
                if (sp.fd >= 0)
                    warned = 0;
            }
            if (sp.fd < 0) {
                usleep(100 * 1000);
                continue;
            }
        }
        pthread_mutex_lock(&sp.lock);
        const int pending = sp.cur_len > 0;
        const int64_t due = sp.last_rx_ms + sp.gap_ms;
        const int fd = sp.fd;
        pthread_mutex_unlock(&sp.lock);
        int timeout = 200;
        if (pending) {
            const int64_t left = due - now_ms();
            timeout = left < 0 ? 0 : (int)left;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        const int r = poll(&p, 1, timeout);
        if (r < 0 && errno != EINTR) {
            close_port(strerror(errno));
            continue;
        }
        if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            close_port("device error");
            continue;
        }
        if (r > 0 && (p.revents & POLLIN)) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                close_port(strerror(errno));
                continue;
            }
            if (n > 0) {
                pthread_mutex_lock(&sp.lock);
                for (ssize_t i = 0; i < n; ++i) {
                    sp.ring[(sp.ring_pos + sp.ring_len) % SERIAL_BUF] = buf[i];
                    if (sp.ring_len < SERIAL_BUF)
                        sp.ring_len++;
                    else
                        sp.ring_pos = (sp.ring_pos + 1) % SERIAL_BUF;
                    if (sp.cur_len == SERIAL_BUF)             // longer than the buffer: split
                        end_frame();
                    sp.cur[sp.cur_len++] = buf[i];
                }
                sp.rx_count += (uint64_t)n;
                sp.last_rx_ms = now_ms();
                pthread_mutex_unlock(&sp.lock);
            }
            continue;
        }
        pthread_mutex_lock(&sp.lock);                     // timeout: frame complete?
        if (sp.cur_len > 0 && now_ms() - sp.last_rx_ms >= (int64_t)sp.gap_ms)
            end_frame();
        pthread_mutex_unlock(&sp.lock);
    }
    return NULL;
}

int serial_start(const gw_config_t *cfg)
{
    sp.cfg = cfg;
    if (!*cfg->serial_device)
        return 0;
    sp.baud = (uint32_t)cfg->serial_baud;
    memcpy(sp.format, cfg->serial_format, 3);
    sp.format[3] = '\0';
    sp.gap_ms = (uint32_t)cfg->serial_frame_gap_ms;
    sp.stop = 0;
    if (pthread_create(&sp.thread, NULL, rx_thread, NULL) != 0) {
        LOG_ERROR("COM1: no receive thread");
        return -1;
    }
    sp.running = 1;
    return 0;
}

void serial_stop(void)
{
    if (!sp.running)
        return;
    sp.stop = 1;
    pthread_join(sp.thread, NULL);
    sp.running = 0;
    if (sp.fd >= 0) {
        close(sp.fd);
        sp.fd = -1;
    }
}

int serial_enabled(void) { return sp.cfg && *sp.cfg->serial_device; }

void serial_snapshot(serial_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled = serial_enabled();
    if (!s->enabled)
        return;
    pthread_mutex_lock(&sp.lock);
    s->open = sp.fd >= 0;
    s->baud = sp.baud;
    memcpy(s->format, sp.format, sizeof(s->format));
    s->frame_gap_ms = sp.gap_ms;
    s->tx_count = sp.tx_count;
    s->rx_count = sp.rx_count;
    s->frame_count = sp.frame_count;
    const uint64_t e = line_errors(sp.fd);
    s->errors = e >= sp.err_base ? e - sp.err_base : 0;
    for (size_t i = 0; i < sp.ring_len; ++i)
        s->rx[i] = sp.ring[(sp.ring_pos + i) % SERIAL_BUF];
    s->rx_len = sp.ring_len;
    memcpy(s->frame, sp.frame, sp.frame_len);
    s->frame_len = sp.frame_len;
    pthread_mutex_unlock(&sp.lock);
}

int serial_write(const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&sp.tx_lock);
    pthread_mutex_lock(&sp.lock);
    const int fd = sp.fd;
    pthread_mutex_unlock(&sp.lock);
    int rc = fd < 0 ? -1 : 0;
    size_t done = 0;
    const int64_t deadline = now_ms() + WRITE_MS;
    while (rc == 0 && done < len) {
        const ssize_t w = write(fd, data + done, len - done);
        if (w > 0) {
            done += (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            const int64_t left = deadline - now_ms();
            struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0)
                rc = -1;
        } else {
            rc = -1;
        }
    }
    pthread_mutex_lock(&sp.lock);
    sp.tx_count += done;
    pthread_mutex_unlock(&sp.lock);
    pthread_mutex_unlock(&sp.tx_lock);
    return rc;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
static int hex4(const char *s, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    return 0;
}

int serial_parse_bytes(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len)
{
    while (n && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
        s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        n--;
    if (n < 2 || s[0] != '"' || s[n - 1] != '"')
        return 400;
    size_t o = 0;
    for (size_t i = 1; i < n - 1;) {
        unsigned cp;
        const unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 >= n - 1)
                return 400;
            const char e = s[i + 1];
            i += 2;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u':
                if (i + 4 > n - 1 || hex4(s + i, &cp) != 0)
                    return 400;
                i += 4;
                break;
            default:
                return 400;
            }
        } else if (c < 0x80) {
            if (c < 0x20)
                return 400;                               // raw control character: not JSON
            cp = c;
            i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n - 1 && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);   // UTF-8, 2 bytes
            i += 2;
        } else {
            return 409;                                   // 3/4-byte UTF-8: above U+00FF
        }
        if (cp > 0xFF || o >= cap)
            return 409;
        out[o++] = (uint8_t)cp;
    }
    *len = o;
    return 0;
}

size_t serial_format_bytes(const uint8_t *d, size_t len, char *buf, size_t cap)
{
    size_t o = 0;
    if (cap < 3)
        return 0;
    buf[o++] = '"';
    for (size_t i = 0; i < len && o + 8 < cap; ++i) {
        if (d[i] >= 0x20 && d[i] < 0x7F && d[i] != '"' && d[i] != '\\')
            buf[o++] = (char)d[i];
        else
            o += (size_t)snprintf(buf + o, cap - o, "\\u%04x", d[i]);
    }
    buf[o++] = '"';
    buf[o] = '\0';
    return o;
}
