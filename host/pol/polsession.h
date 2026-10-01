/* The host's PlayOnline session: what replaces the PlayOnline Viewer's sign-in so that
 * FINAL FANTASY XI can log in on a server with PlayOnline behind it.
 *
 *   1. sqIrc sign-in on the PlayOnline IRC port (P-IRC, 51240): NOTICE AUTH -> USER (our RSA-256
 *      modulus) -> numeric 300 (the session key, RSA-wrapped) -> NICK, encrypted from there on.
 *      The server knows the member from the nick, which encodes the PlayOnline ID. The session
 *      stays open while the player plays: polpro requests are attributed to a member by which
 *      live sqIrc key decrypts them, and it is how the server reaches the member.
 *   2. polpro cmd 4 sub 5 (51220) with contents class 1: the reply's first 16 bytes are the FFXI
 *      session value V. The server issues one V per sign-in and publishes it to
 *      pol_accounts.session_value, which the lobby checks (MD5(V || K+n) in every passwd).
 *
 * Portable C11 over polnet (Winsock / BSD sockets). Blocking; one session per object. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "polcrypt.h"
#include "polnet.h"

#define POL_PORT_IRC 51240
#define POL_PORT_POLPRO 51220
/* PlayOnline's own servers: the polpro host the retail client dials first (pp%03d.pol.com, index 0).
 * The sign-in screen signs in here for a PlayOnline account; there is no server to pick. */
#define POL_HOST "pp000.pol.com"
#define POL_CONTENTS_FFXI 1

typedef struct PolSession
{
    char host[256];
    PolSocket irc;
    PolBlowfish bf;
    uint64_t iv;
    int encrypted; /* from numeric 300 on, every line is enciphered */
    char pol_id[9], nick[10];
    uint8_t inbuf[4096];
    size_t inlen;
    char error[256];
} PolSession;

/* PlayOnline ID ("ABCD1234") -> the 9-character sign-in nick ("US77G53VV"); 0 if not an ID. */
int pol_nick_from_id(const char* pol_id, char nick[10]);

/* Signs in. The password travels as the retail client sends it (MD5 with the greeting token);
 * whether the server checks it is its business. 1 on success, else s->error says why. */
int pol_signin(PolSession* s, const char* host, uint16_t irc_port, const char* pol_id, const char* password);

/* Answers the server's keepalive PINGs and reads whatever it sent; call every few seconds while
 * signed in (the retail Viewer's session is dropped at 400 s without it). 0 once the connection
 * has closed. */
int pol_pump(PolSession* s, int timeout_ms);

/* polpro cmd 4 sub 5, contents class 1: V, the FFXI session value. 1 on success. */
int pol_ffxi_session_value(PolSession* s, uint16_t polpro_port, uint8_t v[16]);

void pol_signout(PolSession* s);
