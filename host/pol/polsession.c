/* The host's PlayOnline session. See polsession.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "polsession.h"

#define TIMEOUT_MS 10000

static int fail(PolSession* s, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error, sizeof s->error, fmt, ap);
    va_end(ap);
    return 0;
}

/* --- PlayOnline ID <-> nick (host __sqIrcScramblePolId) ------------------ */
static const char DIGITS[] = "EFKAOYMJVNGTDSWBQLPCIRHZXU6328401795";

int pol_nick_from_id(const char* pol_id, char nick[10])
{
    if (strlen(pol_id) != 8)
        return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
        const char* p = strchr(DIGITS, pol_id[i]);
        if (!p || !pol_id[i])
            return 0;
        v = v * 36 + (uint64_t)(p - DIGITS);
    }
    v |= (uint64_t)0xC000 << 32;
    for (int i = 0; i < 5; ++i)
    {
        int shift = (4 - i) * 8;
        v ^= (v >> 8) & ((uint64_t)0xFF << shift);
    }
    v &= 0xFFFF3FFFFFFFFFFFull;
    nick[0] = 'U';
    for (int i = 8; i >= 1; --i)
    {
        nick[i] = DIGITS[v % 36];
        v /= 36;
    }
    nick[9] = 0;
    return 1;
}

/* --- sqIrc lines ------------------------------------------------------------------------------------ */

/* One framed line, encrypted once the session key exists. */
static int send_line(PolSession* s, const char* content)
{
    uint8_t raw[1024];
    size_t n = strlen(content);
    if (n + 6 > sizeof raw)
        return 0;
    memcpy(raw, content, n);
    pol_chk4(raw, n, raw + n);
    raw[n + 4] = '\r';
    raw[n + 5] = '\n';
    n += 6;
    if (s->encrypted)
        pol_line_crypt(&s->bf, s->iv, raw, raw, n);
    return pol_send_all(s->irc, raw, n);
}

/* The next line's content (no checksum, no CRLF), decrypted and verified once encrypted.
 * 1 = a line, 0 = timeout, -1 = closed. */
static int recv_line(PolSession* s, char* out, size_t cap, int timeout_ms)
{
    for (;;)
    {
        uint8_t* nl = memchr(s->inbuf, '\n', s->inlen);
        if (nl)
        {
            size_t len = (size_t)(nl - s->inbuf) + 1;
            uint8_t line[4096];
            memcpy(line, s->inbuf, len);
            memmove(s->inbuf, s->inbuf + len, s->inlen - len);
            s->inlen -= len;
            if (s->encrypted)
                pol_line_crypt(&s->bf, s->iv, line, line, len);
            size_t body = len;
            while (body && (line[body - 1] == '\n' || line[body - 1] == '\r'))
                body--;
            if (body < 4)
                continue;
            uint8_t chk[4];
            pol_chk4(line, body - 4, chk);
            if (memcmp(chk, line + body - 4, 4))
                continue; /* a line that fails its checksum is dropped, as the host does */
            body -= 4;
            if (body >= cap)
                body = cap - 1;
            memcpy(out, line, body);
            out[body] = 0;
            return 1;
        }
        if (s->inlen == sizeof s->inbuf)
            s->inlen = 0; /* an overlong line: drop it */
        int k = pol_recv_some(s->irc, s->inbuf + s->inlen, sizeof s->inbuf - s->inlen, timeout_ms);
        if (k == 0)
            return 0;
        if (k < 0)
            return -1;
        s->inlen += (size_t)k;
    }
}

/* the last space-separated field of a line */
static const char* last_field(const char* line)
{
    const char* sp = strrchr(line, ' ');
    return sp ? sp + 1 : line;
}

