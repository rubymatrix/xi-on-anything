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
 * --cexi full: those, and the spell and job-ability ceilings (0x400 spells, 0xB00 commands) raised to
 * 0x1000, so the records the server's menu DAT (ROM/118/114.DAT, enlarged) adds past retail show in
 * the menus, resolve by name and keep recasts, with effects and weapon-skill motions from file ids
 * past the retail ones. The sites come from cexislots' site list (tools/cexi_sites.py --slots):
 *
 *  - Patch group cexi_slots: every loop bound, size and list layout constant the ceilings move
 *    (the known-spell list: bitmap 0x200, ids from +0x200, count at +0x2200; the same for commands),
 *    the record decode loops (the game decodes the whole enlarged tables at start), the recast
 *    array's references and the known-bits getters' operands, all aimed at the host's buffers at a
 *    fixed guest address (cexi_region, reserved here), and the /ja name search's end.
 *  - The list objects: the game builds its retail-size ones and registers them with its user-file
 *    manager, which loads and saves the menu order through them (USER/<id>/mix.dat, aix.dat). As
 *    each is stored (hooks cexi_spell_list, cexi_cmd_list) a larger one takes its place, and each
 *    frame the two are kept in step: an order the manager loaded into the retail one is taken, and
 *    the retail band of ours is written back for its next save.
 *  - The known bits: from packets 0x0AA (spells) and 0x0AC (commands) as retail, and from the
 *    server's 0x1A5 for the ids past retail; the client says it can take them with a 0x1A5 hello at
 *    each zone-in. The command list's fill (wrap cexi_cmd_fill) works on the host's bits.
 *  - The /ja search (hook cexi_ja_search) skips 0x600-0xAFF on its way to the custom band: those
 *    are instincts and mounts, many named like player abilities.
 *  - Effects (hooks cexi_fx_*): a spell animation from 1612 is file 423152 + n, a job ability's from
 *    1024 is 427248 + n; retail numbers keep their arithmetic. A weapon skill's from 272 (hooks
 *    cexi_ws_bank, cexi_ws_join) has its body at 431344 + (n - 272) * 24 + race, its waist packs 8
 *    and 16 on.
 *  - The file tables (hook cexi_file_tables, at the end of their loader) grow to 437488 entries, and
 *    the DAT overlays' ROM<n> tables' registrations past the retail count are merged in.
 *  - Each frame the recast array and the retail band of the known bits are copied back where the
 *    game keeps them, for addons that read them there.
 *
 * Each part checks the game holds what the retail client does (or already the change) and is left
 * alone otherwise, with a line in the log saying why; full needs the enlarged menu DAT, and stays at
 * items without it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "addons/addons.h"
#include "build.h"
#include "cexi.h"
#include "gthread.h"
#include "gwin.h"
#include "runtime.h"
#include "vfs.h"

#define CUSTOM_END 0xE000u

