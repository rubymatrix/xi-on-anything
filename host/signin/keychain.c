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
    if (st != errSecSuccess) /* -34018 errSecMissingEntitlement: the app has no application-identifier */
        fprintf(stderr, "[keychain] not saved: OSStatus %d\n", (int)st);
    return st == errSecSuccess;
}

void keychain_delete(const char* key)
{
    CFMutableDictionaryRef q = query(key);
    SecItemDelete(q);
    CFRelease(q);
}

int keychain_available(void) { return 1; }

#elif defined(_WIN32) && !defined(FFXI_UWP) /* the UWP app has its own (platforms/xbox/app/Keychain.cpp) */
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

#elif defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>

/* The activity's keychainGet/Set/Delete (MainActivity.java): an AES key the Android Keystore keeps,
 * the passwords only ever stored encrypted with it, in the app's private preferences. Any thread:
 * SDL attaches it to the VM, and the activity's own class is used (an attached thread's FindClass
 * can't see the app's classes). */
static JNIEnv* activity(jobject* out, jclass* cls)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    *out = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!*out)
        return NULL;
    *cls = (*env)->GetObjectClass(env, *out);
    return env;
}

static void done(JNIEnv* env, jobject act, jclass cls)
{
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

int keychain_get(const char* key, char* out, size_t n)
{
    jobject act;
    jclass cls;
    JNIEnv* env = activity(&act, &cls);
    if (!env)
        return 0;
    int ok = 0;
    jmethodID m = (*env)->GetMethodID(env, cls, "keychainGet", "(Ljava/lang/String;)[B");
    jstring k = m ? (*env)->NewStringUTF(env, key) : NULL;
    jbyteArray data = k ? (jbyteArray)(*env)->CallObjectMethod(env, act, m, k) : NULL;
    if (data && !(*env)->ExceptionCheck(env))
    {
        jsize len = (*env)->GetArrayLength(env, data);
        jbyte* bytes = (*env)->GetByteArrayElements(env, data, NULL);
        ok = bytes && len >= 0 && (size_t)len < n;
        if (ok)
        {
            memcpy(out, bytes, (size_t)len);
            out[len] = 0;
        }
        if (bytes)
        {
            memset(bytes, 0, (size_t)len);
            (*env)->ReleaseByteArrayElements(env, data, bytes, 0); /* the zeros go back to the array */
        }
        (*env)->DeleteLocalRef(env, data);
    }
    if (k)
        (*env)->DeleteLocalRef(env, k);
    done(env, act, cls);
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    jobject act;
    jclass cls;
    JNIEnv* env = activity(&act, &cls);
    if (!env)
        return 0;
    int ok = 0;
    jmethodID m = (*env)->GetMethodID(env, cls, "keychainSet", "(Ljava/lang/String;[B)Z");
    jstring k = m ? (*env)->NewStringUTF(env, key) : NULL;
    jsize len = (jsize)strlen(password);
    jbyteArray data = k ? (*env)->NewByteArray(env, len) : NULL;
    if (data)
    {
        (*env)->SetByteArrayRegion(env, data, 0, len, (const jbyte*)password);
        ok = (*env)->CallBooleanMethod(env, act, m, k, data) && !(*env)->ExceptionCheck(env); /* it zeroes data */
        (*env)->DeleteLocalRef(env, data);
    }
    if (k)
        (*env)->DeleteLocalRef(env, k);
    done(env, act, cls);
    if (!ok)
        fprintf(stderr, "[keychain] not saved\n");
    return ok;
}

void keychain_delete(const char* key)
{
    jobject act;
    jclass cls;
    JNIEnv* env = activity(&act, &cls);
    if (!env)
        return;
    jmethodID m = (*env)->GetMethodID(env, cls, "keychainDelete", "(Ljava/lang/String;)V");
    jstring k = m ? (*env)->NewStringUTF(env, key) : NULL;
    if (k)
    {
        (*env)->CallVoidMethod(env, act, m, k);
        (*env)->DeleteLocalRef(env, k);
    }
    done(env, act, cls);
}

/* the activity is there to ask (not in the offscreen tests) */
int keychain_available(void)
{
    jobject act;
    jclass cls;
    JNIEnv* env = activity(&act, &cls);
    if (!env)
        return 0;
    int ok = (*env)->GetMethodID(env, cls, "keychainSet", "(Ljava/lang/String;[B)Z") != NULL;
    done(env, act, cls);
    return ok;
}

#elif defined(__SWITCH__)
#include <fcntl.h>
#include <stdlib.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

/* The Switch has no keychain, but its security engine has keys. Each password is a file on the SD card
 * (data/keychain/), encrypted (AES-CTR) and signed (AES-CMAC) with keys the console derives from its own
 * device key through spl:mig: the keys never leave the security engine, so a file copied to a computer
 * or to another console can't be read or changed unseen. Where spl:mig isn't there to use (an emulator),
 * there is no store and the player types the password, as before. The sign-in calls these from its own
 * threads: no stdio here (on the Switch it is the main thread's). */
#define KC_DIR "sdmc:/switch/xi-on-anything/data/keychain"
#define KC_PLAIN 128 /* a length byte, the password and zeros: every file the same size */
#define KC_MAGIC 0x314B4958u /* "XIK1" */

/* the sources the engine's keys are derived from: fixed, so the same console derives the same keys */
static const u8 KC_KEK[16] = { 'x', 'i', '-', 'o', 'n', '-', 'a', 'n', 'y', 't', 'h', 'i', 'n', 'g', ':', 'k' };
static const u8 KC_ENC[16] = { 'k', 'e', 'y', 'c', 'h', 'a', 'i', 'n', ':', 'e', 'n', 'c', 'r', 'y', 'p', 't' };
static const u8 KC_MAC[16] = { 'k', 'e', 'y', 'c', 'h', 'a', 'i', 'n', ':', 'c', 'm', 'a', 'c', ':', 'v', '1' };

typedef struct KcFile
{
    u32 magic;
    u8 ctr[16];
    u8 ct[KC_PLAIN];
    u8 mac[16]; /* CMAC of ctr and ct */
} KcFile;

static Mutex g_kc;

/* out = CTR(in) under the console's encryption key; mac = CMAC(ctr || ct) under its signing key, ct
 * being out when encrypting and in when decrypting */
static int kc_engine(int encrypt, const u8 ctr[16], const u8* in, u8* out, u8 mac[16])
{
    if (R_FAILED(splCryptoInitialize()))
        return 0;
    /* the engine's buffers in pages of their own */
    u8* buf = (u8*)aligned_alloc(0x1000, 0x1000);
    u8 sealed[16];
    u32 slot = 0;
    int ok = 0, locked = 0;
    if (!buf)
        goto out;
    if (R_FAILED(splCryptoGenerateAesKek(KC_KEK, 0, 1 /* device unique */, sealed)))
        goto out;
    if (R_FAILED(splCryptoLockAesEngine(&slot)))
        goto out;
    locked = 1;
    u8 *src = buf, *dst = buf + 0x400, *signed_part = buf + 0x800;
    memcpy(src, in, KC_PLAIN);
    if (R_FAILED(splCryptoLoadAesKey(sealed, KC_ENC, slot)) || R_FAILED(splCryptoCryptAesCtr(src, dst, KC_PLAIN, slot, ctr)))
        goto out;
    memcpy(signed_part, ctr, 16);
    memcpy(signed_part + 16, encrypt ? dst : src, KC_PLAIN);
    if (R_FAILED(splCryptoLoadAesKey(sealed, KC_MAC, slot)) || R_FAILED(splCryptoComputeCmac(signed_part, 16 + KC_PLAIN, slot, mac)))
        goto out;
    memcpy(out, dst, KC_PLAIN);
    ok = 1;
out:
    if (locked)
        splCryptoUnlockAesEngine(slot);
    if (buf)
    {
        memset(buf, 0, 0x1000);
        free(buf);
    }
    splCryptoExit();
    return ok;
}

/* KC_DIR/<key>, the key's characters other than letters, digits and . - _ @ as '_' */
static void kc_path(const char* key, char* out, size_t n)
{
    size_t i = 0;
    for (const char* s = KC_DIR "/"; *s && i + 1 < n; ++s)
        out[i++] = *s;
    for (const char* s = key; *s && i + 5 < n; ++s)
    {
        char c = *s;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                   c == '_' || c == '@';
        out[i++] = keep ? c : '_';
    }
    memcpy(out + i, ".bin", 5);
}

int keychain_get(const char* key, char* out, size_t n)
{
    char path[512];
    kc_path(key, path, sizeof path);
    KcFile f;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t got = read(fd, &f, sizeof f);
    close(fd);
    if (got != (ssize_t)sizeof f || f.magic != KC_MAGIC)
        return 0;
    u8 plain[KC_PLAIN] = { 0 }, mac[16] = { 0 };
    mutexLock(&g_kc);
    int ok = kc_engine(0, f.ctr, f.ct, plain, mac);
    mutexUnlock(&g_kc);
    /* the signature compared in full, whatever differs first */
    u8 diff = 0;
    for (int i = 0; i < 16; ++i)
        diff |= (u8)(mac[i] ^ f.mac[i]);
    size_t len = plain[0];
    ok = ok && !diff && len < KC_PLAIN && len < n;
    if (ok)
    {
        memcpy(out, plain + 1, len);
        out[len] = 0;
    }
    memset(plain, 0, sizeof plain);
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    size_t len = strlen(password);
    if (len >= KC_PLAIN)
        return 0;
    u8 plain[KC_PLAIN];
    memset(plain, 0, sizeof plain);
    plain[0] = (u8)len;
    memcpy(plain + 1, password, len);
    KcFile f;
    f.magic = KC_MAGIC;
    randomGet(f.ctr, sizeof f.ctr);
    mutexLock(&g_kc);
    int ok = kc_engine(1, f.ctr, plain, f.ct, f.mac);
    mutexUnlock(&g_kc);
    memset(plain, 0, sizeof plain);
    if (!ok)
        return 0;
    mkdir("sdmc:/switch/xi-on-anything/data", 0777);
    mkdir(KC_DIR, 0777);
    char path[512];
    kc_path(key, path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    ok = write(fd, &f, sizeof f) == (ssize_t)sizeof f;
    close(fd);
    if (!ok)
        unlink(path);
    return ok;
}

void keychain_delete(const char* key)
{
    char path[512];
    kc_path(key, path, sizeof path);
    unlink(path);
}

/* the engine derives the console's key: asked once */
int keychain_available(void)
{
    static int known = -1;
    if (known < 0)
    {
        mutexLock(&g_kc);
        u8 sealed[16];
        known = R_SUCCEEDED(splCryptoInitialize()) ? 1 : 0;
        if (known)
        {
            known = R_SUCCEEDED(splCryptoGenerateAesKek(KC_KEK, 0, 1, sealed));
            splCryptoExit();
        }
        mutexUnlock(&g_kc);
    }
    return known;
}

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
