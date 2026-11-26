/* The phone's code signature: the game library the phone links (ios_toolchain.cpp) signed with the
 * player's own development certificate, which iOS needs of any code an app loads. The certificate
 * and its key come as a .p12 (exported from Keychain Access for an Xcode account, or from AltStore,
 * SideStore or Impactor for a free one): the same team as the app's own signature (Sideloadly's, with
 * the same Apple Account), as iOS loads only libraries of the app's own team.
 *
 * What codesign writes for a library, made here: a CodeDirectory (SHA-256 of every 4 KB page, and of
 * the empty requirements), and a CMS signature over it with Apple's CDHashes attributes, the
 * certificate chain up to Apple's root (apple_certs.h) included. The library comes from lld with an
 * ad-hoc signature (-adhoc_codesign), which is replaced: its load command and __LINKEDIT grow to fit.
 *
 *   xi_sign(path, identifier, p12, size, password, err, n): 0 when signed
 *   xi_sign_check(p12, size, password, team, name, err, n): 0 when the .p12 opens; team (11 bytes
 *       and a 0) and name (the certificate's common name, 128 bytes) say whose it is */

#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#include <mach-o/loader.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apple_certs.h"

/* ---- a growable buffer and DER ---- */

typedef struct { uint8_t* p; size_t n, cap; } Buf;

static void put(Buf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap)
    {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    if (d)
        memcpy(b->p + b->n, d, n);
    else
        memset(b->p + b->n, 0, n);
    b->n += n;
}

static void put8(Buf* b, uint8_t v) { put(b, &v, 1); }
static void be32(Buf* b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 4);
}
static void be64(Buf* b, uint64_t v) { be32(b, (uint32_t)(v >> 32)); be32(b, (uint32_t)v); }
static void set32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* tag, length and contents */
static void tlv(Buf* b, uint8_t tag, const void* d, size_t n)
{
    put8(b, tag);
    if (n < 0x80)
        put8(b, (uint8_t)n);
    else
    {
        uint8_t l[4];
        int k = 0;
        for (size_t v = n; v; v >>= 8)
            l[k++] = (uint8_t)v;
        put8(b, (uint8_t)(0x80 | k));
        while (k)
            put8(b, l[--k]);
    }
    put(b, d, n);
}

/* b's contents wrapped in a tag, b emptied */
static void wrap(Buf* out, uint8_t tag, Buf* b)
{
    tlv(out, tag, b->p, b->n);
    free(b->p);
    memset(b, 0, sizeof *b);
}

static void oid(Buf* b, const char* dotted)
{
    uint8_t enc[32];
    size_t n = 0;
    unsigned long a = strtoul(dotted, (char**)&dotted, 10);
    unsigned long c = strtoul(dotted + 1, (char**)&dotted, 10);
    enc[n++] = (uint8_t)(a * 40 + c);
    while (*dotted == '.')
    {
        unsigned long v = strtoul(dotted + 1, (char**)&dotted, 10);
        uint8_t t[8];
        int k = 0;
        do
            t[k++] = v & 0x7f, v >>= 7;
        while (v);
        while (k)
        {
            --k;
            enc[n++] = (uint8_t)(t[k] | (k ? 0x80 : 0));
        }
    }
    tlv(b, 0x06, enc, n);
}

/* one DER element at p (of at most n bytes): its whole length, and where its contents start */
static int element(const uint8_t* p, size_t n, size_t* whole, size_t* head)
{
    if (n < 2)
        return 0;
    size_t len = p[1], h = 2;
    if (len & 0x80)
    {
        int k = len & 0x7f;
        if (k < 1 || k > 4 || n < 2u + k)
            return 0;
        len = 0;
        for (int i = 0; i < k; ++i)
            len = len << 8 | p[2 + i];
        h += k;
    }
    if (h + len > n)
        return 0;
    *whole = h + len;
    *head = h;
    return 1;
}

