/* Our own gamecore's function-table slots (R3). Every slot here follows its specification in
 * specs/gamecore-slots.*.txt (read from the retail gamecore and
 * checked against the live R3.0 call log); the comment on each names the retail function.
 *
 * Every slot FFXI uses is cdecl: the caller pops the arguments, so slots return with RETC.
 *
 * Session and lobby (gamecore-slots.session.txt). FFXiMain's lobby bring-up (0x100ed7f0) calls,
 * in order: 818 pump, 221/222 resolve the lobby's host name, 195/196 the presence status request
 * (the request), 936 the authCode block, 1003 the session value V. On retail, V is the first 16
 * bytes of the account server's the request reply; here the host supplies it (gamecore_set_session),
 * byte-identical to the session value in LSB's account table, which LSB's lobby checks. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gamecore.h"
#include "gamecore_config.h"

static uint8_t g_session[16];
static int g_have_session;
static uint8_t g_auth_block[0x34]; /* slot 936; zeros unless the host was given one */
static uint32_t g_lobby_ipv4 = 0x7F000001u; /* host byte order */
static int (*g_lobby_resolve)(const char* name, uint32_t* ipv4);
static int g_lobby_resolved; /* slot 221's answer: 1 found, -1 not */
static uint32_t g_lobby_found;
static int32_t g_exit_code;
static char g_exit_message[1024];
static int32_t g_status_request = -1; /* the request host's id for the request 196 polls */

void gamecore_set_session(const uint8_t v[16])
{
    memcpy(g_session, v, 16);
    g_have_session = 1;
}


void gamecore_set_auth_block(const uint8_t block[0x34])
{
    memcpy(g_auth_block, block, sizeof g_auth_block);
}

void gamecore_set_lobby(uint32_t ipv4_host_order)
{
    g_lobby_ipv4 = ipv4_host_order;
}

void gamecore_set_lobby_resolver(int (*resolve)(const char* name, uint32_t* ipv4_host_order))
{
    g_lobby_resolve = resolve;
}

int32_t gamecore_exit_code(const char** message)
{
    if (message)
        *message = g_exit_message;
    return g_exit_code;
}

/* 818 (0x10045480): the online session pump. Retail returns 1 while its engine is uninitialised,
 * which FFXiMain always accepts; there is no PlayOnline session inside this process. */
static void s818_pump(Guest* g) { gamecore_idle_tick(); RETC(1); }

/* 705 (0x1004a420): the same pump, per frame; it also runs the AFK logout timer. */
static void s705_pump_frame(Guest* g) { gamecore_idle_tick(); RETC(1); }

/* 221 (0x1000ff40): start resolving a host name; returns a handle. The only caller asks for
 * the lobby's host name: it resolves to the configured lobby address, never through DNS (the
 * R3.0 runs showed public DNS sends it to Square Enix) - unless the host gave a resolver for a
 * sign-in with --session, which looks the name up for real. Then it resolves here, at once. */
static void s221_dns_begin(Guest* g)
{
    if (g_lobby_resolve)
    {
        char name[256];
        snprintf(name, sizeof name, "%s", ARGS(0));
        g_lobby_resolved = g_lobby_resolve(name, &g_lobby_found) ? 1 : -1;
        rt_log("[recomp] gamecore: lobby %s -> %s%u.%u.%u.%u\n", name, g_lobby_resolved > 0 ? "" : "(not found) ",
            g_lobby_found >> 24, (g_lobby_found >> 16) & 255, (g_lobby_found >> 8) & 255, g_lobby_found & 255);
    }
    RETC(0);
}

/* 222 (0x100101e0): poll a resolve. 1 = done: out[0..0x14) = { u16 1, u16 0, u32 IPv4 in host
 * byte order, zeros }. */
static void s222_dns_poll(Guest* g)
{
    uint32_t out = ARG(1);
    if (g_lobby_resolve && g_lobby_resolved < 0)
        RETC((uint32_t)-1); /* the lobby's name did not resolve: the bring-up fails, as retail's */
    memset(GUEST_PTR(out), 0, 0x14);
    wr16(out, 1);
    wr32(out + 4, g_lobby_resolve ? g_lobby_found : g_lobby_ipv4);
    RETC(1);
}

/* 195 (0x1001db60): begin the presence status request (handle, chr, mode, openstat, friendauth,
 * class; -2 = keep). Range checks as retail. With a status host (a PlayOnline sign-in) the request
 * goes to PlayOnline as retail's does, the request on the member's session: at the lobby (class 1,
 * no character), at character select (the handle and the character in it) and on leaving the world
 * (no character). Without one it needs no network. */
static void s195_status_begin(Guest* g)
{
    int32_t handle = (int32_t)ARG(0), chr = (int32_t)ARG(1), mode = (int32_t)ARG(2), openstat = (int32_t)ARG(3);
    if (handle < -2 || handle >= 0x40)
        RETC(0xFFFFE3EAu); /* -7190 */
    if (chr < -2 || chr >= 0x40)
        RETC(0xFFFFE3FDu); /* -7171 */
    if (mode != -2 && mode != 0 && mode != 1)
        RETC(0xFFFFE3EFu); /* -7185 */
    if (openstat != -2 && (openstat < 0 || openstat > 4))
        RETC(0xFFFFE3EFu);
    /* what 189/190/204/206/207 report from now on (gamecore_presence.c) */
    presence_update(handle, chr, mode, openstat, (int32_t)ARG(5));
    const GamecoreRequestHost* host = gamecore_request_host();
    if (host)
    {
        int32_t args[6] = { handle, chr, mode, openstat, (int32_t)ARG(4), (int32_t)ARG(5) };
        uint8_t data[24];
        for (int i = 0; i < 6; ++i)
            for (int b = 0; b < 4; ++b)
                data[4 * i + b] = (uint8_t)((uint32_t)args[i] >> (8 * b));
        int32_t id = host->begin(host->ctx, "status", data, sizeof data);
        rt_log("[recomp] gamecore: status request %d (handle %d, chr %d, mode %d, openstat %d, friendauth %d, class %d)\n",
            id, args[0], args[1], args[2], args[3], args[4], args[5]);
        if (id < 0)
            RETC((uint32_t)id);
        g_status_request = id;
    }
    RETC(0);
}

