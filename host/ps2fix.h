/* The PS2 fixes that work in the game's own code (host/ps2fix.c); d3d8.c draws the rest. */
#ifndef XI_PS2FIX_H
#define XI_PS2FIX_H

/* Checks this build's sites against the game's bytes and sets the hooks; logs why not when it can't. */
void ps2fix_setup(void);

#endif