/* the certificate's tbsCertificate fields: serial, issuer and subject (whole elements) */
typedef struct { const uint8_t *serial, *issuer, *subject; size_t serial_n, issuer_n, subject_n; } CertParts;

static int cert_parts(const uint8_t* der, size_t n, CertParts* c)
{
    size_t whole, head;
    if (!element(der, n, &whole, &head)) /* Certificate */
        return 0;
    const uint8_t* p = der + head;
    size_t left = whole - head;
    if (!element(p, left, &whole, &head)) /* tbsCertificate */
        return 0;
    p += head, left = whole - head;
    if (p[0] == 0xa0) /* [0] version */
    {
        if (!element(p, left, &whole, &head))
            return 0;
        p += whole, left -= whole;
    }
    const uint8_t* fields[5];
    size_t sizes[5];
    for (int i = 0; i < 5; ++i) /* serial, signature, issuer, validity, subject */
    {
        if (!element(p, left, &whole, &head))
            return 0;
        fields[i] = p, sizes[i] = whole;
        p += whole, left -= whole;
    }
    c->serial = fields[0], c->serial_n = sizes[0];
    c->issuer = fields[2], c->issuer_n = sizes[2];
    c->subject = fields[4], c->subject_n = sizes[4];
    return 1;
}

/* a Name's attribute (by its OID's encoding) as a string */
static int name_attr(const uint8_t* name, size_t n, const uint8_t* want, size_t want_n, char* out, size_t out_n)
{
    size_t whole, head;
    if (!element(name, n, &whole, &head))
        return 0;
    const uint8_t* p = name + head;
    size_t left = whole - head;
    while (left)
    {
        size_t sw, sh, aw, ah, ow, oh, vw, vh;
        if (!element(p, left, &sw, &sh) || !element(p + sh, sw - sh, &aw, &ah))
            return 0;
        const uint8_t* a = p + sh + ah;
        if (element(a, aw - ah, &ow, &oh) && ow - oh == want_n && !memcmp(a + oh, want, want_n)
            && element(a + ow, aw - ah - ow, &vw, &vh))
        {
            size_t k = vw - vh < out_n - 1 ? vw - vh : out_n - 1;
            memcpy(out, a + ow + vh, k);
            out[k] = 0;
            return 1;
        }
        p += sw, left -= sw;
    }
    return 0;
}

static const uint8_t OID_OU[] = { 0x55, 0x04, 0x0b }, OID_CN[] = { 0x55, 0x04, 0x03 };

/* ---- the identity ---- */

typedef struct
{
    SecIdentityRef identity;
    SecKeyRef key;
    CFDataRef cert;
    CertParts parts;
    char team[16], name[128];
} Signer;

