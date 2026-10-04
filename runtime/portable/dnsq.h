/* One DNS A query to a named DNS server (host64 --dns), for a server whose DNS answers the game's
 * names with its own addresses. Header-only: include after the socket headers (winsock2.h, or
 * sys/socket.h, netinet/in.h and sys/select.h). */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* past one name in a DNS message: labels, ending in a zero or a compression pointer */
static size_t dnsq_skip_name(const uint8_t* m, size_t len, size_t off)
{
    while (off < len)
    {
        uint8_t b = m[off];
        if (b == 0)
            return off + 1;
        if ((b & 0xC0) == 0xC0)
            return off + 2;
        off += 1 + (size_t)b;
    }
    return len + 1;
}

/* name's first IPv4 address (host byte order) as `server` (IPv4, host byte order, port 53) answers
 * it; 0 when it has none or does not answer within two tries of two seconds. */
static int dnsq_a(uint32_t server, const char* name, uint32_t* out)
{
    uint8_t q[300], r[1500];
    size_t n = 12;
    uint16_t id = (uint16_t)(rand() ^ (uintptr_t)name);
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8), q[1] = (uint8_t)id;
    q[2] = 1; /* recursion desired */
    q[5] = 1; /* one question */
    for (const char* p = name; *p;)
    {
        const char* dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l > 63 || n + l + 6 > sizeof q)
            return 0;
        if (l)
        {
            q[n++] = (uint8_t)l;
            memcpy(q + n, p, l);
            n += l;
        }
        if (!dot)
            break;
        p = dot + 1;
    }
    q[n++] = 0;
    q[n++] = 0, q[n++] = 1; /* A */
    q[n++] = 0, q[n++] = 1; /* IN */

#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return 0;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return 0;
#endif
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(server);
    int found = 0;
    for (int attempt = 0; attempt < 2 && !found; ++attempt)
    {
        if (sendto(s, (const char*)q, (int)n, 0, (struct sockaddr*)&to, sizeof to) != (int)n)
            break;
        for (;;)
        {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            struct timeval tv = { 2, 0 };
            if (select((int)s + 1, &rd, NULL, NULL, &tv) != 1)
                break;
            int len = (int)recv(s, (char*)r, sizeof r, 0);
            if (len < 12 || r[0] != q[0] || r[1] != q[1] || !(r[2] & 0x80))
                continue; /* not our answer */
            if ((r[3] & 15) != 0)
                goto done; /* the server says no */
            unsigned qd = (unsigned)r[4] << 8 | r[5], an = (unsigned)r[6] << 8 | r[7];
            size_t off = 12;
            for (unsigned i = 0; i < qd; ++i)
                off = dnsq_skip_name(r, (size_t)len, off) + 4;
            for (unsigned i = 0; i < an && off <= (size_t)len; ++i)
            {
                off = dnsq_skip_name(r, (size_t)len, off);
                if (off + 10 > (size_t)len)
                    break;
                unsigned type = (unsigned)r[off] << 8 | r[off + 1];
                unsigned rdlen = (unsigned)r[off + 8] << 8 | r[off + 9];
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= (size_t)len)
                {
                    *out = (uint32_t)r[off] << 24 | (uint32_t)r[off + 1] << 16 | (uint32_t)r[off + 2] << 8 | r[off + 3];
                    found = 1;
                    break;
                }
                off += rdlen;
            }
            goto done;
        }
    }
done:
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return found;
}
