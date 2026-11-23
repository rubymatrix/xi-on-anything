/* A PlayOnline sign-in for the sign-in screen (signin.c), on PlayOnline's own servers. Its
 * implementation is not in this repository: a build links one in when XI_POL names its folder
 * (docs/building.md), and defines FFXI_POL; without it the screen signs in to LandSandBoat servers
 * only. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SIGNIN_POL_AUTH 0x34 /* the authCode block the game sends its lobby (gamecore slot 936) */

/* Signs in with the Square Enix ID and its password, the PlayOnline ID and its password, and the
 * one-time password (NULL or empty for none), and gets the game's session value and authCode, as
 * host64's --session and --auth take them. The session stays open for the rest of the run.
 * Blocking: call it off the screen's thread. 1 on success, else err says why. */
int signin_pol(const char* pol_id, const char* password, const char* sqex_id, const char* sqex_password,
    const char* otp, uint8_t v[16], uint8_t auth[SIGNIN_POL_AUTH], char* err, size_t n);

/* The game's PlayOnline requests (gamecore_set_request_host: the status requests of slots 195/196
 * and the in-game message client's list, read and delete), on the session signin_pol opened. begin
 * returns an id >= 0 or a negative error; poll returns 0 while pending, then the result with the
 * reply's payload in out. Neither blocks: the session's own thread sends the request. */
int signin_pol_request_begin(const char* verb, const uint8_t* data, uint32_t n);
int signin_pol_request_poll(int32_t id, uint8_t* out, uint32_t cap, uint32_t* got);

/* The member as the session keeps it, as retail's core hands it to the game: the content table (64
 * records of 0x68 bytes, slot 177), the handle table (64 of 0x28, slot 173), the status (handle,
 * character, login mode, OpenStat, contents class), the member's id (slot 283) and the key its ids
 * are kept under. For gamecore_set_contents, _handles, _presence, _member_id and _member_key. 1 if there is a session. */
int signin_pol_state(uint8_t contents[64 * 0x68], uint8_t handles[64 * 0x28], int32_t status[5], uint64_t* id,
    uint64_t* key);

/* Brings the install at host_game up to the newest FINAL FANTASY XI, as the launcher and the
 * Viewer do before they start the game: PlayOnline's lobby takes only the newest client. Then
 * fails if its FFXiMain.dll is not the one this client was recompiled from (sha256: 64 hex digits;
 * NULL for no check): only a new build of the client runs another. progress, when not NULL, gets
 * a line to show and how far along it is (0 to 1, or -1 when unknown). Blocking: call it off the
 * screen's thread. 1 on success, else err says why. */
int signin_pol_update(const char* host_game, const char* sha256,
    void (*progress)(void* ctx, const char* message, float fraction), void* ctx, char* err, size_t n);

/* 1 if pol_id is a PlayOnline ID (8 letters and digits, upper case) */
int signin_pol_id_valid(const char* pol_id);
