/* PlayOnline's wire primitives, client side. See polcrypt.h. */
#include <string.h>

#include "polcrypt.h"

/* --- line framing ---------------------------------------------------------------------------------- */

uint32_t pol_sum32(const uint8_t* data, size_t n)
{
    size_t whole = n / 4 * 4;
    uint32_t s = 0, tail = 0;
    for (size_t i = 0; i < whole; i += 4)
        s += (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) | ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    for (size_t i = whole; i < n; ++i)
        tail = (tail >> 8) | ((uint32_t)data[i] << 24);
    return s + tail;
}

void pol_chk4(const uint8_t* content, size_t n, uint8_t out[4])
{
    uint32_t v = pol_sum32(content, n);
    out[0] = (uint8_t)(((v >> 26) & 63) + 0x3f);
    out[1] = (uint8_t)(((v >> 20) & 63) + 0x3f);
    out[2] = (uint8_t)(((v >> 14) & 63) + 0x3f);
    out[3] = (uint8_t)(((v >> 8) & 63) + 0x3f);
}

/* --- alphabets -------------------------------------------------------------------------------------- */
static const char B64[] = "TSG8IncW3HFKokOg79qzeCmZs2yBYEQVAUxR5rbwi4P@jMDLtpvad0f_J1hlN6uX";
static const char B32[] = "N43OVHBJ1Y2C0WSXED5QFILRZMUTAPGK";

size_t pol_b64enc(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        size_t g = n - i < 3 ? n - i : 3;
        uint32_t v = (uint32_t)in[i] << 16 | (g > 1 ? (uint32_t)in[i + 1] << 8 : 0) | (g > 2 ? in[i + 2] : 0);
        char c[4] = { B64[(v >> 18) & 63], B64[(v >> 12) & 63], B64[(v >> 6) & 63], B64[v & 63] };
        for (size_t k = 0; k < g + 1; ++k)
            out[o++] = c[k];
    }
    out[o] = 0;
    return o;
}

static int b64val(char c)
{
    const char* p = strchr(B64, c);
    return p && c ? (int)(p - B64) : 0;
}

size_t pol_b64dec(const char* in, size_t n, uint8_t* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4)
    {
        size_t g = n - i < 4 ? n - i : 4;
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k)
            v = (v << 6) | (uint32_t)(k < g ? b64val(in[i + k]) : 0);
        uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        for (size_t k = 0; k + 1 < g; ++k)
            out[o++] = b[k];
    }
    return o;
}

size_t pol_b32enc(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 5)
    {
        uint64_t v = 0;
        for (size_t k = 0; k < 5; ++k)
            v = (v << 8) | (i + k < n ? in[i + k] : 0);
        for (int k = 0; k < 8; ++k)
            out[o++] = B32[(v >> (35 - 5 * k)) & 31];
    }
    out[o] = 0;
    return o;
}

/* --- MD5 -------------------------------------------------------------------------------------------- */
static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t MD5_R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9,
    14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6,
    10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(uint32_t h[4], const uint8_t* p)
{
    uint32_t X[16];
    for (int i = 0; i < 16; ++i)
        X[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    uint32_t A = h[0], B = h[1], C = h[2], D = h[3];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t F;
        int g;
        if (i < 16)
            F = (B & C) | (~B & D), g = i;
        else if (i < 32)
            F = (D & B) | (~D & C), g = (5 * i + 1) % 16;
        else if (i < 48)
            F = B ^ C ^ D, g = (3 * i + 5) % 16;
        else
            F = C ^ (B | ~D), g = (7 * i) % 16;
        F = F + A + MD5_K[i] + X[g];
        A = D;
        D = C;
        C = B;
        B = B + ((F << MD5_R[i]) | (F >> (32 - MD5_R[i])));
    }
    h[0] += A;
    h[1] += B;
    h[2] += C;
    h[3] += D;
}

