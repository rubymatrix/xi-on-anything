/* Signing in to a LandSandBoat server.
 *
 * gamecore is our own and Winsock is our shim, so each piece of the sign-in has a direct home:
 *
 *   1. Auth (TLS 1.3 on current servers, port 54231, xi_connect): one JSON request - username,
 *      password, OTP, the loader version (2.2.x now), command 0x10 (login), the saved trust token
 *      and whether to trust this computer - and one JSON reply: result 1 with account_id, the
 *      16-byte session_hash and perhaps a new trust_token, another result, or error_message
 *      (xiloader src/network.cpp VerifyAccount, src/command_handler.h; LandSandBoat
 *      src/login/auth_session.cpp). The hash is random now: it is also the account's credential
 *      on the server's profile service (src/profile), which xiloader 2.2 connects the game's
 *      friend list, presence and messages to. We do not connect there; the game and the lobby do
 *      not need it.
 *   2. The login data connection (TCP 54230): we send 0xFE + the hash, then answer the server for
 *      the rest of the run - 0x01 with 0xA1 (account id, server address, hash), 0x02 / 0x15 with
 *      0xA2 and the server's fixed key, 0x03 (the character list) with nothing: our gamecore
 *      builds its character records from FFXiMain's own table.
 *   3. The lobby (TCP 54001 and 54230): every lobby command FFXiMain sends carries the session
 *      hash at +12, where the server looks for it (ws2_set_lobby_session).
 *   4. gamecore: there is no account-service session, so the session value is zero, and LSB's
 *      zone key is derived from exactly that (the 0xA2 key is MD5 input the client also computes),
 *      so our gamecore reports 16 zero bytes. Its command line carries the view port.
 *   5. The game's lobby host name resolves to the server (host64's --server). */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if !defined(FFXI_UWP) /* UWP apps have no SChannel and no console: uwp_bridge.h */
#include <security.h>
/* SCH_CREDENTIALS (TLS 1.3) is declared only with this, and it needs UNICODE_STRING */
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <schannel.h>
#include <conio.h>
#include <io.h>
#endif
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#if !defined(_WIN32) /* Windows has its own TLS: SChannel */
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#endif
#endif

#include "lsb_login.h"
#include "plat.h"
#if defined(FFXI_UWP)
#include "uwp_bridge.h"
#endif
#include "gamecore_config.h"
#include "keychain.h"
#include "ws2.h"

int lsb_parse_version(const char* s, int out[3])
{
    if (!s)
        return 0;
    for (int i = 0; i < 3; ++i)
    {
        if (*s < '0' || *s > '9')
            return 0;
        char* end;
        long v = strtol(s, &end, 10);
        if (v > 65535 || *end != (i < 2 ? '.' : 0))
            return 0;
        out[i] = (int)v;
        s = end + (i < 2);
    }
    return 1;
}

#define TIMEOUT_MS 15000

static sock_t tcp_connect(uint32_t server, uint16_t port, int timeout_ms, char* err, size_t errn)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD)
    {
        snprintf(err, errn, "no socket");
        return SOCK_BAD;
    }
    if (timeout_ms)
    {
#if defined(_WIN32)
        DWORD tv = (DWORD)timeout_ms;
#else
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(server);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0)
    {
        snprintf(err, errn, "cannot reach %u.%u.%u.%u:%u", server >> 24, (server >> 16) & 255, (server >> 8) & 255, server & 255, port);
        sock_close(s);
        return SOCK_BAD;
    }
    return s;
}

/* --- TLS over our own socket ----------------------------------------------------------------------- */
#if defined(FFXI_UWP)
/* the app's (Windows.Networking.Sockets) */
static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    return uwp_tls_exchange(server, port, request, reply, replyn, err, errn);
}
#elif defined(_WIN32)
/* SChannel: the handshake by hand over the socket, then one encrypted request and one reply */
static int send_all(sock_t s, const void* p, size_t n)
{
    for (size_t done = 0; done < n;)
    {
        int k = send(s, (const char*)p + done, (int)(n - done), 0);
        if (k <= 0)
            return 0;
        done += (size_t)k;
    }
    return 1;
}