int pol_signin(PolSession* s, const char* host, uint16_t irc_port, const char* pol_id, const char* password)
{
    memset(s, 0, sizeof *s);
    s->irc = POL_NO_SOCKET;
    snprintf(s->host, sizeof s->host, "%s", host);
    if (!pol_nick_from_id(pol_id, s->nick))
        return fail(s, "%s is not a PlayOnline ID (8 characters, A-Z and 0-9)", pol_id);
    snprintf(s->pol_id, sizeof s->pol_id, "%s", pol_id);
    pol_net_init();
    s->irc = pol_tcp_connect(host, irc_port, TIMEOUT_MS);
    if (s->irc == POL_NO_SOCKET)
        return fail(s, "cannot connect to %s:%u", host, irc_port);

    char line[1024], token[256];
    if (recv_line(s, line, sizeof line, TIMEOUT_MS) != 1 || !strstr(line, " NOTICE AUTH "))
        return fail(s, "no NOTICE AUTH greeting");
    snprintf(token, sizeof token, "%s", last_field(line));

    /* USER x 8 * :<our RSA-256 modulus, custom base64 of its little-endian bytes> */
    char modulus[64], user[128];
    pol_b64enc(POL_RSA_N, 32, modulus);
    snprintf(user, sizeof user, "USER x 8 * :%s", modulus);
    if (!send_line(s, user))
        return fail(s, "send failed");

    /* :<host> 300 x <custom base64 of RSA(session key)> - still plaintext */
    if (recv_line(s, line, sizeof line, TIMEOUT_MS) != 1 || !strstr(line, " 300 "))
        return fail(s, "no session key (numeric 300)");
    uint8_t cipher[40], key[8];
    const char* b = last_field(line);
    if (pol_b64dec(b, strlen(b), cipher) != 32 || !pol_rsa_session_key(cipher, key))
        return fail(s, "the session key did not decrypt");
    pol_bf_init(&s->bf, key, 8);
    s->iv = pol_rsa_iv();
    s->encrypted = 1;

    /* NICK <nick>:<md5 hex of token + password>:<blob>. The server reads the nick; the blob is
     * opaque retail data (we send "x"). */
    uint8_t md[16], buf[512];
    size_t tl = strlen(token), pl = strlen(password);
    if (tl + pl > sizeof buf)
        return fail(s, "password too long");
    memcpy(buf, token, tl);
    memcpy(buf + tl, password, pl);
    pol_md5(buf, tl + pl, md);
    char nick_line[160];
    int o = snprintf(nick_line, sizeof nick_line, "NICK %s:", s->nick);
    for (int i = 0; i < 16; ++i)
        o += snprintf(nick_line + o, sizeof nick_line - (size_t)o, "%02x", md[i]);
    snprintf(nick_line + o, sizeof nick_line - (size_t)o, ":x");
    if (!send_line(s, nick_line))
        return fail(s, "send failed");

    /* signed in once the server answers the nick (422, no MOTD) */
    for (;;)
    {
        int r = recv_line(s, line, sizeof line, TIMEOUT_MS);
        if (r != 1)
            return fail(s, r ? "the server closed the connection at sign-in" : "no answer to NICK");
        if (strstr(line, " 422 ") || strstr(line, " 376 "))
            return 1;
        if (!strncmp(line, "PING", 4))
        {
            char pong[300];
            snprintf(pong, sizeof pong, "PONG %s", line + 5);
            send_line(s, pong);
        }
    }
}

int pol_pump(PolSession* s, int timeout_ms)
{
    char line[1024];
    for (;;)
    {
        int r = recv_line(s, line, sizeof line, timeout_ms);
        if (r < 0)
            return 0;
        if (r == 0)
            return 1;
        const char* ping = strstr(line, "PING");
        if (ping == line || (line[0] == ':' && ping))
        {
            char pong[300];
            snprintf(pong, sizeof pong, "PONG %s", ping + 5);
            send_line(s, pong);
        }
        timeout_ms = 0; /* drain what is already here, then return */
    }
}

/* --- polpro -----------------------------------------------------------------------------------------
 * One connection per request, as the host does: a plaintext hello (0x28; byte 1 != 1 asks for an
 * encrypted session) and the server's reply (0x14 zeros + a nonce); then the request - header 0x28
 * and body - under an OFB stream started at the sqIrc IV; then the reply header 0x18 and body under
 * a fresh stream from the IV. The server identifies the member by which live sqIrc key decrypts. */
