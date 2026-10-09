/* BSD sockets over the local server's WebSocket (net_web.h): the browser build's network.
 *
 * Every socket is a channel on one WebSocket to tools/webserve.py's /net, which opens the real TCP
 * or UDP socket on the player's machine, to the game server the player configured and nothing else.
 * The WebSocket lives on the page's main thread (tools/web/net.js); this side keeps each socket's
 * state and received data, so the game's threads block, poll and select here exactly as on a host
 * with sockets, and nothing waits on the main thread.
 *
 * Frames, both ways: u8 op, u32 channel (slot | generation << 16), then the op's fields, little
 * endian, addresses as a.b.c.d in a u32 with a first and ports in host order.
 *   to the server:   1 OPEN u8 kind (1 TCP, 2 UDP)   2 CONNECT u32 ip, u16 port   3 SEND data
 *                    4 SENDTO u32 ip, u16 port, data   5 CLOSE
 *   from the server: 0x81 CONNECTED u8 error (0: connected)   0x82 DATA data
 *                    0x83 DATAFROM u32 ip, u16 port, data   0x84 CLOSED u8 error (0: orderly)
 *                    0x85 DOWN (the WebSocket itself closed: every socket fails) */
#define WN_IMPL
#include "net_web.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

/* tools/web/net.js: sends one frame (malloc'd here, freed there), on the main thread */
extern void wn_js_send(uint8_t* frame, uint32_t len);

typedef struct Pkt
{
    struct Pkt* next;
    uint32_t ip, len, off;
    uint16_t port;
    uint8_t data[];
} Pkt;

enum { IDLE, CONNECTING, CONNECTED, FAILED };

typedef struct WSock
{
    int used, udp, nonblock, state, err, rd_closed;
    uint16_t gen;
    uint32_t peer_ip;
    uint16_t peer_port;
    uint32_t rcvtimeo_ms;
    Pkt *head, *tail;
    size_t queued;
} WSock;

static WSock g_s[WN_MAX];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t g_epoch; /* bumped on every arrival: waiters sleep on it */

static WSock* get(int s)
{
    int i = s - WN_BASE;
    if (i < 0 || i >= WN_MAX || !g_s[i].used)
        return NULL;
    return &g_s[i];
}
static uint32_t chan(const WSock* w) { return (uint32_t)(w - g_s) | (uint32_t)w->gen << 16; }

static double now_ms(void) { return emscripten_get_now(); }

/* Called with g_mu held; returns with it held. Waits for any arrival, or until deadline (ms, INFINITY). */
static void wait_locked(double deadline)
{
    uint32_t e = g_epoch;
    double left = deadline == INFINITY ? INFINITY : deadline - now_ms();
    if (left <= 0)
        return;
    pthread_mutex_unlock(&g_mu);
    emscripten_futex_wait(&g_epoch, e, left);
    pthread_mutex_lock(&g_mu);
}

static void notify_locked(void)
{
    __atomic_add_fetch(&g_epoch, 1, __ATOMIC_SEQ_CST);
    emscripten_futex_wake(&g_epoch, 0x7FFFFFFF);
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void frame(uint8_t op, uint32_t ch, const void* head, uint32_t headn, const void* data, uint32_t n)
{
    uint8_t* f = (uint8_t*)malloc(5 + headn + n);
    if (!f)
        return;
    f[0] = op;
    put32(f + 1, ch);
    if (headn)
        memcpy(f + 5, head, headn);
    if (n)
        memcpy(f + 5 + headn, data, n);
    wn_js_send(f, 5 + headn + n);
}

static void addr_head(uint8_t h[6], uint32_t ip_be, uint16_t port)
{
    const uint8_t* b = (const uint8_t*)&ip_be; /* network order: a first */
    memcpy(h, b, 4);
    h[4] = (uint8_t)port, h[5] = (uint8_t)(port >> 8);
}

static void free_queue(WSock* w)
{
    for (Pkt* p = w->head; p;)
    {
        Pkt* n = p->next;
        free(p);
        p = n;
    }
    w->head = w->tail = NULL;
    w->queued = 0;
}

int wn_socket(int domain, int type, int protocol)
{
    (void)protocol;
    type &= 0xF; /* SOCK_NONBLOCK / SOCK_CLOEXEC */
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM))
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    int i = 0;
    while (i < WN_MAX && g_s[i].used)
        ++i;
    if (i == WN_MAX)
    {
        pthread_mutex_unlock(&g_mu);
        errno = EMFILE;
        return -1;
    }
    WSock* w = &g_s[i];
    uint16_t gen = (uint16_t)(w->gen + 1);
    memset(w, 0, sizeof *w);
    w->used = 1, w->gen = gen, w->udp = type == SOCK_DGRAM;
    w->state = w->udp ? CONNECTED : IDLE;
    uint8_t kind = w->udp ? 2 : 1;
    frame(1, chan(w), &kind, 1, NULL, 0);
    pthread_mutex_unlock(&g_mu);
    return WN_BASE + i;
}

