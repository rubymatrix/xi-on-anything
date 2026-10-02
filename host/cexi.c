/* --cexi: client changes for a CatsEyeXI-style server, whose content runs past the retail ids.
 *
 * --cexi items: custom item ids 0x7800-0xDFFF and gear model ids up to 4095. The server sends the
 * ids (its MAX_ITEMID is 0xE000); the player's DAT overlay (--dats) carries the extended Monstrosity
 * item DAT (ROM/288/80.DAT English, 79.DAT Japanese: 0xC00-byte records, record n is id 0x7400 + n,
 * the custom ones from record 1024) and the expanded FTABLE/VTABLE with the gear files. Four changes,
 * found in each build by tools/cexi_sites.py (meta/builds.json, 2026-09-03's in the comments):
 *
 *  - The item range table (cexi_item_ranges, 0x10372150): 20-byte entries {file, first id, end id, 0,
 *    handler}, the first an id falls in wins. Monstrosity's 0x7400-0x7800 becomes 0x7400-0xE000, so
 *    the custom ids read its (extended) DAT, with the general items' handler.
 *  - That handler (wrap cexi_item_general, 0x1015c0a0; cdecl, the item at [esp+4], its record from
 *    +0xb0, the id first) sends a custom id to the handler of its kind by sub-range: 0x7800 general,
 *    0x8800 usable, 0x9800 armour, 0xB800 weapon, 0xD800-0xDFFF furnishing. The handlers are the
 *    table's own (the entries from 0, 0x1000, 0x2800, 0x4000 and 0x7000).
 *  - Two code constants (patch group cexi_items): the inventory's gate (0x101bb112), which hid ids
 *    0x7400-0xFFFE, hides 0xE000-0xFFFE; and the auction house list (0x101d848f) compares its last
 *    item id unsigned, so the cursor stays on an item of 0x8000 or up.
 *  - The gear groups (cexi_gear_groups, 0x1035d878): 8 races x 9 slots (face, head, body, hands,
 *    legs, feet, main, sub, ranged) x 6 groups of {base file id, count}; a model id is counted off
 *    group by group. The last group of head..sub becomes the rest of 4096 models, at
 *    128240 + (race * 9 + slot) * 4096 + model id (the file ids the expanded tables give them).
 *
 * Each part checks the game holds what the retail client does (or already the change) and is left
 * alone otherwise, with a line in the log saying why. */
#include <stdio.h>
#include <string.h>

#include "build.h"
#include "cexi.h"
#include "runtime.h"

#define CUSTOM_END 0xE000u

int cexi_parse(const char* s)
{
    if (!strcmp(s, "off"))
        return CEXI_OFF;
    if (!strcmp(s, "items"))
        return CEXI_ITEMS;
    return -1;
}

/* ---- the item range table ---- */
enum
{
    KIND_GENERAL,
    KIND_USABLE,
    KIND_ARMOUR,
    KIND_WEAPON,
    KIND_FURNISHING,
    KINDS
};
static uint32_t g_handler[KINDS]; /* the game's handler for each kind */

#ifdef FFXI_WRAP_CEXI_ITEM_GENERAL
extern GuestFn rt_wrap_cexi_item_general;
extern const GuestFn rt_orig_cexi_item_general;

static void item_general(Guest* g)
{
    static const uint16_t from[KINDS] = { 0x7800, 0x8800, 0x9800, 0xB800, 0xD800 };
    uint32_t id = rd16(rt_arg(g, 0) + 0xB0);
    int kind = KIND_GENERAL;
    if (id < CUSTOM_END)
        while (kind + 1 < KINDS && id >= from[kind + 1])
            ++kind;
    GuestFn h = kind != KIND_GENERAL ? rt_lookup(g_handler[kind]) : NULL;
    if (h)
        h(g); /* the same arguments and return: it pops what this would */
    else
        rt_orig_cexi_item_general(g);
}
#endif

