/* What platforms/switch/switch_main.c tells platforms/switch/sdl_nx.c (the Switch's stand-in for SDL3), as
 * uwp_bridge.h does for the Xbox app and web_bridge.h for the browser's shell. */
#pragma once

/* The screen's size in pixels: 1280x720 handheld, 1920x1080 docked. A change while the game runs is a
 * window size event. */
void nx_set_view_size(int w, int h);

/* The face buttons: 0 Nintendo's (A confirms, B cancels), 1 Xbox's by position (B, the bottom one, confirms). */
void nx_set_layout(int xbox);