int wn_close(int s)
{
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    if (!w)
    {
        pthread_mutex_unlock(&g_mu);
        errno = EBADF;
        return -1;
    }
    frame(5, chan(w), NULL, 0, NULL, 0);
    free_queue(w);
    w->used = 0;
    notify_locked(); /* a select on it ends */
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int wn_bind(int s, const struct sockaddr* a, socklen_t len)
{
    (void)a, (void)len; /* the server's socket takes any local port: replies come back on it */
    return get(s) ? 0 : (errno = EBADF, -1);
}

int wn_listen(int s, int backlog)
{
    (void)s, (void)backlog;
    errno = EOPNOTSUPP; /* the game only connects out */
    return -1;
}

int wn_accept(int s, struct sockaddr* a, socklen_t* len)
{
    (void)s, (void)a, (void)len;
    errno = EOPNOTSUPP;
    return -1;
}

int wn_connect(int s, const struct sockaddr* a, socklen_t len)
{
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    if (!a || len < sizeof *in || in->sin_family != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    int r = 0;
    if (!w)
        errno = EBADF, r = -1;
    else if (w->udp) /* a default peer only */
        w->peer_ip = in->sin_addr.s_addr, w->peer_port = ntohs(in->sin_port);
    else if (w->state == CONNECTED)
        errno = EISCONN, r = -1;
    else if (w->state == CONNECTING)
        errno = EALREADY, r = -1;
    else
    {
        w->peer_ip = in->sin_addr.s_addr, w->peer_port = ntohs(in->sin_port);
        w->state = CONNECTING;
        uint8_t h[6];
        addr_head(h, w->peer_ip, w->peer_port);
        frame(2, chan(w), h, 6, NULL, 0);
        if (w->nonblock)
            errno = EINPROGRESS, r = -1;
        else
        {
            uint32_t ch = chan(w);
            while (w->used && chan(w) == ch && w->state == CONNECTING)
                wait_locked(INFINITY);
            if (!w->used || chan(w) != ch)
                errno = EBADF, r = -1;
            else if (w->state != CONNECTED)
                errno = w->err ? w->err : ECONNREFUSED, w->err = 0, r = -1;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static ssize_t send_locked(WSock* w, const void* buf, size_t n, uint32_t ip, uint16_t port, int to)
{
    if (w->udp)
    {
        if (!to)
            ip = w->peer_ip, port = w->peer_port;
        if (!port)
        {
            errno = EDESTADDRREQ;
            return -1;
        }
        uint8_t h[6];
        addr_head(h, ip, port);
        frame(4, chan(w), h, 6, buf, (uint32_t)n);
        return (ssize_t)n;
    }
    if (w->state != CONNECTED)
    {
        errno = w->state == FAILED ? (w->err ? w->err : ECONNRESET) : ENOTCONN;
        return -1;
    }
    frame(3, chan(w), NULL, 0, buf, (uint32_t)n); /* the server buffers: a send never blocks */
    return (ssize_t)n;
}

ssize_t wn_send(int s, const void* buf, size_t n, int flags)
{
    (void)flags;
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    ssize_t r = w ? send_locked(w, buf, n, 0, 0, 0) : (errno = EBADF, -1);
    pthread_mutex_unlock(&g_mu);
    return r;
}

ssize_t wn_sendto(int s, const void* buf, size_t n, int flags, const struct sockaddr* a, socklen_t len)
{
    (void)flags;
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    ssize_t r;
    if (!w)
        errno = EBADF, r = -1;
    else if (in && len >= sizeof *in)
        r = send_locked(w, buf, n, in->sin_addr.s_addr, ntohs(in->sin_port), 1);
    else
        r = send_locked(w, buf, n, 0, 0, 0);
    pthread_mutex_unlock(&g_mu);
    return r;
}

ssize_t wn_recvfrom(int s, void* buf, size_t n, int flags, struct sockaddr* a, socklen_t* len)
{
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    if (!w)
    {
        pthread_mutex_unlock(&g_mu);
        errno = EBADF;
        return -1;
    }
    uint32_t ch = chan(w);
    double deadline = w->rcvtimeo_ms ? now_ms() + w->rcvtimeo_ms : INFINITY;
    int nonblock = w->nonblock || (flags & MSG_DONTWAIT);
    for (;;)
    {
        if (!w->used || chan(w) != ch)
        {
            pthread_mutex_unlock(&g_mu);
            errno = EBADF;
            return -1;
        }
        if (w->head)
            break;
        if (w->state == FAILED || w->err)
        {
            int e = w->err ? w->err : ECONNRESET;
            w->err = 0;
            pthread_mutex_unlock(&g_mu);
            errno = e;
            return -1;
        }
        if (w->rd_closed)
        {
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
        if (nonblock || now_ms() >= deadline)
        {
            pthread_mutex_unlock(&g_mu);
            errno = EAGAIN;
            return -1;
        }
        wait_locked(deadline);
    }
    size_t got = 0;
    Pkt* p = w->head;
    if (a && len && *len >= sizeof(struct sockaddr_in))
    {
        struct sockaddr_in* in = (struct sockaddr_in*)a;
        memset(in, 0, sizeof *in);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = w->udp ? p->ip : w->peer_ip;
        in->sin_port = htons(w->udp ? p->port : w->peer_port);
        *len = sizeof *in;
    }
    if (w->udp)
    {
        got = p->len < n ? p->len : n; /* the rest of a datagram too big for the buffer is lost */
        memcpy(buf, p->data, got);
        if (!(flags & MSG_PEEK))
        {
            w->head = p->next;
            if (!w->head)
                w->tail = NULL;
            w->queued -= p->len;
            free(p);
        }
    }
    else
    {
        for (Pkt* q = p; q && got < n; q = q->next)
        {
            size_t c = q->len - q->off < n - got ? q->len - q->off : n - got;
            memcpy((uint8_t*)buf + got, q->data + q->off, c);
            got += c;
        }
        if (!(flags & MSG_PEEK))
        {
            size_t left = got;
            w->queued -= got;
            while (left)
            {
                Pkt* q = w->head;
                size_t c = q->len - q->off < left ? q->len - q->off : left;
                q->off += (uint32_t)c, left -= c;
                if (q->off == q->len)
                {
                    w->head = q->next;
                    if (!w->head)
                        w->tail = NULL;
                    free(q);
                }
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return (ssize_t)got;
}

ssize_t wn_recv(int s, void* buf, size_t n, int flags) { return wn_recvfrom(s, buf, n, flags, NULL, NULL); }

int wn_shutdown(int s, int how)
{
    (void)how;
    return get(s) ? 0 : (errno = EBADF, -1);
}

/* readable: data, an orderly close or an error; writable: connected (or a connect that failed, as BSD
 * reports it); except: a failed connect */
static int ready(const WSock* w, int which)
{
    if (which == 0)
        return w->head || w->rd_closed || w->state == FAILED || w->err;
    if (which == 1)
        return w->state == CONNECTED || w->state == FAILED;
    return w->state == FAILED;
}

int wn_select(int nfds, fd_set* rd, fd_set* wr, fd_set* ex, struct timeval* tv)
{
    fd_set* sets[3] = { rd, wr, ex };
    fd_set in[3];
    for (int k = 0; k < 3; ++k)
        if (sets[k])
            in[k] = *sets[k];
    double deadline = tv ? now_ms() + tv->tv_sec * 1000.0 + tv->tv_usec / 1000.0 : INFINITY;
    pthread_mutex_lock(&g_mu);
    for (;;)
    {
        int count = 0;
        for (int k = 0; k < 3; ++k)
        {
            if (!sets[k])
                continue;
            FD_ZERO(sets[k]);
            for (int fd = WN_BASE; fd < nfds && fd < WN_BASE + WN_MAX; ++fd)
            {
                if (!FD_ISSET(fd, &in[k]))
                    continue;
                WSock* w = get(fd);
                if (!w || ready(w, k))
                {
                    FD_SET(fd, sets[k]);
                    ++count;
                }
            }
        }
        if (count || now_ms() >= deadline)
        {
            pthread_mutex_unlock(&g_mu);
            return count;
        }
        wait_locked(deadline);
    }
}

int wn_fcntl(int s, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int arg = cmd == F_SETFL ? va_arg(ap, int) : 0;
    va_end(ap);
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    int r = 0;
    if (!w)
        errno = EBADF, r = -1;
    else if (cmd == F_GETFL)
        r = O_RDWR | (w->nonblock ? O_NONBLOCK : 0);
    else if (cmd == F_SETFL)
        w->nonblock = (arg & O_NONBLOCK) != 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}

int wn_ioctl(int s, int req, ...)
{
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    int r = 0;
    if (!w)
        errno = EBADF, r = -1;
    else if (req == FIONREAD)
        *(int*)arg = (int)(w->udp ? (w->head ? w->head->len : 0) : w->queued);
    else if (req == FIONBIO)
        w->nonblock = *(int*)arg != 0;
    else
        errno = EINVAL, r = -1;
    pthread_mutex_unlock(&g_mu);
    return r;
}

int wn_setsockopt(int s, int level, int name, const void* v, socklen_t len)
{
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    if (w && level == SOL_SOCKET && name == SO_RCVTIMEO && v && len >= sizeof(struct timeval))
    {
        const struct timeval* t = (const struct timeval*)v;
        w->rcvtimeo_ms = (uint32_t)(t->tv_sec * 1000 + t->tv_usec / 1000);
    }
    pthread_mutex_unlock(&g_mu);
    return w ? 0 : (errno = EBADF, -1); /* the rest (buffers, no-delay, keepalive) are the server socket's */
}

int wn_getsockopt(int s, int level, int name, void* v, socklen_t* len)
{
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    int r = 0;
    if (!w)
        errno = EBADF, r = -1;
    else if (v && len && *len >= sizeof(int))
    {
        int val = 0;
        if (level == SOL_SOCKET && name == SO_ERROR)
            val = w->err ? w->err : w->state == FAILED ? ECONNREFUSED : 0, w->err = 0;
        else if (level == SOL_SOCKET && name == SO_TYPE)
            val = w->udp ? SOCK_DGRAM : SOCK_STREAM;
        *(int*)v = val;
        *len = sizeof(int);
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

int wn_getpeername(int s, struct sockaddr* a, socklen_t* len)
{
    pthread_mutex_lock(&g_mu);
    WSock* w = get(s);
    int r = -1;
    if (!w)
        errno = EBADF;
    else if (!w->peer_port || (!w->udp && w->state != CONNECTED))
        errno = ENOTCONN;
    else if (a && len && *len >= sizeof(struct sockaddr_in))
    {
        struct sockaddr_in* in = (struct sockaddr_in*)a;
        memset(in, 0, sizeof *in);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = w->peer_ip;
        in->sin_port = htons(w->peer_port);
        *len = sizeof *in;
        r = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

int wn_getsockname(int s, struct sockaddr* a, socklen_t* len)
{
    if (!get(s))
        return errno = EBADF, -1;
    if (a && len && *len >= sizeof(struct sockaddr_in))
    {
        struct sockaddr_in* in = (struct sockaddr_in*)a;
        memset(in, 0, sizeof *in);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(0x7F000001u);
        in->sin_port = htons((uint16_t)s);
        *len = sizeof *in;
    }
    return 0;
}

/* From tools/web/net.js, on the main thread: one frame from the server, malloc'd there, freed here. */
EMSCRIPTEN_KEEPALIVE void wn_deliver(uint8_t* f, uint32_t n)
{
    if (n < 5)
    {
        free(f);
        return;
    }
    uint8_t op = f[0];
    uint32_t ch = get32(f + 1), slot = ch & 0xFFFF;
    pthread_mutex_lock(&g_mu);
    if (op == 0x85)
    {
        for (int i = 0; i < WN_MAX; ++i)
            if (g_s[i].used)
                g_s[i].err = ENETDOWN, g_s[i].state = g_s[i].state == CONNECTING ? FAILED : g_s[i].state, g_s[i].rd_closed = 1;
    }
    else if (slot < WN_MAX && g_s[slot].used && g_s[slot].gen == ch >> 16)
    {
        WSock* w = &g_s[slot];
        if (op == 0x81 && n >= 6)
        {
            if (f[5])
                w->state = FAILED, w->err = ECONNREFUSED;
            else
                w->state = CONNECTED;
        }
        else if ((op == 0x82 && n > 5) || (op == 0x83 && n >= 11))
        {
            uint32_t at = op == 0x83 ? 11 : 5, len = n - at;
            Pkt* p = (Pkt*)malloc(sizeof *p + len);
            if (p)
            {
                p->next = NULL, p->len = len, p->off = 0, p->ip = 0, p->port = 0;
                if (op == 0x83)
                    memcpy(&p->ip, f + 5, 4), p->port = (uint16_t)(f[9] | f[10] << 8);
                memcpy(p->data, f + at, len);
                if (w->tail)
                    w->tail->next = p;
                else
                    w->head = p;
                w->tail = p;
                w->queued += len;
            }
        }
        else if (op == 0x84)
        {
            w->rd_closed = 1;
            if (n >= 6 && f[5])
                w->err = ECONNRESET;
            if (w->state == CONNECTING)
                w->state = FAILED;
        }
    }
    notify_locked();
    pthread_mutex_unlock(&g_mu);
    free(f);
}
