/* TF-PSA-Crypto's user configuration for the Switch build (TF_PSA_CRYPTO_USER_CONFIG_FILE, set by
 * tools/build_switch.py for mbedtls and for the code that includes its headers): Horizon is neither Unix
 * nor Windows, so the time and the entropy come from platforms/switch/tls_nx.c instead of the built-in
 * sources. Also mbedtls' own user configuration (MBEDTLS_USER_CONFIG_FILE): its socket module is
 * Unix's or Windows', and host/lsb_login.c passes its own socket calls (mbedtls_ssl_set_bio). */
#undef MBEDTLS_NET_C
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY
#define MBEDTLS_PLATFORM_MS_TIME_ALT