int cexi_parse(const char* s)
{
    if (!strcmp(s, "off"))
        return CEXI_OFF;
    if (!strcmp(s, "items"))
        return CEXI_ITEMS;
    if (!strcmp(s, "full"))
        return CEXI_FULL;
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

/* ---- full: the spell and job-ability ceilings ---- */
#if defined(FFXI_CEXI_REGION) && defined(FFXI_HOOK_CEXI_SPELL_LIST) && defined(FFXI_HOOK_CEXI_CMD_LIST) &&                   \
    defined(FFXI_HOOK_CEXI_JA_SEARCH) && defined(FFXI_HOOK_CEXI_FX_SPELL_EDI) && defined(FFXI_HOOK_CEXI_FX_SPELL_EAX) &&      \
    defined(FFXI_HOOK_CEXI_FX_JA_EAX) && defined(FFXI_HOOK_CEXI_FX_DISPATCH) && defined(FFXI_HOOK_CEXI_WS_BANK) &&            \
    defined(FFXI_HOOK_CEXI_WS_JOIN) && defined(FFXI_HOOK_CEXI_FILE_TABLES) && defined(FFXI_WRAP_CEXI_CMD_FILL)
extern GuestFn rt_hook_cexi_spell_list, rt_hook_cexi_cmd_list, rt_hook_cexi_ja_search, rt_hook_cexi_fx_spell_edi,
    rt_hook_cexi_fx_spell_eax, rt_hook_cexi_fx_ja_eax, rt_hook_cexi_fx_dispatch, rt_hook_cexi_ws_bank, rt_hook_cexi_ws_join,
    rt_hook_cexi_file_tables, rt_wrap_cexi_cmd_fill;

#define CEILING 0x1000u
#define SPELLS 0x400u   /* retail */
#define COMMANDS 0xB00u /* retail */
#define BITS (CEILING / 8)
/* The region (tools/cexi_sites.py writes the code that names it; keep the two in step) */
#define R_SPELL_PLAYER (FFXI_CEXI_REGION + 0x0)
#define R_CMD_PLAYER (FFXI_CEXI_REGION + 0x4)
#define R_SPELL_BITS (FFXI_CEXI_REGION + 0x100)
#define R_CMD_BITS (FFXI_CEXI_REGION + 0x400)
#define R_RECAST (FFXI_CEXI_REGION + 0x1000)
#define R_SIZE 0x30000u

/* effect file ids: from these numbers, past the retail files (the server's DATs put them there) */
#define FX_SPELL_RETAIL 0xAF0u
#define FX_SPELL_FIRST 1612u
#define FX_SPELL_BASE 423152u
#define FX_JA_RETAIL 0x113Cu
#define FX_JA_FIRST 1024u
#define FX_JA_BASE 427248u
#define WS_FIRST 272u
#define WS_BASE 431344u
#define FILE_IDS 437488u /* the file tables' entries, for all of the above */

static int g_slots; /* applied */
static uint32_t g_spell_disp, g_cmd_disp; /* the known bits' place in the player, as the getters had it */

/* ---- the 114.DAT check: the enlarged tables, each record past retail empty or its own id ---- */
static int popcount8(uint8_t v)
{
    int n = 0;
    for (; v; v &= (uint8_t)(v - 1))
        ++n;
    return n;
}

/* a menu record as the game decodes it: every byte but 0x02, 0x0B and 0x0C rotated left */
static void decode_record(uint8_t* r, uint32_t stride)
{
    static const uint8_t amount[5] = { 1, 7, 2, 6, 3 };
    int v = popcount8(r[0x02]) + popcount8(r[0x0C]) - popcount8(r[0x0B]);
    unsigned k = amount[(v < 0 ? -v : v) % 5];
    for (uint32_t i = 0; i < stride; ++i)
        if (i != 0x02 && i != 0x0B && i != 0x0C)
            r[i] = (uint8_t)(r[i] << k | r[i] >> (8 - k));
}

static int table_ok(const uint8_t* d, long n, const char* name, uint32_t stride, uint32_t retail)
{
    for (long o = 0; o + 16 <= n;)
    {
        uint32_t info = (uint32_t)d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
        long len = (long)((info >> 7) & 0x7FFFF) * 16;
        if (len < 16 || o + len > n)
            break;
        if (!memcmp(d + o, name, 4))
        {
            if ((uint32_t)(len - 16) != CEILING * stride)
            {
                rt_log("[recomp] cexi: ROM/118/114.DAT's %s has %u records, not %u: the server's enlarged menu DAT is "
                       "not in the --dats overlay\n", name, (unsigned)((len - 16) / stride), CEILING);
                return 0;
            }
            uint8_t r[0x64];
            for (uint32_t i = retail; i < CEILING; ++i)
            {
                memcpy(r, d + o + 16 + i * stride, stride);
                int empty = 1;
                for (uint32_t k = 0; k < stride && empty; ++k)
                    empty = !r[k];
                if (empty)
                    continue;
                decode_record(r, stride);
                if ((uint32_t)(r[0] | r[1] << 8) != i)
                {
                    rt_log("[recomp] cexi: ROM/118/114.DAT's %s record %#x is not one\n", name, i);
                    return 0;
                }
            }
            return 1;
        }
        o += len;
    }
    rt_log("[recomp] cexi: ROM/118/114.DAT has no %s table\n", name);
    return 0;
}

static uint8_t* read_game_file(const char* game, const char* rel, long* size)
{
    char guest[1200], path[1200];
    snprintf(guest, sizeof guest, "%s\\%s", game, rel);
    if (!vfs_overlay_path(guest, path, sizeof path) && !vfs_host_path(guest, path, sizeof path))
        return NULL;
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n)
        free(d), d = NULL;
    fclose(f);
    *size = n;
    return d;
}

