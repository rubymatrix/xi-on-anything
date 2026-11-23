/* What the host tells our own gamecore (gamecore_slots.c). */
#pragma once

#include <stdint.h>

/* Register the implemented slots; call before gamecore_init. */
void gamecore_slots_init(void); /* session and lobby (gamecore_slots.c) */
void gamecore_files_init(void); /* paths, files, registry, timers (gamecore_files.c) */
void gamecore_presence_init(void); /* presence client, records, mail, text (gamecore_presence.c) */

/* The viewer folder as the guest sees it ("C:\...\<viewer>"): the root of every path the
 * core hands out (slot 126). */
void gamecore_set_root(const char* guest_viewer_dir);
/* A patch.ver (0x120 bytes, encrypted as retail's with no registry key) carrying the client
 * version string, e.g. "30260903_0", for an install that ships none (private-server installs
 * launched without the viewer). 0 if the string does not fit. */
int gamecore_make_patch_ver(const char* version, uint8_t out[0x120]);
/* The AFK logout timer's check, run from the per-frame pump (gamecore_files.c). */
void gamecore_idle_tick(void);

/* The session value V for this sign-in: 16 bytes, no NUL, byte-identical to the session value in
 * LSB's account table (LSB's lobby checks the passwords built from it). */
void gamecore_set_session(const uint8_t v[16]);
/* The 0x34-byte authCode block slot 936 hands the game (it goes into the lobby's RequestLobbyLogin
 * and RequestSelectChr), as the launcher that signed in made it. Opaque bytes here; zeros when
 * none is given. */
void gamecore_set_auth_block(const uint8_t block[0x34]);
/* PlayOnline's content table for slot 177, as retail's core keeps it (0x10403080): 64 records of
 * 0x68 bytes by record number (the launcher's pol_content_table). Without it slot 177 builds
 * records from the game's own character list, as a LandSandBoat sign-in needs. */
void gamecore_set_contents(const uint8_t table[64 * 0x68]);
/* PlayOnline's handle table for slot 173 (0x10405800: 64 records of 0x28 bytes, by handle number,
 * the content slots at +0x20; the launcher's pol_handle_table), and the member's id for slot 283 as
 * retail's core keeps it (pol_member_id). Without them: one handle 0, and a fixed id. */
void gamecore_set_handles(const uint8_t table[64 * 0x28]);
void gamecore_set_member_id(uint64_t id);
/* Kept for hosts that still pass it; the PlayOnline client, which answers the requests, uses its own. */
void gamecore_set_member_key(uint64_t key);
/* Where the game's PlayOnline requests go, for a PlayOnline sign-in: begin(verb, data) returns an
 * id >= 0 or a negative error; poll(id) returns 0 while pending, then the result (> 0 done, < 0
 * failed) with the reply's payload in out (*got bytes, at most cap). Verbs: "status" (slot 195's
 * six int32s; payload V and the authCode when the request went out), "gmsglist" (u16 cap; payload
 * the 0x48-byte headers of the member's PlayOnline messages, as the game holds them), "gmsgread"
 * and "gmsgdelete" (such a header; payload the message body), "msgname" (such a header; payload
 * its 96-character file name) and "msgheader" (a file name; payload its header), "contentlist" (no
 * data; payload the member's content table, 64 records of 0x68, as slot 177 hands them out). Called
 * on the game thread with the guest lock held: neither may block. */
typedef struct GamecoreRequestHost
{
    int32_t (*begin)(void* ctx, const char* verb, const uint8_t* data, uint32_t n);
    int32_t (*poll)(void* ctx, int32_t id, uint8_t* out, uint32_t cap, uint32_t* got);
    void* ctx;
} GamecoreRequestHost;
void gamecore_set_request_host(const GamecoreRequestHost* host);
/* The path of polcore's GetPath id (slot 126), guest form. */
void gamecore_path(uint32_t id, char* out, size_t n);
/* The member's status as PlayOnline's core keeps it after sign-in (the Viewer's status request and
 * Load MyStatus): handle, character, login mode, OpenStat, contents class, -1 for none. What slots
 * 189/190/204/206/207 report until the game's own requests change it. */
void gamecore_set_presence(int32_t handle, int32_t chr, int32_t mode, int32_t openstat, int32_t contents_class);
/* The lobby's IPv4 address (host byte order), which the lobby's host name resolves to. Default
 * 127.0.0.1; the hosts set it to their account server (host64: --server). */
void gamecore_set_lobby(uint32_t ipv4_host_order);
/* Resolve the lobby's host name through this instead (a sign-in with --session: the name's real
 * address, as retail does); NULL goes back to the address above. It is called with the guest lock
 * held and returns 1 with the IPv4 in host byte order, 0 if the name does not resolve. */
void gamecore_set_lobby_resolver(int (*resolve)(const char* name, uint32_t* ipv4_host_order));
/* The command line gamecore reports (GetlpCmdLine): empty, or " /game eAZcFcB -net 3
 * -port <lobby view port>" for a LandSandBoat sign-in. Before gamecore_init. */
void gamecore_set_cmdline(const char* text);
/* The exit code and message the game left (slots 969/1026), after GameStart returns. */
int32_t gamecore_exit_code(const char** message);