static int polpro_request(PolSession* s, uint16_t port, uint8_t cmd, uint8_t sub, const uint8_t* body, uint32_t len,
    uint8_t* reply, uint32_t cap, uint32_t* got)
{
    PolSocket c = pol_tcp_connect(s->host, port, TIMEOUT_MS);
    if (c == POL_NO_SOCKET)
        return fail(s, "cannot connect to polpro %s:%u", s->host, port);
    uint8_t hello[0x28] = { 0 }, answer[0x18];
    if (!pol_send_all(c, hello, sizeof hello) || !pol_recv_exact(c, answer, sizeof answer, TIMEOUT_MS))
    {
        pol_close(c);
        return fail(s, "polpro hello failed");
    }
    /* header: 2, cmd, sub, 0, u32 length, 16 zeros, MD5(8 bytes || 15 bytes || nonce) - the
     * server does not verify the digest; ours covers zeros and the nonce */
    uint8_t msg[0x28 + 512] = { 0 }, md_in[27] = { 0 };
    if (len > 512)
    {
        pol_close(c);
        return fail(s, "request too long");
    }
    msg[0] = 2;
    msg[1] = cmd;
    msg[2] = sub;
    msg[4] = (uint8_t)len;
    msg[5] = (uint8_t)(len >> 8);
    memcpy(md_in + 23, answer + 0x14, 4);
    pol_md5(md_in, sizeof md_in, msg + 0x18);
    memcpy(msg + 0x28, body, len);
    PolOfb tx;
    pol_ofb_start(&tx, &s->bf, s->iv);
    pol_ofb_crypt(&tx, msg, msg, 0x28 + len);
    if (!pol_send_all(c, msg, 0x28 + len))
    {
        pol_close(c);
        return fail(s, "polpro send failed");
    }
    uint8_t hdr[0x18];
    if (!pol_recv_exact(c, hdr, sizeof hdr, TIMEOUT_MS))
    {
        pol_close(c);
        return fail(s, "no polpro reply");
    }
    PolOfb rx;
    pol_ofb_start(&rx, &s->bf, s->iv);
    pol_ofb_crypt(&rx, hdr, hdr, sizeof hdr);
    uint32_t n = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    if (hdr[1])
    {
        pol_close(c);
        return fail(s, "polpro error %u (PlayOnline shows %u)", hdr[1], 5200u + hdr[1]);
    }
    if (n > cap || !pol_recv_exact(c, reply, n, TIMEOUT_MS))
    {
        pol_close(c);
        return fail(s, "polpro reply body of %u bytes not received", n);
    }
    pol_ofb_crypt(&rx, reply, reply, n);
    pol_close(c);
    *got = n;
    return 1;
}

int pol_ffxi_session_value(PolSession* s, uint16_t polpro_port, uint8_t v[16])
{
    /* the 0x28-byte request body: the status block at +0x10 (specs/polcore-slots.session.txt,
     * section C): handle 0, no active character, login mode, contents
     * class 1 (FFXI), OpenStat 0 (+1), no friend-auth, not a change, and the constant 1 at +0x19 */
    uint8_t body[0x28] = { 0 };
    body[0x13] = 1;
    body[0x14] = POL_CONTENTS_FFXI;
    body[0x16] = 1;
    body[0x19] = 1;
    uint8_t reply[0x40];
    uint32_t n = 0;
    if (!polpro_request(s, polpro_port, 4, 5, body, sizeof body, reply, sizeof reply, &n))
        return 0;
    if (n != 0x20 || pol_sum32(reply, 0x1c) != ((uint32_t)reply[0x1c] | (uint32_t)reply[0x1d] << 8 |
                                                   (uint32_t)reply[0x1e] << 16 | (uint32_t)reply[0x1f] << 24))
        return fail(s, "the session reply is not the 0x20-byte checked body (%u bytes)", n);
    memcpy(v, reply, 16);
    for (int i = 0; i < 16; ++i)
        if (!v[i])
            return fail(s, "the server issued no session value (is %s a registered member?)", s->pol_id);
    return 1;
}

void pol_signout(PolSession* s)
{
    if (s->irc != POL_NO_SOCKET)
    {
        send_line(s, "QUIT");
        pol_close(s->irc);
    }
    s->irc = POL_NO_SOCKET;
}