/* Drives InitializeSecurityContext until it is done: the handshake (no context yet), or, on a
 * context, a TLS 1.3 post-handshake message DecryptMessage hands back with SEC_I_RENEGOTIATE: the
 * two session tickets a LandSandBoat server's OpenSSL sends right after the handshake each come
 * back that way. in[0..*got] is what was received and not yet used; what is left over stays there. */
static int tls_drive(sock_t s, CredHandle* cred, CtxtHandle* ctx, int* have_ctx, char* in, size_t cap, size_t* got,
    char* err, size_t errn)
{
    for (;;)
    {
        SecBuffer ib[2] = { { (unsigned long)*got, SECBUFFER_TOKEN, in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBuffer ob[1] = { { 0, SECBUFFER_TOKEN, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib }, od = { SECBUFFER_VERSION, 1, ob };
        unsigned long flags = ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM;
        SECURITY_STATUS st = InitializeSecurityContextA(cred, *have_ctx ? ctx : NULL, *have_ctx ? NULL : (SEC_CHAR*)"ffxi", flags, 0, 0,
            *have_ctx ? &id : NULL, 0, *have_ctx ? NULL : ctx, &od, &flags, NULL);
        *have_ctx = 1;
        if (ib[1].BufferType == SECBUFFER_EXTRA)
        {
            memmove(in, in + (*got - ib[1].cbBuffer), ib[1].cbBuffer);
            *got = ib[1].cbBuffer;
        }
        else if (st != SEC_E_INCOMPLETE_MESSAGE)
            *got = 0;
        if (ob[0].pvBuffer)
        {
            int sent = !ob[0].cbBuffer || send_all(s, ob[0].pvBuffer, ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!sent)
                st = SEC_E_INTERNAL_ERROR;
        }
        if (st == SEC_E_OK)
            return 1;
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_INCOMPLETE_MESSAGE)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (0x%08lx)", (unsigned long)st);
            return 0;
        }
        int n = *got < cap ? recv(s, in + *got, (int)(cap - *got), 0) : 0;
        if (n <= 0)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (connection closed)");
            return 0;
        }
        *got += (size_t)n;
    }
}

/* The credentials for one try. Current LandSandBoat servers speak TLS 1.3 only (src/login/handler.h:
 * asio::ssl::context::tlsv13_server; xiloader src/network.cpp sets the same minimum), which
 * SChannel offers only through SCH_CREDENTIALS (SCHANNEL_CRED stops at TLS 1.2; TLS 1.3 itself
 * needs Windows 11 or Server 2022). Try 0 asks for TLS 1.2 or 1.3 that way; try 1 is the older
 * SCHANNEL_CRED with the system's protocols, for Windows without SCH_CREDENTIALS. Neither checks
 * the certificate: private servers present self-signed ones. */
static int tls_credentials(int attempt, CredHandle* cred)
{
    const DWORD flags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
#if defined(SCH_CREDENTIALS_VERSION)
    if (attempt == 0)
    {
        TLS_PARAMETERS tp;
        memset(&tp, 0, sizeof tp);
        tp.grbitDisabledProtocols = (DWORD) ~(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
        SCH_CREDENTIALS sc;
        memset(&sc, 0, sizeof sc);
        sc.dwVersion = SCH_CREDENTIALS_VERSION;
        sc.dwFlags = flags;
        sc.cTlsParameters = 1;
        sc.pTlsParameters = &tp;
        return AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, cred, NULL) == SEC_E_OK;
    }
#else
    if (attempt == 0)
        return 0;
#endif
    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = flags;
    return AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, cred, NULL) == SEC_E_OK;
}