/* 196 (0x1001df10): poll it. 1 = complete, with V available to 1003; a reply that went over the
 * network brings a new V and authCode, as retail's (state 5 copies them out of every reply). Without
 * a session value the lobby cannot log in: report "not signed in" (-515), as retail does without
 * the online session's crypto. */
static void s196_status_poll(Guest* g)
{
    const GamecoreRequestHost* host = gamecore_request_host();
    if (host && g_status_request >= 0)
    {
        uint8_t reply[16 + 0x34];
        uint32_t got = 0;
        int32_t r = host->poll(host->ctx, g_status_request, reply, sizeof reply, &got);
        int has_v = got == sizeof reply;
        const uint8_t *v = reply, *auth = reply + 16;
        if (r == 0)
            RETC(0);
        g_status_request = -1;
        if (r < 0)
        {
            rt_log("[recomp] gamecore: the status request failed (%d)\n", r);
            RETC((uint32_t)r);
        }
        if (has_v)
        {
            gamecore_set_session(v);
            gamecore_set_auth_block(auth);
        }
    }
    if (!g_have_session)
    {
        rt_log("[recomp] gamecore: the lobby asked for the session value, but the host has not supplied one\n");
        RETC(0xFFFFFDFDu);
    }
    RETC(1);
}

/* 936 (0x10020020): the 0x34-byte authCode block, which FFXiMain puts into RequestLobbyLogin
 * (0x26) and hashes into RequestSelectChr (0x07): whatever the host was given
 * (gamecore_set_auth_block: a launcher that signed in made it), zeros otherwise. LSB reads none of it; the call succeeding is what matters
 * there, since it switches FFXiMain to hashing V as a fixed 16 bytes. */
static void s936_auth_block(Guest* g)
{
    memcpy(ARGP(0), g_auth_block, sizeof g_auth_block);
    RETC(0);
}

/* 1003 (0x1001c870): the 16-byte session value V. */
static void s1003_session_value(Guest* g)
{
    memcpy(ARGP(0), g_session, 16);
    RETC(16);
}

/* 1080 (0x10012920): the client's UDP base port. */
static void s1080_udp_port(Guest* g) { RETC(54090); }

/* 689 (0x1004ccf0): the online clock: *out = Unix seconds; returns 1. */
static void s689_online_time(Guest* g)
{
    wr32(ARG(0), (uint32_t)time(NULL));
    RETC(1);
}

/* 838 (0x10046010): sign-in state; 3 = signed in (0 raises a connection error at shutdown). */
static void s838_signin_state(Guest* g) { RETC(3); }

/* 421 (0x10018200): region/language; 1 = the US/English client. */
static void s421_language(Guest* g) { RETC(1); }

/* 460 (0x10044380): a flag only the viewer sets; its value without a viewer. */
static void s460_viewer_flag(Guest* g) { RETC(0xFFFFFFFFu); }

/* 969 (0x1004a440) / 1026 (0x1004a450): the exit code, and a message, for the host to read
 * after GameStart returns. */
static void s969_set_exit(Guest* g)
{
    g_exit_code = (int32_t)ARG(0);
    g_exit_message[0] = 0;
    RETC(0);
}

static void s1026_set_exit_message(Guest* g)
{
    g_exit_code = (int32_t)ARG(0);
    if (!ARG(1))
    {
        g_exit_message[0] = 0;
        RETC(0);
    }
    strncpy(g_exit_message, ARGS(1), sizeof g_exit_message - 1);
    g_exit_message[sizeof g_exit_message - 1] = 0;
    RETC(0x3FF);
}

/* 981 (0x10039630) / 982 (0x1003963a): the "-patch" path: a buffer size, and a fill that writes
 * nothing. */
static void s981_patch_size(Guest* g) { RETC(0x10000); }
static void s982_patch_fill(Guest* g) { RETC(ARG(0) ? 1u : 0xFFFFD000u); }

static const GamecoreSlot SESSION[] = {
    { 0xcc8, s818_pump },
    { 0xb04, s705_pump_frame },
    { 0x374, s221_dns_begin },
    { 0x378, s222_dns_poll },
    { 0x30c, s195_status_begin },
    { 0x5e0, s195_status_begin }, /* the same function sits in both slots on retail */
    { 0x310, s196_status_poll },
    { 0xea0, s936_auth_block },
    { 0xfac, s1003_session_value },
    { 0x10e0, s1080_udp_port },
    { 0xac4, s689_online_time },
    { 0xd18, s838_signin_state },
    { 0x694, s421_language },
    { 0x730, s460_viewer_flag },
    { 0xf24, s969_set_exit },
    { 0x1008, s1026_set_exit_message },
    { 0xf54, s981_patch_size },
    { 0xf58, s982_patch_fill },
    { 0, NULL },
};

void gamecore_slots_init(void)
{
    gamecore_register(SESSION);
}