static void fail(char* err, size_t n, const char* fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int signer_open(Signer* s, const void* p12, size_t size, const char* password, char* err, size_t n)
{
    memset(s, 0, sizeof *s);
    CFDataRef data = CFDataCreate(NULL, p12, (CFIndex)size);
    CFStringRef pass = CFStringCreateWithCString(NULL, password ? password : "", kCFStringEncodingUTF8);
    const void* keys[] = { kSecImportExportPassphrase };
    const void* vals[] = { pass };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef items = NULL;
    OSStatus st = SecPKCS12Import(data, opts, &items);
    CFRelease(opts);
    CFRelease(pass);
    CFRelease(data);
    if (st != errSecSuccess || !items || CFArrayGetCount(items) < 1)
    {
        fail(err, n, st == errSecAuthFailed ? "The certificate's password is wrong." : "The certificate file didn't open (%d).", (int)st);
        if (items)
            CFRelease(items);
        return 0;
    }
    CFDictionaryRef first = CFArrayGetValueAtIndex(items, 0);
    s->identity = (SecIdentityRef)CFDictionaryGetValue(first, kSecImportItemIdentity);
    if (!s->identity)
    {
        CFRelease(items);
        fail(err, n, "The certificate file has no private key in it.");
        return 0;
    }
    CFRetain(s->identity);
    CFRelease(items);
    SecCertificateRef cert = NULL;
    if (SecIdentityCopyCertificate(s->identity, &cert) || SecIdentityCopyPrivateKey(s->identity, &s->key))
    {
        fail(err, n, "The certificate file's certificate or key didn't open.");
        return 0;
    }
    s->cert = SecCertificateCopyData(cert);
    CFRelease(cert);
    if (!cert_parts(CFDataGetBytePtr(s->cert), (size_t)CFDataGetLength(s->cert), &s->parts))
    {
        fail(err, n, "The certificate didn't read.");
        return 0;
    }
    name_attr(s->parts.subject, s->parts.subject_n, OID_OU, sizeof OID_OU, s->team, sizeof s->team);
    name_attr(s->parts.subject, s->parts.subject_n, OID_CN, sizeof OID_CN, s->name, sizeof s->name);
    if (!s->team[0])
    {
        fail(err, n, "The certificate names no team: it isn't a development certificate.");
        return 0;
    }
    return 1;
}

static void signer_close(Signer* s)
{
    if (s->key)
        CFRelease(s->key);
    if (s->cert)
        CFRelease(s->cert);
    if (s->identity)
        CFRelease(s->identity);
}

int xi_sign_check(const void* p12, size_t size, const char* password, char* team, char* name, char* err, size_t n)
{
    Signer s;
    int ok = signer_open(&s, p12, size, password, err, n);
    if (ok)
    {
        snprintf(team, 12, "%s", s.team);
        snprintf(name, 128, "%s", s.name);
    }
    signer_close(&s);
    return ok ? 0 : 1;
}

/* ---- the CMS signature over the CodeDirectory ---- */

static const char* b64(const uint8_t* d, size_t n, char* out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* o = out;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *o++ = T[v >> 18 & 63], *o++ = T[v >> 12 & 63];
        *o++ = i + 1 < n ? T[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? T[v & 63] : '=';
    }
    *o = 0;
    return out;
}

static int cmp_der(const void* a, const void* b)
{
    const Buf *x = a, *y = b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->p, y->p, n);
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

static void attribute(Buf* out, const char* type, Buf* value)
{
    Buf a = { 0 }, set = { 0 };
    oid(&a, type);
    wrap(&set, 0x31, value);
    put(&a, set.p, set.n);
    free(set.p);
    wrap(out, 0x30, &a);
}

static int cms(Signer* s, const uint8_t* cd, size_t cd_n, Buf* out, char* err, size_t n)
{
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(cd, (CC_LONG)cd_n, digest);
    int ec = 0;
    CFDictionaryRef ka = SecKeyCopyAttributes(s->key);
    if (ka)
    {
        CFTypeRef type = CFDictionaryGetValue(ka, kSecAttrKeyType);
        ec = type && CFEqual(type, kSecAttrKeyTypeECSECPrimeRandom);
        CFRelease(ka);
    }

    /* the signed attributes, DER-sorted */
    Buf attrs[5] = { { 0 } };
    {
        Buf v = { 0 };
        oid(&v, "1.2.840.113549.1.7.1");
        attribute(&attrs[0], "1.2.840.113549.1.9.3", &v); /* contentType: data */
        char t[16];
        time_t now = time(NULL);
        strftime(t, sizeof t, "%y%m%d%H%M%SZ", gmtime(&now));
        tlv(&v, 0x17, t, strlen(t));
        attribute(&attrs[1], "1.2.840.113549.1.9.5", &v); /* signingTime */
        tlv(&v, 0x04, digest, sizeof digest);
        attribute(&attrs[2], "1.2.840.113549.1.9.4", &v); /* messageDigest */
        char plist[1024], h[64];
        snprintf(plist, sizeof plist,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n\t<key>cdhashes</key>\n\t<array>\n\t\t<data>\n\t\t%s\n\t\t</data>\n\t</array>\n</dict>\n</plist>\n",
            b64(digest, 20, h));
        tlv(&v, 0x04, plist, strlen(plist));
        attribute(&attrs[3], "1.2.840.113635.100.9.1", &v); /* Apple's CDHashes */
        Buf seq = { 0 };
        oid(&seq, "2.16.840.1.101.3.4.2.1");
        tlv(&seq, 0x04, digest, sizeof digest);
        wrap(&v, 0x30, &seq);
        attribute(&attrs[4], "1.2.840.113635.100.9.2", &v); /* Apple's CDHashes2: SHA-256 */
    }
    qsort(attrs, 5, sizeof *attrs, cmp_der);
    Buf signed_attrs = { 0 };
    for (int i = 0; i < 5; ++i)
        put(&signed_attrs, attrs[i].p, attrs[i].n), free(attrs[i].p);

    /* the signature over them, as a SET */
    Buf as_set = { 0 };
    tlv(&as_set, 0x31, signed_attrs.p, signed_attrs.n);
    CFDataRef msg = CFDataCreate(NULL, as_set.p, (CFIndex)as_set.n);
    free(as_set.p);
    CFErrorRef e = NULL;
    CFDataRef sig = SecKeyCreateSignature(s->key, ec ? kSecKeyAlgorithmECDSASignatureMessageX962SHA256 : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
        msg, &e);
    CFRelease(msg);
    if (!sig)
    {
        fail(err, n, "The key didn't sign (%ld).", e ? (long)CFErrorGetCode(e) : 0L);
        if (e)
            CFRelease(e);
        free(signed_attrs.p);
        return 0;
    }

    Buf alg = { 0 }, tmp = { 0 };
    oid(&tmp, "2.16.840.1.101.3.4.2.1");
    tlv(&tmp, 0x05, NULL, 0);
    wrap(&alg, 0x30, &tmp); /* sha256 AlgorithmIdentifier */

    Buf si = { 0 };
    uint8_t one = 1;
    tlv(&si, 0x02, &one, 1);
    put(&tmp, s->parts.issuer, s->parts.issuer_n);
    put(&tmp, s->parts.serial, s->parts.serial_n);
    wrap(&si, 0x30, &tmp);
    put(&si, alg.p, alg.n);
    tlv(&si, 0xa0, signed_attrs.p, signed_attrs.n);
    free(signed_attrs.p);
    oid(&tmp, ec ? "1.2.840.10045.4.3.2" : "1.2.840.113549.1.1.1");
    if (!ec)
        tlv(&tmp, 0x05, NULL, 0);
    wrap(&si, 0x30, &tmp);
    tlv(&si, 0x04, CFDataGetBytePtr(sig), (size_t)CFDataGetLength(sig));
    CFRelease(sig);

    Buf sd = { 0 };
    tlv(&sd, 0x02, &one, 1);
    tlv(&sd, 0x31, alg.p, alg.n);
    oid(&tmp, "1.2.840.113549.1.7.1");
    wrap(&sd, 0x30, &tmp);
    /* the certificates: the player's, then Apple's intermediate that issued it and the roots */
    put(&tmp, CFDataGetBytePtr(s->cert), (size_t)CFDataGetLength(s->cert));
    for (size_t i = 0; i < sizeof APPLE_CERTS / sizeof *APPLE_CERTS; ++i)
    {
        CertParts c;
        if (cert_parts(APPLE_CERTS[i].der, APPLE_CERTS[i].size, &c) && c.subject_n == s->parts.issuer_n
            && !memcmp(c.subject, s->parts.issuer, c.subject_n))
        {
            put(&tmp, APPLE_CERTS[i].der, APPLE_CERTS[i].size);
            for (size_t j = 0; j < sizeof APPLE_CERTS / sizeof *APPLE_CERTS; ++j)
            {
                CertParts r;
                if (cert_parts(APPLE_CERTS[j].der, APPLE_CERTS[j].size, &r) && r.subject_n == c.issuer_n
                    && !memcmp(r.subject, c.issuer, r.subject_n))
                    put(&tmp, APPLE_CERTS[j].der, APPLE_CERTS[j].size);
            }
            break;
        }
    }
    wrap(&sd, 0xa0, &tmp);
    wrap(&tmp, 0x30, &si);
    wrap(&sd, 0x31, &tmp);
    free(alg.p);

    Buf ci = { 0 };
    oid(&ci, "1.2.840.113549.1.7.2");
    Buf inner = { 0 };
    wrap(&inner, 0x30, &sd);
    wrap(&ci, 0xa0, &inner);
    wrap(out, 0x30, &ci);
    return 1;
}

/* ---- the Mach-O ---- */

#define PAGE_SHIFT_CS 12
#define PAGE_CS (1u << PAGE_SHIFT_CS)
#define RESERVE 18000 /* the CMS blob's room: the certificates and the signature (about 6 KB) */

int xi_sign(const char* path, const char* ident, const void* p12, size_t size, const char* password, char* err, size_t n)
{
    Signer s;
    if (!signer_open(&s, p12, size, password, err, n))
    {
        signer_close(&s);
        return 1;
    }
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        fail(err, n, "%s didn't open.", path);
        signer_close(&s);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    Buf file = { 0 };
    put(&file, NULL, (size_t)len);
    size_t got = fread(file.p, 1, (size_t)len, f);
    fclose(f);
    int rc = 1;
    struct mach_header_64* mh = (struct mach_header_64*)file.p;
    if (got != (size_t)len || len < (long)sizeof *mh || mh->magic != MH_MAGIC_64)
    {
        fail(err, n, "%s isn't a 64-bit Mach-O.", path);
        goto out;
    }
    struct linkedit_data_command* sigcmd = NULL;
    struct segment_command_64 *linkedit = NULL, *text = NULL;
    uint8_t* lc = file.p + sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds; ++i)
    {
        struct load_command* c = (struct load_command*)lc;
        if (c->cmd == LC_CODE_SIGNATURE)
            sigcmd = (struct linkedit_data_command*)c;
        else if (c->cmd == LC_SEGMENT_64 && !strcmp(((struct segment_command_64*)c)->segname, "__LINKEDIT"))
            linkedit = (struct segment_command_64*)c;
        else if (c->cmd == LC_SEGMENT_64 && !strcmp(((struct segment_command_64*)c)->segname, "__TEXT"))
            text = (struct segment_command_64*)c;
        lc += c->cmdsize;
    }
    if (!sigcmd || !linkedit || !text || sigcmd->dataoff + sigcmd->datasize != (uint32_t)len
        || linkedit->fileoff + linkedit->filesize != (uint64_t)len)
    {
        fail(err, n, "%s has no signature at its end to replace (link it with -adhoc_codesign).", path);
        goto out;
    }
    uint32_t limit = sigcmd->dataoff, slots = (limit + PAGE_CS - 1) / PAGE_CS;
    size_t ident_n = strlen(ident) + 1, team_n = strlen(s.team) + 1, nspecial = 2;
    size_t cd_head = 88, hash_off = cd_head + ident_n + team_n + nspecial * 32;
    size_t cd_n = hash_off + (size_t)slots * 32;
    uint32_t total = (uint32_t)((36 + cd_n + 12 + 8 + RESERVE + 15) & ~(size_t)15);

    /* the file's new size in its header first: the page hashes cover the header */
    file.n = limit;
    put(&file, NULL, total);
    mh = (struct mach_header_64*)file.p;
    lc = file.p + sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds; ++i)
    {
        struct load_command* c = (struct load_command*)lc;
        if (c->cmd == LC_CODE_SIGNATURE)
            sigcmd = (struct linkedit_data_command*)c;
        else if (c->cmd == LC_SEGMENT_64 && !strcmp(((struct segment_command_64*)c)->segname, "__LINKEDIT"))
            linkedit = (struct segment_command_64*)c;
        else if (c->cmd == LC_SEGMENT_64 && !strcmp(((struct segment_command_64*)c)->segname, "__TEXT"))
            text = (struct segment_command_64*)c;
        lc += c->cmdsize;
    }
    sigcmd->datasize = total;
    linkedit->filesize = limit + total - linkedit->fileoff;
    linkedit->vmsize = (linkedit->filesize + 0x3fff) & ~(uint64_t)0x3fff;

    Buf req = { 0 };
    be32(&req, 0xfade0c01), be32(&req, 12), be32(&req, 0);

    Buf cd = { 0 };
    be32(&cd, 0xfade0c02);
    be32(&cd, (uint32_t)cd_n);
    be32(&cd, 0x20400);
    be32(&cd, 0);                      /* flags */
    be32(&cd, (uint32_t)hash_off);
    be32(&cd, (uint32_t)cd_head);      /* identOffset */
    be32(&cd, (uint32_t)nspecial);
    be32(&cd, slots);
    be32(&cd, limit);
    put8(&cd, 32), put8(&cd, 2 /* SHA-256 */), put8(&cd, 0), put8(&cd, PAGE_SHIFT_CS);
    be32(&cd, 0);                      /* spare2 */
    be32(&cd, 0);                      /* scatterOffset */
    be32(&cd, (uint32_t)(cd_head + ident_n)); /* teamOffset */
    be32(&cd, 0);                      /* spare3 */
    be64(&cd, 0);                      /* codeLimit64 */
    be64(&cd, text->fileoff);          /* execSegBase */
    be64(&cd, text->filesize);         /* execSegLimit */
    be64(&cd, 0);                      /* execSegFlags: a library */
    put(&cd, ident, ident_n);
    put(&cd, s.team, team_n);
    uint8_t h[32];
    CC_SHA256(req.p, (CC_LONG)req.n, h);
    put(&cd, h, 32);                   /* slot -2: the requirements */
    put(&cd, NULL, 32);                /* slot -1: no Info.plist */
    for (uint32_t i = 0; i < slots; ++i)
    {
        uint32_t lo = i * PAGE_CS, k = limit - lo < PAGE_CS ? limit - lo : PAGE_CS;
        CC_SHA256(file.p + lo, k, h);
        put(&cd, h, 32);
    }

    Buf sig = { 0 };
    if (!cms(&s, cd.p, cd.n, &sig, err, n))
    {
        free(cd.p), free(req.p);
        goto out;
    }
    uint32_t used = (uint32_t)(36 + cd.n + req.n + 8 + sig.n);
    if (used > total)
    {
        fail(err, n, "The signature didn't fit (%u of %u bytes).", used, total);
        free(cd.p), free(req.p), free(sig.p);
        goto out;
    }
    uint8_t* sb = file.p + limit;
    set32(sb, 0xfade0cc0);
    set32(sb + 4, used);
    set32(sb + 8, 3);
    uint32_t at = 36;
    set32(sb + 12, 0), set32(sb + 16, at);
    memcpy(sb + at, cd.p, cd.n), at += (uint32_t)cd.n;
    set32(sb + 20, 2), set32(sb + 24, at);
    memcpy(sb + at, req.p, req.n), at += (uint32_t)req.n;
    set32(sb + 28, 0x10000), set32(sb + 32, at);
    set32(sb + at, 0xfade0b01), set32(sb + at + 4, (uint32_t)(8 + sig.n));
    memcpy(sb + at + 8, sig.p, sig.n);
    free(cd.p), free(req.p), free(sig.p);

    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.signing", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(file.p, 1, file.n, f) != file.n)
    {
        if (f)
            fclose(f);
        fail(err, n, "%s couldn't be written.", tmp);
        goto out;
    }
    fclose(f);
    if (rename(tmp, path))
    {
        fail(err, n, "%s couldn't be replaced.", path);
        goto out;
    }
    rc = 0;
out:
    free(file.p);
    signer_close(&s);
    return rc;
}