static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    sock_t s = SOCK_BAD;
    int ok = 0, have_cred = 0, have_ctx = 0;
    CredHandle cred;
    CtxtHandle ctx;
    static char in[32768];
    size_t got = 0;
    /* a handshake that fails is tried again, on a new connection, with the next credentials;
     * nothing has been sent yet */
    for (int attempt = 0;; ++attempt)
    {
        if (attempt == 2)
            goto out;
        if (have_ctx)
            DeleteSecurityContext(&ctx), have_ctx = 0;
        if (have_cred)
            FreeCredentialsHandle(&cred), have_cred = 0;
        if (s != SOCK_BAD)
            sock_close(s);
        got = 0, s = SOCK_BAD;
        if (!tls_credentials(attempt, &cred))
        {
            snprintf(err, errn, "TLS setup failed");
            continue;
        }
        have_cred = 1;
        s = tcp_connect(server, port, TIMEOUT_MS, err, errn);
        if (s == SOCK_BAD)
            goto out;
        if (tls_drive(s, &cred, &ctx, &have_ctx, in, sizeof in, &got, err, errn))
            break;
    }
    SecPkgContext_StreamSizes sz;
    if (QueryContextAttributesA(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz) != SEC_E_OK)
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    /* the request, one record at a time */
    size_t n = strlen(request);
    char* rec = (char*)malloc(sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
    for (size_t done = 0; done < n;)
    {
        size_t part = n - done < sz.cbMaximumMessage ? n - done : sz.cbMaximumMessage;
        memcpy(rec + sz.cbHeader, request + done, part);
        SecBuffer b[4] = { { sz.cbHeader, SECBUFFER_STREAM_HEADER, rec }, { (unsigned long)part, SECBUFFER_DATA, rec + sz.cbHeader },
            { sz.cbTrailer, SECBUFFER_STREAM_TRAILER, rec + sz.cbHeader + part }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK || !send_all(s, rec, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))
        {
            SecureZeroMemory(rec, sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
            free(rec);
            snprintf(err, errn, "could not send the login request");
            goto out;
        }
        done += part;
    }
    SecureZeroMemory(rec, sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
    free(rec);
    /* the reply: the first record with data in it */
    for (;;)
    {
        if (got)
        {
            SecBuffer b[4] = { { (unsigned long)got, SECBUFFER_DATA, in }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL },
                { 0, SECBUFFER_EMPTY, NULL } };
            SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
            SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, NULL);
            if (st == SEC_E_OK)
            {
                SecBuffer *data = NULL, *extra = NULL;
                for (int i = 1; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_DATA)
                        data = &b[i];
                    else if (b[i].BufferType == SECBUFFER_EXTRA)
                        extra = &b[i];
                size_t k = 0;
                if (data)
                {
                    k = data->cbBuffer < replyn - 1 ? data->cbBuffer : replyn - 1;
                    memcpy(reply, data->pvBuffer, k);
                }
                if (extra)
                    memmove(in, in + (got - extra->cbBuffer), extra->cbBuffer), got = extra->cbBuffer;
                else
                    got = 0;
                if (k)
                {
                    reply[k] = 0;
                    ok = 1;
                    break;
                }
                continue; /* a record with nothing for us */
            }
            if (st == SEC_I_RENEGOTIATE)
            {
                /* a TLS 1.3 post-handshake message (a session ticket), which SChannel wants
                 * InitializeSecurityContext to take: what follows it is in the extra buffer */
                SecBuffer* extra = NULL;
                for (int i = 1; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_EXTRA)
                        extra = &b[i];
                if (extra)
                    memmove(in, in + (got - extra->cbBuffer), extra->cbBuffer), got = extra->cbBuffer;
                else
                    got = 0;
                if (!tls_drive(s, &cred, &ctx, &have_ctx, in, sizeof in, &got, err, errn))
                    break;
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE)
            {
                snprintf(err, errn, "the login server did not reply");
                break;
            }
        }
        int k = got < sizeof in ? recv(s, in + got, (int)(sizeof in - got), 0) : 0;
        if (k <= 0)
        {
            snprintf(err, errn, "the login server did not reply");
            break;
        }
        got += (size_t)k;
    }
out:
    if (have_ctx)
        DeleteSecurityContext(&ctx);
    if (have_cred)
        FreeCredentialsHandle(&cred);
    if (s != SOCK_BAD)
        sock_close(s);
    return ok;
}
#else
static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    long n = (long)send(*(sock_t*)ctx, (const char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    long n = (long)recv(*(sock_t*)ctx, (char*)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_TIMEOUT : n == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : (int)n;
}

/* one request, one reply (NUL-terminated); 0 with a message on failure */
static int tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err, size_t errn)
{
    sock_t s = tcp_connect(server, port, TIMEOUT_MS, err, errn);
    if (s == SOCK_BAD)
        return 0;
    int ok = 0, r;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"lsblogin", 8);
#endif
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    /* private servers present self-signed certificates. TLS 1.2 or 1.3: current servers speak
     * only 1.3 (xiloader src/network.cpp asks for nothing less), older ones 1.2 as well */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#endif
    if (mbedtls_ssl_setup(&ssl, &conf))
    {
        snprintf(err, errn, "TLS setup failed");
        goto out;
    }
    mbedtls_ssl_set_bio(&ssl, &s, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            snprintf(err, errn, "TLS handshake with the login server failed (-0x%04x)", (unsigned)-r);
            goto out;
        }
    size_t n = strlen(request), done = 0;
    while (done < n)
    {
        r = mbedtls_ssl_write(&ssl, (const unsigned char*)request + done, n - done);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0)
        {
            snprintf(err, errn, "could not send the login request");
            goto out;
        }
        done += (size_t)r;
    }
    do
        r = mbedtls_ssl_read(&ssl, (unsigned char*)reply, replyn - 1);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
           || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
    );
    if (r <= 0)
    {
        snprintf(err, errn, "the login server did not reply");
        goto out;
    }
    reply[r] = 0;
    ok = 1;
    mbedtls_ssl_close_notify(&ssl);
out:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
#endif
    sock_close(s);
    return ok;
}
#endif

/* --- the little JSON xi_connect speaks ------------------------------------------------------------- */
static void json_string(char* out, size_t n, const char* s)
{
    size_t o = 0;
    out[o++] = '"';
    for (; *s && o + 7 < n; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            out[o++] = '\\', out[o++] = (char)c;
        else if (c < 0x20)
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else
            out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
}

/* the value after "key": in a flat object, or NULL */
static const char* json_find(const char* j, const char* key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = strstr(j, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    return p;
}

static int json_int(const char* j, const char* key, long long* out)
{
    const char* p = json_find(j, key);
    if (!p || !((*p >= '0' && *p <= '9') || *p == '-'))
        return 0;
    *out = strtoll(p, NULL, 10);
    return 1;
}

/* a JSON string's text, unescaped (\uXXXX beyond ASCII as '?') */
static int json_str(const char* j, const char* key, char* out, size_t n)
{
    const char* p = json_find(j, key);
    if (!p || *p != '"')
        return 0;
    size_t o = 0;
    for (++p; *p && *p != '"' && o + 1 < n; ++p)
    {
        if (*p == '\\' && p[1])
        {
            ++p;
            if (*p == 'u')
            {
                /* \uXXXX: ASCII as itself, anything else as '?' */
                char hex[5] = { 0 };
                int k = 0;
                for (; k < 4 && isxdigit((unsigned char)p[1]); ++k)
                    hex[k] = *++p;
                unsigned long c = strtoul(hex, NULL, 16);
                out[o++] = k == 4 && c < 0x80 ? (char)c : '?';
            }
            else
                out[o++] = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : *p;
        }
        else
            out[o++] = *p;
    }
    out[o] = 0;
    return 1;
}

/* --- the login data connection --------------------------------------------------------------------- */
typedef struct DataConn
{
    sock_t s;
    uint32_t account, server;
    uint8_t hash[16];
} DataConn;

static DataConn g_data;

static void data_thread(void* arg)
{
    DataConn* d = (DataConn*)arg;
    uint8_t in[4096], out[28];
    for (;;)
    {
        long n = (long)recv(d->s, (char*)in, (int)sizeof in, 0);
        if (n <= 0)
        {
            fprintf(stderr, "[lsb] login data connection closed\n");
            return;
        }
        memset(out, 0, sizeof out);
        switch (in[0])
        {
        case 0x01: /* who is this: the account, the server as the client sees it, the hash */
            out[0] = 0xA1;
            for (int i = 0; i < 4; ++i)
                out[1 + i] = (uint8_t)(d->account >> (8 * i));
            for (int i = 0; i < 4; ++i) /* in_addr bytes: a.b.c.d */
                out[5 + i] = (uint8_t)(d->server >> (24 - 8 * i));
            memcpy(out + 12, d->hash, 16);
            break;
        case 0x02:
        case 0x15: /* the key: the constant a zero session value gives */
            out[0] = 0xA2;
            out[17] = 0x58, out[18] = 0xE0, out[19] = 0x5D, out[20] = 0xAD;
            break;
        default: /* 0x03 the character list, and anything else: no answer */
            continue;
        }
        if (send(d->s, (const char*)out, sizeof out, 0) <= 0)
        {
            fprintf(stderr, "[lsb] login data connection lost\n");
            return;
        }
    }
}

int net_resolve_ipv4(const char* name, uint32_t* out)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res)
        return 0;
    *out = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 1;
}