static int setup_item_ranges(void)
{
#if defined(FFXI_CEXI_ITEM_RANGES) && defined(FFXI_WRAP_CEXI_ITEM_GENERAL)
    static const uint32_t kind_first[KINDS] = { 0x0000, 0x1000, 0x2800, 0x4000, 0x7000 };
    uint32_t monstrosity = 0;
    for (unsigned i = 0; i < 64; ++i)
    {
        uint32_t e = FFXI_CEXI_ITEM_RANGES + 20 * i, first = rd32(e + 4), end = rd32(e + 8);
        if (first == 0xFFFFFFFFu)
            break;
        for (int k = 0; k < KINDS; ++k)
            if (first == kind_first[k] && !g_handler[k])
                g_handler[k] = rd32(e + 16);
        if (first == 0x7400 && (end == 0x7800 || end == CUSTOM_END))
            monstrosity = e;
    }
    for (int k = 0; k < KINDS; ++k)
        if (!g_handler[k] || !rt_lookup(g_handler[k]))
        {
            rt_log("[recomp] cexi: the item range table has no handler for ids from %#x; items left as they are\n", kind_first[k]);
            return 0;
        }
    if (g_handler[KIND_GENERAL] != FFXI_WRAP_CEXI_ITEM_GENERAL)
    {
        rt_log("[recomp] cexi: the general items' handler is %#x, not %#x; items left as they are\n",
            g_handler[KIND_GENERAL], FFXI_WRAP_CEXI_ITEM_GENERAL);
        return 0;
    }
    if (!monstrosity)
    {
        rt_log("[recomp] cexi: no Monstrosity entry (0x7400-0x7800) in the item range table; items left as they are\n");
        return 0;
    }
    wr32(monstrosity + 8, CUSTOM_END);
    wr32(monstrosity + 16, g_handler[KIND_GENERAL]);
    rt_wrap_cexi_item_general = item_general;
    return 1;
#else
    rt_log("[recomp] cexi: build %s has no item range table or handler; custom items are off\n", FFXI_BUILD);
    return 0;
#endif
}

/* ---- the code constants ---- */
static int setup_patch(const char* group)
{
    unsigned n = 0;
    for (unsigned i = 0; i < rt_patch_count; ++i)
        if (rt_patches[i].group && !strcmp(rt_patches[i].group, group))
        {
            memcpy(GUEST_PTR(rt_patches[i].addr), rt_patches[i].bytes, rt_patches[i].size);
            *rt_patches[i].on = 1;
            ++n;
        }
    if (!n)
        rt_log("[recomp] cexi: build %s has no %s code patch\n", FFXI_BUILD, group);
    return n != 0;
}

/* ---- the gear groups ---- */
#define GEAR_RACES 8
#define GEAR_SLOTS 9
#define GEAR_GROUPS 6
#define GEAR_FIRST_FILE 128240u /* race 0's face: the expanded gear files, 4096 to a race and slot */

static int setup_gear(void)
{
#ifdef FFXI_CEXI_GEAR_GROUPS
    uint32_t want[GEAR_RACES][GEAR_SLOTS][2];
    for (int race = 0; race < GEAR_RACES; ++race)
        for (int slot = 1; slot <= 7; ++slot)
        {
            uint32_t at = FFXI_CEXI_GEAR_GROUPS + (uint32_t)((race * GEAR_SLOTS + slot) * GEAR_GROUPS) * 8, start = 0;
            for (int grp = 0; grp < GEAR_GROUPS - 1; ++grp)
                start += rd32(at + 8 * grp + 4);
            uint32_t retail = slot <= 5 ? 608 : 1196; /* head..feet, main and sub */
            uint32_t base = rd32(at + 40), count = rd32(at + 44);
            want[race][slot][0] = GEAR_FIRST_FILE + (uint32_t)(race * GEAR_SLOTS + slot) * 4096 + retail;
            want[race][slot][1] = 4096 - retail;
            int as_retail = count == 64 && (slot <= 5 ? base && base < GEAR_FIRST_FILE : !base);
            int as_changed = base == want[race][slot][0] && count == want[race][slot][1];
            if (start != retail || !(as_retail || as_changed))
            {
                rt_log("[recomp] cexi: gear group race %d slot %d is not the retail client's (%u models, then %u at %u); "
                       "gear left as it is\n", race, slot, start, count, base);
                return 0;
            }
        }
    for (int race = 0; race < GEAR_RACES; ++race)
        for (int slot = 1; slot <= 7; ++slot)
        {
            uint32_t at = FFXI_CEXI_GEAR_GROUPS + (uint32_t)((race * GEAR_SLOTS + slot) * GEAR_GROUPS) * 8;
            wr32(at + 40, want[race][slot][0]);
            wr32(at + 44, want[race][slot][1]);
        }
    return 1;
#else
    rt_log("[recomp] cexi: build %s has no gear group table; custom gear is off\n", FFXI_BUILD);
    return 0;
#endif
}

void cexi_init(int mode)
{
    if (mode == CEXI_OFF)
        return;
    int items = setup_item_ranges();
    int code = items && setup_patch("cexi_items");
    int gear = setup_gear();
    rt_log("[recomp] cexi items: custom item ids %s, inventory and auction house %s, gear model ids %s\n",
        items ? "0x7800-0xDFFF" : "off", code ? "changed" : "as they ship", gear ? "to 4095" : "as they ship");
}