static int menu_dat_ok(const char* game)
{
    long n = 0;
    uint8_t* d = read_game_file(game, "ROM\\118\\114.DAT", &n);
    if (!d)
    {
        rt_log("[recomp] cexi: cannot read ROM/118/114.DAT\n");
        return 0;
    }
    int ok = table_ok(d, n, "mgc_", 0x64, SPELLS) && table_ok(d, n, "comm", 0x30, COMMANDS);
    free(d);
    return ok;
}

/* ---- the list objects: retail {bitmap, ids, count}, ours {bitmap 0x200, ids from +0x200, count} ---- */
typedef struct List
{
    const char* what;
    uint32_t retail_count, retail_ids_at; /* retail: bitmap [0, ids_at), ids, count */
    uint32_t retail, ours;                /* the objects */
    uint8_t written[0x1764];              /* the retail one as this last left it */
    int have_written;
} List;
#define OURS_IDS 0x200u
#define OURS_COUNT (OURS_IDS + CEILING * 2)
#define OURS_SIZE (OURS_COUNT + 4)
static List g_spells = { "spell", SPELLS, 0x80 }, g_cmds = { "command", COMMANDS, 0x160 };

static uint32_t retail_size(const List* l) { return l->retail_ids_at + l->retail_count * 2 + 4; }

/* The game just built and registered its retail object: ours takes its place, its contents moved
 * into our layout. The game's malloc, so the game's free at its end works on it. */
static uint32_t swap_list(List* l, uint32_t retail)
{
    if (!retail)
        return retail;
    uint32_t size = OURS_SIZE;
    uint32_t ours = guest_call(FFXI_CEXI_MALLOC, 1, &size);
    if (!ours)
    {
        rt_log("[recomp] cexi: no memory for the %s list; it stays retail\n", l->what);
        return retail;
    }
    memset(GUEST_PTR(ours), 0, OURS_SIZE);
    memcpy(GUEST_PTR(ours), GUEST_PTR(retail), l->retail_ids_at);
    memcpy(GUEST_PTR(ours + OURS_IDS), GUEST_PTR(retail + l->retail_ids_at), l->retail_count * 2);
    wr32(ours + OURS_COUNT, rd32(retail + l->retail_ids_at + l->retail_count * 2));
    l->retail = retail, l->ours = ours, l->have_written = 0;
    return ours;
}

static void spell_list(Guest* g) { g->eax = swap_list(&g_spells, g->eax); }
static void cmd_list(Guest* g) { g->eax = swap_list(&g_cmds, g->eax); }

/* The order the user-file manager loaded into the retail object becomes ours; our custom ids keep
 * theirs, after it. The bitmap copy follows the ids (the game's reconcile expects bit set == listed). */
static void take_order(const List* l, const uint8_t* r, uint8_t* n)
{
    uint16_t* ids = (uint16_t*)(n + OURS_IDS);
    uint32_t count = rd32(l->ours + OURS_COUNT), k = 0;
    static uint16_t custom[CEILING];
    uint32_t ncustom = 0;
    for (uint32_t i = 0; i < count && i < CEILING; ++i)
        if (ids[i] >= l->retail_count)
            custom[ncustom++] = ids[i];
    uint32_t saved;
    memcpy(&saved, r + l->retail_ids_at + l->retail_count * 2, 4);
    if (saved > l->retail_count)
        saved = 0;
    memset(n, 0, l->retail_ids_at);
    for (uint32_t i = 0; i < saved; ++i)
    {
        uint16_t id;
        memcpy(&id, r + l->retail_ids_at + 2 * i, 2);
        if (!id || id >= l->retail_count || n[id >> 3] & 1u << (id & 7))
            continue;
        n[id >> 3] |= (uint8_t)(1u << (id & 7));
        ids[k++] = id;
    }
    for (uint32_t i = 0; i < ncustom && k < CEILING; ++i)
        ids[k++] = custom[i];
    memset(ids + k, 0, (CEILING - k) * 2);
    wr32(l->ours + OURS_COUNT, k);
}

