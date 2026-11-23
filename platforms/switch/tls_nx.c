/* What mbedtls (the LandSandBoat sign-in's TLS, host/lsb_login.c) needs from the platform on the Switch
 * (platforms/switch/mbedtls_config.h): a millisecond clock and an entropy source. */
#include <time.h>

#include <switch.h>

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (mbedtls_ms_time_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* csrng, the system's random service; else libnx's generator, seeded from the kernel's entropy
 * (svcGetInfo RandomEntropy) */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t* estimate_bits, unsigned char* output,
    size_t output_size)
{
    (void)flags;
    static int csrng = -1;
    if (csrng < 0)
        csrng = R_SUCCEEDED(csrngInitialize());
    if (!csrng || R_FAILED(csrngGetRandomBytes(output, output_size)))
        randomGet(output, output_size);
    *estimate_bits = 8 * output_size;
    return 0;
}
