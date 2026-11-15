/* The addon host's interface (host/addons/addons.h) for builds without it: the browser build
 * (tools/build_web.py), where LuaJIT can't run. Nothing loads; Config > Addons has no page; the
 * packet taps see nothing (the Mog House detection and CatsEyeXI's that ride on them stay off).
 * The browser's addons will be its own (HTML and JS over the canvas). */
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
size_t xi_packet_last(int outgoing, uint16_t id, uint8_t* out, size_t cap, uint64_t* when_ms)
{
    (void)outgoing, (void)id, (void)out, (void)cap, (void)when_ms;
    return 0;
}

/* What host64 takes from the addon host's game-struct reader (game.c) and DAT reader (res.c), and
 * Discord's presence, which rides on both: none of them in this build. The sun shadows then follow
 * the camera, not the character. */
#include <stdint.h>
int32_t xi_game_player_index(void) { return -1; }
uint32_t xi_game_entity(uint32_t index) { (void)index; return 0; }
int xi_game_rd(uint32_t a, void* out, uint32_t n) { (void)a, (void)out, (void)n; return 0; }
/* Read as "in game" by frame.c, so the world keeps its speed at 60 fps; out of the world the menus
 * then run at the player's pace, not held to their 30 fps. */
int xi_game_login_status(void) { return 2; }
void xi_res_setup(const char* game_dir, const char* const* overlays, unsigned n) { (void)game_dir, (void)overlays, (void)n; }
void discord_init(void) {}
void discord_frame(void) {}
void discord_signin_frame(void) {}

/* Whether the player is typing in the chat line (the Switch shows its keyboard then: host64.c), without
 * the addon host's hooks.c: the chat manager, found as hooks.c finds it (a pointer in the code that
 * loads it), and its state field 4 or 5 while the line is typed in. */
#include <string.h>

#include "gwin.h"
#include "runtime.h"

static int committed(uint32_t a, uint32_t n)
{
    if (a < 0x10000u || (uint64_t)a + n > 0xFFF00000ull)
        return 0;
    for (uint32_t p = a & ~0xFFFu; p <= ((a + n - 1) & ~0xFFFu); p += 0x1000)
        if (!gwin_is_committed(p))
            return 0;
    return 1;
}

static uint32_t chat_manager(void)
{
    static uint32_t global;
    if (!global && rt_image_lo) /* looked for once the game's code is there */
    {
        /* 8B0D???????? 6A?? 5353 8B11 536A?? 6A?? 536A: mov ecx, [global] ... */
        static const uint8_t want[] = { 0x8B, 0x0D, 0, 0, 0, 0, 0x6A, 0, 0x53, 0x53, 0x8B, 0x11, 0x53, 0x6A, 0, 0x6A, 0, 0x53, 0x6A };
        static const uint8_t any[] = { 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0 };
        uint32_t text = rt_image_lo + rt_image_text_rva, size = rt_image_text_size;
        global = 0xFFFFFFFFu;
        if (committed(text, size))
        {
            const uint8_t* m = GUEST_PTR(text);
            for (uint32_t i = 0; i + sizeof want <= size; ++i)
            {
                uint32_t k = 0;
                while (k < sizeof want && (any[k] || m[i + k] == want[k]))
                    ++k;
                if (k == sizeof want)
                {
                    global = rd32(text + i + 2);
                    break;
                }
            }
        }
    }
    if (!global || global == 0xFFFFFFFFu || !committed(global, 4))
        return 0;
    uint32_t m = rd32(global);
    return m && committed(m, 0xF110) ? m : 0;
}

int xi_chat_typing(void)
{
    uint32_t m = chat_manager();
    uint32_t state = m ? rd32(m + 0xF108) : 0;
    return state == 4 || state == 5;
}
