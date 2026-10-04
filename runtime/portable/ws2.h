/* WS2_32 on host sockets (ws2.c). */
#pragma once

#include <stdint.h>

/* Starts host networking and registers the shims (before the images are mapped). */
void ws2_init(void);
/* Where the game's hosts are: gethostbyname answers the game's domain and every name under it with
 * this IPv4 address (host byte order) instead of asking DNS, whose answer is Square Enix's. */
void ws2_set_game_server(uint32_t ipv4_host_order);
/* A DNS server (IPv4, host byte order) to ask for every name instead of the host's DNS; 0 for the
 * host's. The game's hosts still go to ws2_set_game_server's address when one is set. */
void ws2_set_dns(uint32_t ipv4_host_order);
/* name's IPv4 (host byte order) through the host's DNS (or ws2_set_dns's server); 0 if it has none. Call with the guest lock
 * held: it is let go while DNS answers. gamecore's lobby resolver for a sign-in with --session. */
int ws2_resolve_ipv4(const char* name, uint32_t* ipv4_host_order);
/* A LandSandBoat sign-in (host/lsb_login.c): every lobby command FFXiMain sends to the login
 * server's data or view port carries this session hash at +12, where the server looks for it. */
void ws2_set_lobby_session(const uint8_t hash[16], uint16_t data_port, uint16_t view_port);