static void sync_order(List* l, uint32_t global)
{
    if (!l->ours || rd32(global) != l->ours)
        return;
    uint32_t size = retail_size(l);
    uint8_t* r = GUEST_PTR(l->retail);
    uint8_t* n = GUEST_PTR(l->ours);
    if (l->have_written && memcmp(r, l->written, size))
        take_order(l, r, n);
    static uint8_t out[0x1764];
    memset(out, 0, size);
    const uint16_t* ids = (const uint16_t*)(n + OURS_IDS);
    uint32_t count = rd32(l->ours + OURS_COUNT), k = 0;
    for (uint32_t i = 0; i < count && i < CEILING; ++i)
    {
        uint16_t id = ids[i];
        if (!id || id >= l->retail_count || out[id >> 3] & 1u << (id & 7))
            continue;
        out[id >> 3] |= (uint8_t)(1u << (id & 7));
        memcpy(out + l->retail_ids_at + 2 * k++, &id, 2);
    }
    memcpy(out + l->retail_ids_at + l->retail_count * 2, &k, 4);
    if (memcmp(r, out, size))
        memcpy(r, out, size);
    memcpy(l->written, out, size);
    l->have_written = 1;
}

/* ---- the known bits ---- */
static void cmd_fill(Guest* g)
{
    /* the game's: cdecl (player, ids, count); the bits of 0x700..0xAFF cleared, one set per listed id */
    uint32_t ids = rt_arg(g, 1), count = rt_arg(g, 2) & 0xFFFF;
    uint8_t* bits = GUEST_PTR(R_CMD_BITS);
    memset(bits + 0xE0, 0, 0x160 - 0xE0);
    for (uint32_t i = 0; ids && i < count; ++i)
    {
        uint32_t id = rd16(ids + 2 * i);
        if (id && id < COMMANDS)
            bits[id >> 3] |= (uint8_t)(1u << (id & 7));
    }
    rt_return(g, g->eax, 0);
}

static volatile int g_hello;

static void packet_in(const uint8_t* p, size_t n)
{
    if (!g_slots)
        return;
    uint32_t id = (uint32_t)(p[0] | p[1] << 8) & 0x1FF;
    if (id == 0x0AA && n >= 4 + 0x80)
        memcpy(GUEST_PTR(R_SPELL_BITS), p + 4, 0x80); /* known spells 0..0x3FF */
    else if (id == 0x0AC && n >= 4 + 0xE0)
        memcpy(GUEST_PTR(R_CMD_BITS), p + 4, 0xE0); /* known commands 0..0x6FF */
    else if (id == 0x1A5 && n >= 10)
    {
        /* {action 1, kind 0 spells / 1 commands, u16 byte offset, u16 length, bits} */
        uint32_t off = (uint32_t)(p[6] | p[7] << 8), len = (uint32_t)(p[8] | p[9] << 8);
        if (p[4] == 1 && p[5] <= 1 && off + len <= BITS && 10 + len <= n)
            memcpy(GUEST_PTR((p[5] ? R_CMD_BITS : R_SPELL_BITS) + off), p + 10, len);
    }
    else if (id == 0x00A)
        g_hello = 1; /* zone-in: another map process may serve the zone */
}

/* ---- the /ja search, effects and weapon-skill motions ---- */
static void ja_search(Guest* g)
{
    /* before cmp ebp,0x30000 (ebp the next record's offset, ebx its index): past the retail second
     * pass (0x600 * 0x30) straight on to the custom band */
    if (g->ebp >= 0x600 * 0x30 && g->ebp < COMMANDS * 0x30)
        g->ebp = COMMANDS * 0x30, g->ebx = COMMANDS;
}

/* before add edi/eax, retail base: a number from the first custom one lands at base + n */
static void fx_spell_edi(Guest* g)
{
    if (g->edi >= FX_SPELL_FIRST)
        g->edi += FX_SPELL_BASE - FX_SPELL_RETAIL;
}
static void fx_spell_eax(Guest* g)
{
    if (g->eax >= FX_SPELL_FIRST)
        g->eax += FX_SPELL_BASE - FX_SPELL_RETAIL;
}
static void fx_ja_eax(Guest* g)
{
    if (g->eax >= FX_JA_FIRST)
        g->eax += FX_JA_BASE - FX_JA_RETAIL;
}
/* before add ebx,eax: ebx the kind's base, eax the number */
static void fx_dispatch(Guest* g)
{
    if (g->ebx == FX_SPELL_RETAIL && g->eax >= FX_SPELL_FIRST)
        g->ebx = FX_SPELL_BASE;
    else if (g->ebx == FX_JA_RETAIL && g->eax >= FX_JA_FIRST)
        g->ebx = FX_JA_BASE;
}

