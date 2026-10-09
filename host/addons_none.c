/* The addon host's interface (host/addons/addons.h) for builds without it: the browser build
 * (tools/build_web.py), where LuaJIT can't run. Nothing loads; Config > Addons has no page; the
 * packet taps see nothing (the Mog House detection and CatsEyeXI's that ride on them stay off).
 * The browser's addons will be its own (HTML and JS over the canvas: docs/web-port-plan.md). */
#include "addons/addons.h"

void addons_init(const AddonsSetup* s) { (void)s; }
void addons_frame(void) {}
void addons_shutdown(void) {}
const struct ModernAddons* addons_menu(void) { return 0; }
int addons_harness(const char* script)
{
    (void)script;
    return 1;
}
void addons_packet_tap(void (*tap)(const uint8_t* p, size_t n)) { (void)tap; }
void addons_packet_send(const uint8_t* p, size_t n) { (void)p, (void)n; }

/* What host64 takes from the addon host's game-struct reader (game.c) and DAT reader (res.c), and
 * Discord's presence, which rides on both: none of them in this build. The sun shadows then follow
 * the camera, not the character. */
#include <stdint.h>
int32_t xi_game_player_index(void) { return -1; }
uint32_t xi_game_entity(uint32_t index) { (void)index; return 0; }
int xi_game_rd(uint32_t a, void* out, uint32_t n) { (void)a, (void)out, (void)n; return 0; }
void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n) { (void)game_dir, (void)overlays, (void)n; }
void discord_init(void) {}
void discord_frame(void) {}
void discord_signin_frame(void) {}
