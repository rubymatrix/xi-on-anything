/* The PlayOnline client's primitives (host/pol/polcrypt.c) against a reference implementation
 * validated on the retail and PS2 hosts: every expected value below came from it on the same inputs. */
#include <stdio.h>
#include <string.h>

#include "polcrypt.h"
#include "polsession.h"

static int failures;

static void check(int ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static int hex_eq(const uint8_t* b, size_t n, const char* hex)
{
    char s[256];
    for (size_t i = 0; i < n; ++i)
        snprintf(s + 2 * i, 3, "%02x", b[i]);
    return !strcmp(s, hex);
}

int main(void)
{
    uint8_t ck[4];
    pol_chk4((const uint8_t*)"PING :pol", 9, ck);
    check(!memcmp(ck, "GjyB", 4), "line checksum");
    check(pol_sum32((const uint8_t*)"hello world", 11) == 0x404ff7d7u, "sum32 with a tail");

    uint8_t bytes[20];
    for (int i = 0; i < 20; ++i)
        bytes[i] = (uint8_t)i;
    char text[64];
    pol_b64enc(bytes, 20, text);
    check(!strcmp(text, "TTIGTt7nSAY3G7iK8TdO8pT9IUo"), "custom base64 encode");
    uint8_t back[32];
    check(pol_b64dec(text, strlen(text), back) == 20 && !memcmp(back, bytes, 20), "custom base64 round trip");
    pol_b32enc(bytes, 15, text);
    check(!strcmp(text, "NNNEVNZVNFONS3NY41HEZO1S"), "custom base32 encode");

    uint8_t md[16];
    pol_md5((const uint8_t*)"abc", 3, md);
    check(hex_eq(md, 16, "900150983cd24fb0d6963f7d28e17f72"), "MD5");
    pol_md5_zero_iv((const uint8_t*)"abc", 3, md);
    check(hex_eq(md, 16, "c974bc19183bcf1b909e7f07ea1e8fbb"), "MD5 from a zeroed context");

    static PolBlowfish bf;
    const uint8_t key[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    pol_bf_init(&bf, key, 8);
    check(pol_bf_enc64(&bf, 0) == 0xc6e03b0eb7e710d8ull, "Blowfish variant, block 0");
    check(pol_bf_enc64(&bf, 123456789) == 0x502fe7662aa102a9ull, "Blowfish variant, block 123456789");
    const char* line = "NICK US77G53VV:abc:x\r\n";
    uint8_t out[64];
    pol_line_crypt(&bf, 0x1122334455667788ull, (const uint8_t*)line, out, strlen(line));
    check(hex_eq(out, strlen(line), "2a588819e384a0a80fdbda8693cca4097dcdeefa0d0a"), "line cipher (CR/LF pass through)");

    char nick[10];
    check(pol_nick_from_id("ABCD1234", nick) && !strcmp(nick, "US77G53VV"), "PlayOnline ID -> nick");

    /* numeric 300 as the server builds it for our modulus, key bytes 01..08 */
    const char* token = "GShsn33oq7cyPZAJoSdFnfBWn7yCYjFgz3ihBC9geYo";
    uint8_t cipher[40], session[8];
    check(pol_b64dec(token, strlen(token), cipher) == 32 && pol_rsa_session_key(cipher, session) &&
              hex_eq(session, 8, "0807060504030201"),
        "RSA-256 session key (numeric 300)");

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