static void md5_iv(const uint8_t* msg, size_t n, const uint32_t iv[4], uint8_t out[16])
{
    uint32_t h[4] = { iv[0], iv[1], iv[2], iv[3] };
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
        md5_block(h, msg + i);
    uint8_t tail[128] = { 0 };
    size_t rest = n - i;
    memcpy(tail, msg + i, rest);
    tail[rest] = 0x80;
    size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; ++k)
        tail[len - 8 + k] = (uint8_t)(bits >> (8 * k));
    md5_block(h, tail);
    if (len == 128)
        md5_block(h, tail + 64);
    for (int k = 0; k < 4; ++k)
        for (int b = 0; b < 4; ++b)
            out[4 * k + b] = (uint8_t)(h[k] >> (8 * b));
}

void pol_md5(const uint8_t* msg, size_t n, uint8_t out[16])
{
    static const uint32_t IV[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    md5_iv(msg, n, IV, out);
}

void pol_md5_zero_iv(const uint8_t* msg, size_t n, uint8_t out[16])
{
    static const uint32_t ZERO[4] = { 0, 0, 0, 0 };
    md5_iv(msg, n, ZERO, out);
}

/* --- Blowfish variant -------------------------------------------------------------------------------- */
static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t bf_f(const PolBlowfish* bf, uint32_t x)
{
    return ((bf->S[0][x >> 24] + bf->S[1][(x >> 16) & 255]) ^ bf->S[2][(x >> 8) & 255]) + bf->S[3][x & 255];
}

uint64_t pol_bf_enc64(const PolBlowfish* bf, uint64_t v)
{
    uint32_t cur = (uint32_t)v, other = (uint32_t)(v >> 32);
    for (int i = 0; i < 16; ++i)
    {
        cur ^= bf->P[i];
        uint32_t next = other ^ bf_f(bf, cur);
        other = cur;
        cur = next;
    }
    uint32_t hi = cur ^ bf->P[16], lo = other ^ bf->P[17];
    return (uint64_t)hi << 32 | lo;
}

void pol_bf_init(PolBlowfish* bf, const uint8_t* key, size_t n)
{
    /* The key grows to 0x1000 bytes by appending the MD5 of everything so far: the first digest
     * with MD5's own IV, every later one from a zeroed context. */
    uint8_t buf[0x1000 + 16];
    size_t len = n < 0x1000 ? n : 0x1000;
    memcpy(buf, key, len);
    int first = 1;
    while (len < 0x1000)
    {
        uint8_t d[16];
        if (first)
            pol_md5(buf, len, d);
        else
            pol_md5_zero_iv(buf, len, d);
        first = 0;
        size_t take = 0x1000 - len < 16 ? 0x1000 - len : 16;
        memcpy(buf + len, d, take);
        len += take;
    }
    for (int i = 0; i < 18; ++i)
        bf->P[i] = be32(buf + i); /* sliding by one byte, as the host does */
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 256; ++j)
            bf->S[k][j] = be32(buf + 4 * (256 * k + j));
    for (int i = 0; i < 18; ++i)
        bf->P[i] ^= be32(buf + 4 * i);
    for (int i = 0; i < 18; i += 2) /* always encrypts block 0, not a chain */
    {
        uint64_t v = pol_bf_enc64(bf, 0);
        bf->P[i] = (uint32_t)v;
        bf->P[i + 1] = (uint32_t)(v >> 32);
    }
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 256; j += 2)
        {
            uint64_t v = pol_bf_enc64(bf, 0);
            bf->S[k][j] = (uint32_t)v;
            bf->S[k][j + 1] = (uint32_t)(v >> 32);
        }
}

void pol_line_crypt(const PolBlowfish* bf, uint64_t iv, const uint8_t* in, uint8_t* out, size_t n)
{
    PolOfb s;
    pol_ofb_start(&s, bf, iv);
    pol_ofb_crypt(&s, in, out, n);
}

void pol_ofb_start(PolOfb* s, const PolBlowfish* bf, uint64_t iv)
{
    s->bf = bf;
    s->state = iv;
    s->counter = 0;
}

void pol_ofb_crypt(PolOfb* s, const uint8_t* in, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        unsigned b = s->counter & 7;
        if (b == 0)
            s->state = pol_bf_enc64(s->bf, s->state);
        uint8_t k = (uint8_t)(s->state >> (8 * b));
        s->counter++;
        uint8_t p = in[i], c = p ^ k;
        out[i] = (p == 10 || p == 13 || c == 10 || c == 13) ? p : c;
    }
}