int read_secret(const char* prompt, char* out, size_t n)
{
#if defined(FFXI_UWP)
    (void)prompt, (void)out, (void)n;
    return 0; /* no terminal: the app asks */
#elif defined(_WIN32)
    if (!_isatty(_fileno(stdin)))
        return 0;
    fputs(prompt, stderr);
    size_t o = 0;
    for (int c; (c = _getch()) != '\r' && c != '\n' && c != EOF;)
        if (c == 8 && o)
            o--;
        else if (c >= 32 && o + 1 < n)
            out[o++] = (char)c;
    out[o] = 0;
    fputs("\n", stderr);
    return 1;
#else
    if (!isatty(STDIN_FILENO))
        return 0;
    fputs(prompt, stderr);
    fflush(stderr);
    struct termios old, quiet;
    tcgetattr(STDIN_FILENO, &old);
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    int ok = fgets(out, (int)n, stdin) != NULL;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    fputs("\n", stderr);
    if (!ok)
        return 0;
    out[strcspn(out, "\r\n")] = 0;
    return 1;
#endif
}

/* The version a refusal names, an x (or *, or a missing patch) as 0. Servers word it differently -
 * "Unsupported ... version 1.0.0.\nThis server requires version 2.2.x.", "Please update to version
 * '2.0.x'", "Needs 2.2.x" - so every major.minor[.patch] in the message is a candidate and
 * the last one other than the version we sent wins (the requirement follows the echo of ours). */
