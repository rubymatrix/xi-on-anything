/* API 29 ELF TLS in a dlopen'ed Android library, without the game.
 * Built by tools/build_android.py tls-test [--native-tls]. The same source is
 * compiled as an executable and a shared library; TLS objects never cross the ABI.
 *
 * Existing pthreads first touch the module after dlopen. Three load/unload
 * cycles also exercise thread teardown and reused pthread identities. Nested
 * module -> executable -> module callbacks must retain each side's TLS.
 * This checks ABI/lifecycle behavior, not game fidelity or performance.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*TlsCallback)(uint64_t, unsigned);
typedef int (*TlsEntry)(uint64_t, unsigned, TlsCallback);

typedef struct
{
    uint64_t key, value;
} TlsSlot;

#if defined(XI_TLS_TEST_MODULE)
static _Thread_local TlsSlot slots[4096]; /* as many as runtime.c's per-thread lookup cache */
static _Thread_local uint64_t owner;
static _Thread_local unsigned calls, depth;
static _Thread_local uint64_t initialized = UINT64_C(0x7194302d38c5f6a1);

__attribute__((visibility("default"))) int xi_tls_exercise(uint64_t id, unsigned iteration, TlsCallback callback)
{
    unsigned slot = (iteration * 2053u) & 4095u;
    if (depth)
    {
        /* Reentrant callback sees the very same module thread state. */
        return owner != id || depth != 1 || calls != iteration + 1 || slots[slot].key != id ||
               slots[slot].value != iteration;
    }
    if (!calls)
    {
        if (owner || depth || initialized != UINT64_C(0x7194302d38c5f6a1))
            return 1;
        for (unsigned i = 0; i < 4096; ++i)
            if (slots[i].key || slots[i].value)
                return 1;
        owner = id;
    }
    if (owner != id || calls != iteration)
        return 1;
    ++calls;
    slots[slot].key = id;
    slots[slot].value = iteration;
    initialized = id;
    ++depth;
    int failed = callback(id, iteration);
    --depth;
    return failed || owner != id || initialized != id || slots[slot].key != id || slots[slot].value != iteration;
}
#elif defined(XI_TLS_TEST_DRIVER)
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define THREADS 4
#define ITERATIONS 8192u
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned ready, release_threads;
static TlsEntry entry;
static _Thread_local uint64_t driver_owner;
static _Thread_local unsigned driver_iteration;
static unsigned failures[THREADS];

static int callback(uint64_t id, unsigned iteration)
{
    if (driver_owner != id || driver_iteration != iteration)
        return 1;
    /* The second entry intentionally does not invoke callback again. */
    return entry(id, iteration, callback);
}

static void* worker(void* argument)
{
    unsigned number = (unsigned)(uintptr_t)argument;
    unsigned errors = 0;
    if (driver_owner || driver_iteration)
        ++errors;
    pthread_mutex_lock(&lock);
    ++ready;
    pthread_cond_broadcast(&changed);
    while (!release_threads)
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    if (entry)
    {
        driver_owner = (uint64_t)number + 1;
        for (driver_iteration = 0; driver_iteration < ITERATIONS; ++driver_iteration)
            errors += (unsigned)entry(driver_owner, driver_iteration, callback);
    }
    failures[number] = errors;
    return NULL;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: xi_tls_test /absolute/path/libxi_tls_test.so\n");
        return 2;
    }
    unsigned total = 0;
    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        pthread_t threads[THREADS];
        ready = release_threads = 0;
        entry = NULL;
        memset(failures, 0, sizeof failures);
        for (unsigned i = 0; i < THREADS; ++i)
        {
            int error = pthread_create(&threads[i], NULL, worker, (void*)(uintptr_t)i);
            if (error)
            {
                fprintf(stderr, "pthread_create: %s\n", strerror(error));
                /* Exiting prevents fixture-owned threads from hanging. */
                return 2;
            }
        }
        pthread_mutex_lock(&lock);
        while (ready != THREADS)
            pthread_cond_wait(&changed, &lock);
        void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!module)
        {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            return 2;
        }
        entry = (TlsEntry)dlsym(module, "xi_tls_exercise");
        if (!entry)
        {
            fprintf(stderr, "dlsym: %s\n", dlerror());
            return 2;
        }
        release_threads = 1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        for (unsigned i = 0; i < THREADS; ++i)
        {
            int error = pthread_join(threads[i], NULL);
            if (error)
            {
                fprintf(stderr, "pthread_join: %s\n", strerror(error));
                return 2;
            }
            total += failures[i];
        }
        entry = NULL;
        if (dlclose(module))
        {
            fprintf(stderr, "dlclose: %s\n", dlerror());
            return 2;
        }
    }
    printf("TLS fixture: %u calls, %u reentrant callbacks, 12 thread lifetimes, 3 dlopen cycles, %u failures\n",
           3 * THREADS * ITERATIONS, 3 * THREADS * ITERATIONS, total);
    return total ? 1 : 0;
}
#else
#error Define XI_TLS_TEST_MODULE or XI_TLS_TEST_DRIVER
#endif
