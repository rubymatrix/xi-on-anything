/* --cexi: client changes for a CatsEyeXI-style server, whose content runs past the retail ids
 * (cexi.c). Off by default: the game is left as it ships. */
#pragma once

enum
{
    CEXI_OFF,
    CEXI_ITEMS, /* custom item ids 0x7800-0xDFFF and gear model ids up to 4095 */
    CEXI_FULL,  /* and spell and job-ability ids up to 0xFFF, with their effects and motions */
};

/* "off", "items" or "full" -> CEXI_*; -1 if it is none */
int cexi_parse(const char* s);
/* After the images are loaded and before the game runs: makes the changes the mode asks for. Each
 * part checks the game holds what the retail client does first, and leaves it alone if not. game is
 * the FINAL FANTASY XI folder as the game sees it (full reads its menu DAT and file tables). */
void cexi_init(int mode, const char* game);
/* Each frame, on the game's thread (the Present hook). */
void cexi_frame(void);