static int server_version(const char* message, const int sent[3], int out[3])
{
    int found = 0;
    for (const char* p = message; *p; ++p)
    {
        if (*p < '0' || *p > '9' || (p > message && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9'))))
            continue;
        int v[3] = { 0, 0, 0 }, parts = 0;
        const char* q = p;
        while (parts < 3)
        {
            if (*q >= '0' && *q <= '9')
            {
                char* end;
                long n = strtol(q, &end, 10);
                if (n > 65535)
                    break;
                v[parts++] = (int)n;
                q = end;
            }
            else if (parts == 2 && (*q == 'x' || *q == 'X' || *q == '*'))
                v[parts++] = 0, ++q;
            else
                break;
            if (parts == 3 || *q != '.' || !q[1] || !(q[1] == 'x' || q[1] == 'X' || q[1] == '*' || (q[1] >= '0' && q[1] <= '9')))
                break;
            ++q;
        }
        if (parts < 2)
            continue;
        if (memcmp(v, sent, sizeof v) || !found)
        {
            memcpy(out, v, sizeof v);
            found = memcmp(v, sent, sizeof v) ? 2 : 1;
        }
        p = q - 1;
    }
    return found == 2;
}

/* --- trust tokens ----------------------------------------------------------------------------------
 * xiloader keeps them per server and user with their expiry, in a DPAPI-sealed file
 * (src/trust_token.cpp); ours go in the credential store as "<expires> <token>" under
 * "lsbtrust:<server>:<user>". Where there is no store (keychain.h) none is kept, and an account
 * with an OTP asks for the code every time, as before. */
static void trust_key(const LsbLogin* l, char* out, size_t n)
{
    snprintf(out, n, "lsbtrust:%s:%s", l->trust_name, l->user);
}

/* the saved token, "" for none; an expired one is forgotten (xiloader loadTrustToken) */
static void trust_load(const LsbLogin* l, char* out, size_t n)
{
    out[0] = 0;
    if (!l->trust_name)
        return;
    char key[400], v[256];
    trust_key(l, key, sizeof key);
    if (!keychain_get(key, v, sizeof v))
        return;
    char* sp;
    long long expires = strtoll(v, &sp, 10);
    if (*sp == ' ' && expires > (long long)time(NULL))
        snprintf(out, n, "%s", sp + 1);
    else
        keychain_delete(key);
    memset(v, 0, sizeof v);
}

static void trust_forget(const LsbLogin* l)
{
    if (!l->trust_name)
        return;
    char key[400];
    trust_key(l, key, sizeof key);
    keychain_delete(key);
}

/* the token a sign-in handed out: trust_expires (seconds since 1970) when the server says, else
 * 30 days, LandSandBoat's lifetime (src/login/otp_helpers.h saveTrustToken; xiloader
 * src/command_handler.h does the same) */
static void trust_save(const LsbLogin* l, const char* reply)
{
    char token[200], key[400], v[256];
    if (!json_str(reply, "trust_token", token, sizeof token) || !token[0])
        return;
    long long now = (long long)time(NULL), expires;
    if (!json_int(reply, "trust_expires", &expires))
        expires = now + 30 * 24 * 60 * 60;
    if (!l->trust_name)
        return;
    trust_key(l, key, sizeof key);
    snprintf(v, sizeof v, "%lld %s", expires, token);
    if (keychain_set(key, v))
        fprintf(stderr, "[lsb] this computer is trusted for %lld days\n", (expires - now) / (24 * 60 * 60));
    else
        fprintf(stderr, "[lsb] the server trusts this computer, but there is nowhere to keep its token\n");
    memset(token, 0, sizeof token), memset(v, 0, sizeof v);
}

int lsb_login(const LsbLogin* l, char* err, size_t errn)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int version[3];
    const char* v = l->version && *l->version ? l->version : LSB_LOADER_VERSION;
    if (!lsb_parse_version(v, version))
    {
        snprintf(err, errn, "the loader version \"%s\" is not major.minor.patch", v);
        return 0;
    }
    char user[160], pass[160], otp[64], token[600], trust[200], req[1600];
    json_string(user, sizeof user, l->user);
    json_string(pass, sizeof pass, l->password ? l->password : "");
    json_string(otp, sizeof otp, l->otp ? l->otp : "");
    /* the trust token saved for this server and user, sent with every login (xiloader
     * src/network.cpp: trust_token and trust_this_computer go with command 0x10 only) */
    {
        char t[160];
        trust_load(l, t, sizeof t);
        json_string(trust, sizeof trust, t);
        memset(t, 0, sizeof t);
    }
    /* login_token, which some servers add: their launcher's single-use token
     * stands in for the password and the OTP */
    token[0] = 0;
    if (l->login_token && *l->login_token)
    {
        char t[560];
        json_string(t, sizeof t, l->login_token);
        snprintf(token, sizeof token, "\"login_token\":%s,", t);
    }
    if (l->version_used)
        l->version_used[0] = 0;
    char reply[8192], message[512];
    int adopted = 0;
    for (int attempt = 0;; ++attempt)
    {
        snprintf(req, sizeof req,
            "{\"command\":16,%s\"new_password\":\"\",\"otp\":%s,\"password\":%s,\"trust_this_computer\":%s,"
            "\"trust_token\":%s,\"username\":%s,\"version\":[%d,%d,%d]}",
            token, otp, pass, l->trust ? "true" : "false", trust, user, version[0], version[1], version[2]);
        int ok = tls_exchange(l->server, l->auth_port, req, reply, sizeof reply, err, errn);
        memset(req, 0, sizeof req);
        if (!ok)
        {
            memset(pass, 0, sizeof pass), memset(trust, 0, sizeof trust);
            return 0;
        }
        /* A version the server refuses names the one it wants ("This server requires version
         * 2.2.x."). Sign in again with that (x as 0) and report it for next time; a second refusal
         * naming yet another version is followed too, up to a few tries. */
        int want[3];
        if (attempt < 3 && json_str(reply, "error_message", message, sizeof message)
            && server_version(message, version, want))
        {
            fprintf(stderr, "[lsb] the server wants loader version %d.%d.%d, not %d.%d.%d: signing in with that\n",
                want[0], want[1], want[2], version[0], version[1], version[2]);
            memcpy(version, want, sizeof version);
            adopted = 1;
            continue;
        }
        break;
    }
    memset(pass, 0, sizeof pass), memset(trust, 0, sizeof trust);

    if (json_str(reply, "error_message", message, sizeof message) && message[0])
    {
        /* its lines (\r\n) and other control characters as single spaces: the message is shown on
         * one line, in the log and on the sign-in screen */
        size_t o = 0;
        for (const char* p = message; *p; ++p)
            if ((unsigned char)*p < 0x20 || *p == ' ')
            {
                if (o && message[o - 1] != ' ')
                    message[o++] = ' ';
            }
            else
                message[o++] = *p;
        while (o && message[o - 1] == ' ')
            --o;
        message[o] = 0;
        snprintf(err, errn, "the server says: %s", message);
        return 0;
    }
    long long result = 0, account = 0;
    if (!json_int(reply, "result", &result))
    {
        snprintf(err, errn, "the login server's reply has no result");
        return 0;
    }
    /* the results a login can get (LandSandBoat src/login/auth_session.h login_result, xiloader
     * src/command_handler.h handleLoginCommand) */
    switch (result)
    {
    case 1: /* LOGIN_SUCCESS */
        break;
    case 0: /* LOGIN_FAIL: an account whose status is not normal (auth_session.cpp, LOGIN_ATTEMPT) */
        snprintf(err, errn, "this account may not sign in (it is suspended or banned)");
        return 0;
    case 2: /* LOGIN_ERROR: the password, or the OTP of an account that has one */
        snprintf(err, errn, (l->otp && *l->otp) ? "invalid username, password or one-time code" : "invalid username or password");
        return 0;
    case 0x0A: /* LOGIN_ERROR_ALREADY_LOGGED_IN */
        snprintf(err, errn, "this account is already signed in");
        return 0;
    case 0x0B: /* LOGIN_ERROR_VERSION_UNSUPPORTED */
        snprintf(err, errn, "the server does not accept this loader version: check with the server which one it expects");
        return 0;
    case 0x13: /* LOGIN_ERROR_TRUST_TOKEN_INVALID: the saved token, sent without a code, was refused */
        trust_forget(l);
        snprintf(err, errn, "the server no longer trusts this computer: sign in with your one-time code");
        return 0;
    case 0x14: /* not LandSandBoat's: servers whose launchers hand out login_token */
        snprintf(err, errn, "the launch token is invalid or expired: get a new one from the server's launcher");
        return 0;
    default:
        snprintf(err, errn, "the login server answered %lld", result);
        return 0;
    }
    if (!json_int(reply, "account_id", &account))
    {
        snprintf(err, errn, "the login server sent no account id");
        return 0;
    }
    /* session_hash: 16 numbers (signed chars) */
    const char* p = json_find(reply, "session_hash");
    uint8_t hash[16];
    int k = 0;
    if (p && *p == '[')
        for (++p; k < 16 && *p && *p != ']';)
        {
            char* end;
            long v = strtol(p, &end, 10);
            if (end == p)
                break;
            hash[k++] = (uint8_t)v;
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
    if (k != 16)
    {
        snprintf(err, errn, "the login server sent no session hash");
        return 0;
    }
    trust_save(l, reply);

    /* the data connection, answered for the rest of the run */
    g_data.s = tcp_connect(l->server, l->data_port, 0, err, errn);
    if (g_data.s == SOCK_BAD)
        return 0;
    g_data.account = (uint32_t)account, g_data.server = l->server;
    memcpy(g_data.hash, hash, 16);
    uint8_t first[28] = { 0xFE };
    memcpy(first + 12, hash, 16);
    if (send(g_data.s, (const char*)first, sizeof first, 0) <= 0 || !plat_thread_start(data_thread, &g_data))
    {
        snprintf(err, errn, "the login data connection failed");
        sock_close(g_data.s);
        return 0;
    }

    ws2_set_lobby_session(hash, l->data_port, l->view_port);
    static const uint8_t zero[16] = { 0 };
    gamecore_set_session(zero);
    char cmd[64];
    snprintf(cmd, sizeof cmd, " /game eAZcFcB -net 3 -port %u", l->view_port);
    gamecore_set_cmdline(cmd);
    fprintf(stderr, "[lsb] signed in to LSB as %s (account %lld)\n", l->user, account);
    if (adopted && l->version_used)
        snprintf(l->version_used, 24, "%d.%d.%d", version[0], version[1], version[2]);
    return 1;
}
