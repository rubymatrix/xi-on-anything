/* --cexi: client changes for a CatsEyeXI-style server, whose content runs past the retail ids
 * (cexi.c). Off by default: the game is left as it ships. */
#pragma once

enum
{
    CEXI_OFF,
    CEXI_ITEMS, /* custom item ids 0x7800-0xDFFF and gear model ids up to 4095 */
};

/* "off" or "items" -> CEXI_*; -1 if it is neither */
int cexi_parse(const char* s);
/* After the images are loaded and before the game runs: makes the changes the mode asks for. Each
 * part checks the game holds what the retail client does first, and leaves it alone if not. */
void cexi_init(int mode);