/* Weapon-skill motions: before cmp ecx,0x100 (eax the race, ecx the number, dl the waist pack), and
 * where the bank paths meet again. A custom number takes the extended bank's path with 0x100 (its
 * first, harmless reads) and gets its own ids at the join. */
static int g_ws_pending;
static uint32_t g_ws_body, g_ws_waist;
static void ws_bank(Guest* g)
{
    if (g->ecx < WS_FIRST)
        return;
    g_ws_body = WS_BASE + (g->ecx - WS_FIRST) * 24 + g->eax;
    g_ws_waist = g_ws_body + ((g->edx & 0xFF) == 1 ? 8 : 16);
    g_ws_pending = 1;
    g->ecx = 0x100;
}
static void ws_join(Guest* g)
{
    /* then: add esi,ecx (ecx is the offset into the bank) */
    if (!g_ws_pending)
        return;
    g_ws_pending = 0;
    g->edi = g_ws_body, g->esi = g_ws_waist, g->ecx = 0;
}

/* ---- the file tables ---- */
static const char* g_game;

/* At the end of their loader (esi the file manager: +0xbc the volume byte per file id, +0xc0 their
 * count, +0xc4 the file word per id, +0xc8 their count, +0xcc its size in bytes): both grow to
 * FILE_IDS, and the overlays' ROM<n> registrations past the old count (the loader read only as far
 * as VTABLE.DAT goes) are merged in, as the loader does below it. */
static void file_tables(Guest* g)
{
    uint32_t m = g->esi, count = rd32(m + 0xc8), vbuf = rd32(m + 0xbc), fbuf = rd32(m + 0xc4);
    if (count >= FILE_IDS || count != rd32(m + 0xc0) || !vbuf || !fbuf)
        return;
    uint32_t sizes[2] = { FILE_IDS, FILE_IDS * 2 };
    uint32_t v = guest_call(FFXI_CEXI_MALLOC, 1, &sizes[0]), f = guest_call(FFXI_CEXI_MALLOC, 1, &sizes[1]);
    if (!v || !f)
    {
        rt_log("[recomp] cexi: no memory for the file tables\n");
        return;
    }
    memset(GUEST_PTR(v), 0, FILE_IDS), memset(GUEST_PTR(f), 0, FILE_IDS * 2);
    memcpy(GUEST_PTR(v), GUEST_PTR(vbuf), count), memcpy(GUEST_PTR(f), GUEST_PTR(fbuf), count * 2);
    unsigned merged = 0;
    for (int rom = 2; rom <= 13; ++rom)
    {
        char vrel[64], frel[64];
        snprintf(vrel, sizeof vrel, "ROM%d\\VTABLE%d.DAT", rom, rom);
        snprintf(frel, sizeof frel, "ROM%d\\FTABLE%d.DAT", rom, rom);
        long vn = 0, fn = 0;
        uint8_t* vd = read_game_file(g_game, vrel, &vn);
        uint8_t* fd = vd ? read_game_file(g_game, frel, &fn) : NULL;
        for (uint32_t id = count; fd && id < FILE_IDS && id < (uint32_t)vn && 2 * id + 1 < (uint32_t)fn; ++id)
            if (vd[id] == rom)
            {
                wr8(v + id, (uint8_t)rom);
                wr16(f + 2 * id, (uint16_t)(fd[2 * id] | fd[2 * id + 1] << 8));
                ++merged;
            }
        free(vd), free(fd);
    }
    guest_call(FFXI_CEXI_FREE, 1, &vbuf);
    guest_call(FFXI_CEXI_FREE, 1, &fbuf);
    wr32(m + 0xbc, v), wr32(m + 0xc0, FILE_IDS), wr32(m + 0xc4, f), wr32(m + 0xc8, FILE_IDS), wr32(m + 0xcc, FILE_IDS * 2);
    rt_log("[recomp] cexi: file tables %u -> %u entries, %u registrations from the overlays past the old end\n", count,
        FILE_IDS, merged);
}