/* --- RSA-256 ----------------------------------------------------------------------------------------
 * The client picks its own modulus; this pair was generated once for this client (the protocol
 * carries no secret in it: the server RSA-wraps a fresh session key under whatever modulus the
 * client names). Numbers are 8 little-endian 32-bit limbs. */
const uint8_t POL_RSA_N[32] = { 0xab, 0x28, 0xb6, 0x5e, 0x30, 0x96, 0x76, 0xa4, 0x74, 0xcc, 0x4a, 0x77, 0x9a, 0x44,
                                0xc0, 0xf3, 0x82, 0x49, 0x36, 0x68, 0xff, 0x1e, 0xfd, 0x26, 0x12, 0xf7, 0x0f, 0x46,
                                0xae, 0x35, 0x2f, 0xef };
static const uint8_t RSA_D[32] = { 0x1f, 0x04, 0xc4, 0x98, 0x5e, 0x76, 0x76, 0xbc, 0x5e, 0x68, 0x49, 0x82, 0x20,
                                   0x83, 0x52, 0x95, 0x64, 0x0c, 0x40, 0x05, 0x06, 0x66, 0x2b, 0xca, 0x12, 0xdc,
                                   0x38, 0xb3, 0x2b, 0xcc, 0x67, 0xab };

typedef struct Big
{
    uint32_t w[9]; /* one spare limb for the reduction */
} Big;

static void big_from_le(Big* a, const uint8_t b[32])
{
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 32; ++i)
        a->w[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
}

static int big_ge(const Big* a, const Big* b)
{
    for (int i = 8; i >= 0; --i)
        if (a->w[i] != b->w[i])
            return a->w[i] > b->w[i];
    return 1;
}

static void big_sub(Big* a, const Big* b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < 9; ++i)
    {
        uint64_t d = (uint64_t)a->w[i] - b->w[i] - borrow;
        a->w[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}

/* r = a * b mod n, by schoolbook multiplication then bit-serial reduction (runs once per sign-in) */
static void big_mulmod(Big* r, const Big* a, const Big* b, const Big* n)
{
    uint32_t p[16] = { 0 };
    for (int i = 0; i < 8; ++i)
    {
        uint64_t carry = 0;
        for (int j = 0; j < 8; ++j)
        {
            uint64_t t = (uint64_t)a->w[i] * b->w[j] + p[i + j] + carry;
            p[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        p[i + 8] = (uint32_t)carry;
    }
    Big rem;
    memset(&rem, 0, sizeof rem);
    for (int bit = 511; bit >= 0; --bit)
    {
        for (int i = 8; i > 0; --i) /* rem <<= 1 */
            rem.w[i] = (rem.w[i] << 1) | (rem.w[i - 1] >> 31);
        rem.w[0] = (rem.w[0] << 1) | ((p[bit / 32] >> (bit % 32)) & 1);
        if (big_ge(&rem, n))
            big_sub(&rem, n);
    }
    *r = rem;
}

int pol_rsa_session_key(const uint8_t cipher_be[32], uint8_t key[8])
{
    uint8_t le[32];
    for (int i = 0; i < 32; ++i)
        le[i] = cipher_be[31 - i];
    Big c, n, d, m;
    big_from_le(&c, le);
    big_from_le(&n, POL_RSA_N);
    big_from_le(&d, RSA_D);
    memset(&m, 0, sizeof m);
    m.w[0] = 1;
    for (int bit = 255; bit >= 0; --bit)
    {
        big_mulmod(&m, &m, &m, &n);
        if ((d.w[bit / 32] >> (bit % 32)) & 1)
            big_mulmod(&m, &m, &c, &n);
    }
    uint8_t be[32];
    for (int i = 0; i < 32; ++i)
        be[31 - i] = (uint8_t)(m.w[i / 4] >> (8 * (i % 4)));
    /* PKCS#1 v1.5 type 2: 00 02 <21 non-zero bytes> 00 <8-byte key> */
    if (be[0] != 0 || be[1] != 2 || be[23] != 0)
        return 0;
    for (int i = 0; i < 8; ++i)
        key[i] = be[31 - i]; /* the client keys Blowfish with the key bytes reversed */
    return 1;
}

uint64_t pol_rsa_iv(void)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)POL_RSA_N[i] << (8 * i);
    return v;
}
