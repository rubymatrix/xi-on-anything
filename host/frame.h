/* FFXiMain past 60 fps (frame.c): the frame delta's users get the real delta, in sixtieths of a
 * second, instead of at least one whole sixtieth a frame, so the world keeps its speed at any frame
 * rate. On only while the player's frame rate is Uncapped (divisor 0); 30 and 60 fps, and a
 * divisor an addon asks for (FastCS's 0, which runs cutscenes fast), stay as the game does them -
 * but at any divisor 0 the connection's wait for the server keeps the wall clock's pace, or a zone
 * change times out (frame_net_wait). */
#pragma once

#include <stdint.h>

/* Installs the hooks (a build tools/frame_sites.py has not seen keeps the game's pacing) and
 * reads FFXI_FPS_LIMIT (a cap in frames a second while uncapped, over the player's) and FFXI_FRAMELOG=1. */
void frame_setup(void);
/* From the Present hook: whether the player's uncapped frame rate is in effect this frame, whether
 * the menus out of the world keep their 30 fps pace (the player's 60 fps or uncapped there), the
 * player's cap while uncapped (Frame Rate 120, 144; 0 none), which this holds frames to - waiting out
 * each frame to an even pace (FFXI_FPS_LIMIT, when set, takes its place) - and whether the game's
 * divisor is 0 this frame, the player's or an addon's (the connection's wait for the server then
 * keeps the wall clock's pace: frame_net_wait). */
void frame_present(int uncapped, int menus30, uint32_t limit, int unpaced);
/* Whether this build can run uncapped at the game's speed (Config > Display offers it then). */
int frame_supported(void);
