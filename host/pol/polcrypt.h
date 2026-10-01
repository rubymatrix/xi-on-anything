/* PlayOnline's wire primitives, client side: the sqIrc line framing and checksum, the custom
 * base64/base32 alphabets, MD5 (with the host's zeroed-IV continuation), the sqIrc Blowfish
 * variant and its OFB line cipher, and the RSA-256 session-key transport.
 *
 * Ported from a reference implementation validated against the retail and PS2 hosts;
 * tests/polcrypt_test.c checks this port against vectors that reference produced. Portable C11, no OS
 * calls: the same code runs on macOS and Windows. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* --- line framing: content + 4 checksum characters + CRLF --------------------------------------- */
uint32_t pol_sum32(const uint8_t* data, size_t n);
void pol_chk4(const uint8_t* content, size_t n, uint8_t out[4]);

/* --- the custom alphabets ------------------------------------------------------------------------- */
size_t pol_b64enc(const uint8_t* in, size_t n, char* out);          /* no padding; returns length */
size_t pol_b64dec(const char* in, size_t n, uint8_t* out);
size_t pol_b32enc(const uint8_t* in, size_t n, char* out);          /* 8 characters per 5 bytes */

/* --- MD5 ------------------------------------------------------------------------------------------ */
void pol_md5(const uint8_t* msg, size_t n, uint8_t out[16]);
/* MD5 with the chaining values zeroed (the host's MD5Final clears its context, so the Blowfish key
 * schedule's later digests start from 0,0,0,0) */
void pol_md5_zero_iv(const uint8_t* msg, size_t n, uint8_t out[16]);

/* --- the sqIrc Blowfish variant ------------------------------------------------------------------- */
typedef struct PolBlowfish
{
    uint32_t P[18];
    uint32_t S[4][256];
} PolBlowfish;

void pol_bf_init(PolBlowfish* bf, const uint8_t* key, size_t n);
uint64_t pol_bf_enc64(const PolBlowfish* bf, uint64_t v);

/* The sqIrc line cipher: OFB from the IV, restarted every line; CR and LF pass through. */
void pol_line_crypt(const PolBlowfish* bf, uint64_t iv, const uint8_t* in, uint8_t* out, size_t n);

/* A resumable OFB stream: polpro restarts it at the IV for each header and continues into the body. */
typedef struct PolOfb
{
    const PolBlowfish* bf;
    uint64_t state;
    uint32_t counter;
} PolOfb;

void pol_ofb_start(PolOfb* s, const PolBlowfish* bf, uint64_t iv);
void pol_ofb_crypt(PolOfb* s, const uint8_t* in, uint8_t* out, size_t n);

/* --- RSA-256 ---------------------------------------------------------------------------------------
 * The client's modulus goes out in USER (custom base64 of its 32 bytes, little-endian); the server
 * returns the 8-byte session key RSA-encrypted under it (e = 0xFFFF, PKCS#1 v1.5 type 2) in numeric
 * 300. The low 8 bytes of the modulus are the cipher IV. */
extern const uint8_t POL_RSA_N[32]; /* little-endian */
/* Decrypts the 300 token's 32 big-endian bytes and returns the Blowfish key (the 8 key bytes,
 * reversed, as the client keys it). 0 if the padding is wrong. */
int pol_rsa_session_key(const uint8_t cipher_be[32], uint8_t key[8]);
uint64_t pol_rsa_iv(void);