static int setup_slots(const char* game)
{
    if (!menu_dat_ok(game))
        return 0;
    if (gwin_reserve(FFXI_CEXI_REGION, R_SIZE) != FFXI_CEXI_REGION || !gwin_commit(FFXI_CEXI_REGION, R_SIZE))
    {
        rt_log("[recomp] cexi: guest memory at %#x is taken; the spell and ability ceilings stay retail\n", FFXI_CEXI_REGION);
        return 0;
    }
    /* the getters' bits displacements, from the code before the patch rewrites their operands */
    g_spell_disp = rd32(FFXI_CEXI_SPELL_GETTER + 0x18);
    g_cmd_disp = rd32(FFXI_CEXI_CMD_GETTER + 0x1E);
    if (!setup_patch("cexi_slots"))
        return 0;
    memset(GUEST_PTR(FFXI_CEXI_REGION), 0, R_SIZE);
    wr32(R_SPELL_PLAYER, R_SPELL_BITS - g_spell_disp);
    wr32(R_CMD_PLAYER, R_CMD_BITS - g_cmd_disp);
    wr8(R_SPELL_BITS + BITS, 1), wr8(R_CMD_BITS + BITS, 1), wr8(R_CMD_BITS + BITS + 1, 1); /* "data arrived" */
    g_game = game;
    rt_hook_cexi_spell_list = spell_list;
    rt_hook_cexi_cmd_list = cmd_list;
    rt_wrap_cexi_cmd_fill = cmd_fill;
    rt_hook_cexi_ja_search = ja_search;
    rt_hook_cexi_fx_spell_edi = fx_spell_edi;
    rt_hook_cexi_fx_spell_eax = fx_spell_eax;
    rt_hook_cexi_fx_ja_eax = fx_ja_eax;
    rt_hook_cexi_fx_dispatch = fx_dispatch;
    rt_hook_cexi_ws_bank = ws_bank;
    rt_hook_cexi_ws_join = ws_join;
    rt_hook_cexi_file_tables = file_tables;
    addons_packet_tap(packet_in);
    g_slots = 1;
    return 1;
}

void cexi_frame(void)
{
    if (!g_slots)
        return;
    if (g_hello)
    {
        /* the server learns the client takes ids to the ceiling (sent from here, the game's thread,
         * where the addons' own packets are queued too) */
        g_hello = 0;
        uint8_t hello[8] = { 0xA5, 0x01 | (8 / 4) << 1, 0, 0, 0 /* hello */, 1 /* version */, CEILING & 0xFF, CEILING >> 8 };
        addons_packet_send(hello, sizeof hello);
    }
    /* the order the user-file manager loads and saves; the hooks are on mov [global],eax */
    sync_order(&g_spells, rd32(FFXI_HOOK_CEXI_SPELL_LIST + 1));
    sync_order(&g_cmds, rd32(FFXI_HOOK_CEXI_CMD_LIST + 1));
    /* where addons read them: the recasts, and the retail band of the known bits */
    memcpy(GUEST_PTR(FFXI_CEXI_SPELL_RECAST), GUEST_PTR(R_RECAST), SPELLS * 2);
    uint32_t player = rd32(FFXI_CEXI_PLAYER);
    if (player)
    {
        memcpy(GUEST_PTR(player + g_spell_disp), GUEST_PTR(R_SPELL_BITS), SPELLS / 8);
        memcpy(GUEST_PTR(player + g_cmd_disp), GUEST_PTR(R_CMD_BITS), COMMANDS / 8);
    }
}
#else
static int setup_slots(const char* game)
{
    (void)game;
    rt_log("[recomp] cexi: build %s has no spell and ability sites (tools/cexi_sites.py --slots); they stay retail\n", FFXI_BUILD);
    return 0;
}
void cexi_frame(void) {}
#endif

void cexi_init(int mode, const char* game)
{
    if (mode == CEXI_OFF)
        return;
    int items = setup_item_ranges();
    int code = items && setup_patch("cexi_items");
    int gear = setup_gear();
    rt_log("[recomp] cexi items: custom item ids %s, inventory and auction house %s, gear model ids %s\n",
        items ? "0x7800-0xDFFF" : "off", code ? "changed" : "as they ship", gear ? "to 4095" : "as they ship");
    if (mode == CEXI_FULL)
        rt_log("[recomp] cexi full: spell and job-ability ids %s\n", setup_slots(game) ? "to 0xFFF" : "as they ship");
}
