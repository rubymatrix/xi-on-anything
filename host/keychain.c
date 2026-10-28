/* Saved sign-in passwords. See keychain.h. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "keychain.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#define SERVICE "FINAL FANTASY XI sign-in"

/* A query for key's item: class, service and account; the caller adds the rest and releases it. */
static CFMutableDictionaryRef query(const char* key)
{
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(NULL, SERVICE, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}

int keychain_get(const char* key, char* out, size_t n)
{
    CFMutableDictionaryRef q = query(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef data = NULL;
    OSStatus st = SecItemCopyMatching(q, &data);
    CFRelease(q);
    if (st != errSecSuccess || !data)
        return 0;
    CFIndex len = CFDataGetLength(data);
    int ok = len >= 0 && (size_t)len < n;
    if (ok)
    {
        memcpy(out, CFDataGetBytePtr(data), (size_t)len);
        out[len] = 0;
    }
    CFRelease(data);
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    CFDataRef data = CFDataCreate(NULL, (const UInt8*)password, (CFIndex)strlen(password));
    CFMutableDictionaryRef q = query(key);
    CFMutableDictionaryRef change =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(change, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, change);
    if (st == errSecItemNotFound)
    {
        CFDictionarySetValue(q, kSecValueData, data);
        st = SecItemAdd(q, NULL);
    }
    CFRelease(change);
    CFRelease(q);
    CFRelease(data);
    return st == errSecSuccess;
}

void keychain_delete(const char* key)
{
    CFMutableDictionaryRef q = query(key);
    SecItemDelete(q);
    CFRelease(q);
}

int keychain_available(void) { return 1; }

#elif defined(_WIN32) && !defined(FFXI_UWP) /* the UWP app has its own (xbox/app/Keychain.cpp) */
#include <windows.h>
#include <wincred.h>

/* Generic credentials named "<SERVICE>:<key>", the password as the blob */
#define SERVICE "FINAL FANTASY XI sign-in"

static void target(const char* key, char* out, size_t n)
{
    snprintf(out, n, "%s:%s", SERVICE, key);
}

int keychain_get(const char* key, char* out, size_t n)
{
    char t[512];
    target(key, t, sizeof t);
    PCREDENTIALA c = NULL;
    if (!CredReadA(t, CRED_TYPE_GENERIC, 0, &c))
        return 0;
    int ok = c->CredentialBlobSize < n;
    if (ok)
    {
        memcpy(out, c->CredentialBlob, c->CredentialBlobSize);
        out[c->CredentialBlobSize] = 0;
    }
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    char t[512];
    target(key, t, sizeof t);
    CREDENTIALA c;
    memset(&c, 0, sizeof c);
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = t;
    c.UserName = (LPSTR)key;
    c.CredentialBlobSize = (DWORD)strlen(password);
    c.CredentialBlob = (LPBYTE)password;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteA(&c, 0) != 0;
}

void keychain_delete(const char* key)
{
    char t[512];
    target(key, t, sizeof t);
    CredDeleteA(t, CRED_TYPE_GENERIC, 0);
}

int keychain_available(void) { return 1; }

#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <pthread.h>

/* libsecret's password calls, loaded at run time: the items carry the attributes service and
 * account (secret-tool search service "FINAL FANTASY XI sign-in" finds them). The layouts below are
 * libsecret's public ABI (secret-schema.h); gchar is char, gboolean int. */
#define SERVICE "FINAL FANTASY XI sign-in"

typedef struct
{
    const char* name;
    int type; /* SECRET_SCHEMA_ATTRIBUTE_STRING = 0 */
} SecretSchemaAttribute;

typedef struct
{
    const char* name;
    int flags; /* SECRET_SCHEMA_DONT_MATCH_NAME = 1 << 1 */
    SecretSchemaAttribute attributes[32];
    int reserved;
    void* reserved1[7];
} SecretSchema;

typedef struct
{
    uint32_t domain;
    int code;
    char* message;
} GError;

static const SecretSchema g_schema = {
    "org.ffxi.SignIn", 1 << 1, { { "service", 0 }, { "account", 0 }, { NULL, 0 } }, 0, { 0 },
};

static struct
{
    char* (*lookup)(const SecretSchema*, void*, GError**, ...);
    int (*store)(const SecretSchema*, const char*, const char*, const char*, void*, GError**, ...);
    int (*clear)(const SecretSchema*, void*, GError**, ...);
    void (*free_password)(char*);
    void (*error_free)(GError*);
} g_ss;
static int g_ss_loaded;
static pthread_once_t g_ss_once = PTHREAD_ONCE_INIT;

static void ss_load(void)
{
    void* h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return;
    *(void**)&g_ss.lookup = dlsym(h, "secret_password_lookup_sync");
    *(void**)&g_ss.store = dlsym(h, "secret_password_store_sync");
    *(void**)&g_ss.clear = dlsym(h, "secret_password_clear_sync");
    *(void**)&g_ss.free_password = dlsym(h, "secret_password_free");
    *(void**)&g_ss.error_free = dlsym(h, "g_error_free"); /* glib's, a dependency of libsecret */
    g_ss_loaded = g_ss.lookup && g_ss.store && g_ss.clear && g_ss.free_password && g_ss.error_free;
    if (!g_ss_loaded)
        dlclose(h);
}

static int ss(void)
{
    pthread_once(&g_ss_once, ss_load);
    return g_ss_loaded;
}

/* 1 if the call worked; a failure (no Secret Service, the keyring locked and not unlocked) is
 * logged */
static int ss_ok(GError* e, const char* what)
{
    if (!e)
        return 1;
    fprintf(stderr, "[keychain] %s: %s\n", what, e->message ? e->message : "failed");
    g_ss.error_free(e);
    return 0;
}

int keychain_get(const char* key, char* out, size_t n)
{
    if (!ss())
        return 0;
    GError* e = NULL;
    char* p = g_ss.lookup(&g_schema, NULL, &e, "service", SERVICE, "account", key, (char*)NULL);
    if (!ss_ok(e, "lookup") || !p)
        return 0;
    size_t len = strlen(p);
    int ok = len < n;
    if (ok)
        memcpy(out, p, len + 1);
    g_ss.free_password(p); /* clears it too */
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    if (!ss())
        return 0;
    char label[320];
    snprintf(label, sizeof label, "%s (%s)", SERVICE, key);
    GError* e = NULL;
    int ok = g_ss.store(&g_schema, NULL, label, password, NULL, &e, "service", SERVICE, "account", key, (char*)NULL);
    return ss_ok(e, "store") && ok;
}

void keychain_delete(const char* key)
{
    if (!ss())
        return;
    GError* e = NULL;
    g_ss.clear(&g_schema, NULL, &e, "service", SERVICE, "account", key, (char*)NULL);
    ss_ok(e, "clear");
}

int keychain_available(void) { return ss(); }

#elif !defined(FFXI_UWP)

int keychain_get(const char* key, char* out, size_t n)
{
    (void)key, (void)out, (void)n;
    return 0;
}

int keychain_set(const char* key, const char* password)
{
    (void)key, (void)password;
    return 0;
}

void keychain_delete(const char* key)
{
    (void)key;
}

int keychain_available(void) { return 0; }

#endif
