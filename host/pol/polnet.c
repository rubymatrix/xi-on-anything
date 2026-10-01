/* Blocking TCP for the POL client. See polnet.h. */
#include "polnet.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET RawSocket;
#define CLOSE closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int RawSocket;
#define CLOSE close
#endif

int pol_net_init(void)
{
#if defined(_WIN32)
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return 1;
#endif
}

/* readable (or writable) within ms: 1, timeout 0, error -1 */
static int wait_fd(RawSocket s, int write, int ms)
{
#if defined(_WIN32)
    WSAPOLLFD p = { s, (SHORT)(write ? POLLWRNORM : POLLRDNORM), 0 };
    int r = WSAPoll(&p, 1, ms);
#else
    struct pollfd p = { s, (short)(write ? POLLOUT : POLLIN), 0 };
    int r = poll(&p, 1, ms);
#endif
    return r < 0 ? -1 : r;
}

static void set_blocking(RawSocket s, int blocking)
{
#if defined(_WIN32)
    u_long nb = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

PolSocket pol_tcp_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%u", port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res)
        return POL_NO_SOCKET;
    RawSocket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
#if defined(_WIN32)
    if (s == INVALID_SOCKET)
#else
    if (s < 0)
#endif
    {
        freeaddrinfo(res);
        return POL_NO_SOCKET;
    }
    set_blocking(s, 0);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0)
    {
        int err = 0;
        socklen_t len = sizeof err;
        if (wait_fd(s, 1, timeout_ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err)
        {
            CLOSE(s);
            return POL_NO_SOCKET;
        }
    }
    set_blocking(s, 1);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    return (PolSocket)s;
}

int pol_send_all(PolSocket s, const void* data, size_t n)
{
    const char* p = (const char*)data;
    while (n)
    {
        int k = (int)send((RawSocket)s, p, (int)n, 0);
        if (k <= 0)
            return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

int pol_recv_some(PolSocket s, void* data, size_t n, int timeout_ms)
{
    int w = wait_fd((RawSocket)s, 0, timeout_ms);
    if (w == 0)
        return 0;
    if (w < 0)
        return -1;
    int k = (int)recv((RawSocket)s, (char*)data, (int)n, 0);
    return k > 0 ? k : -1;
}

int pol_recv_exact(PolSocket s, void* data, size_t n, int timeout_ms)
{
    char* p = (char*)data;
    while (n)
    {
        int k = pol_recv_some(s, p, n, timeout_ms);
        if (k <= 0)
            return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

void pol_close(PolSocket s)
{
    if (s != POL_NO_SOCKET)
        CLOSE((RawSocket)s);
}
