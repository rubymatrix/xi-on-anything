/* The PS2 fixes that work in the game's own code; runtime/portable/d3d8.c draws the rest. After portfix
 * (github.com/voetre/portfix, MIT License, Copyright (c) 2026 voetre), which found and measured them.
 *
 * Shine batches (with fix_shine). Some of the PC's weapon and armour files lost batch boundaries in conversion: the
 * PS2 draws a mesh as one draw per material, but the PC file put a later batch's triangles into the draw before it
 * and left that batch's material at the end of the stream with nothing to draw, or filed whole draws under the wrong
 * material, so the shiny part is drawn as plain. shinesplit.h holds the PS2's batches, keyed by a hash of the texture
 * name, the counts, the PC's grouping and the triangles. A part whose command stream matches is pointed at a rebuilt
 * copy in guest memory: per segment its material's record, the texture record and its triangles, in their order. The
 * game writes into a stream's records once it is loaded (the texture record caches its texture) and frees a model's
 * streams and loads them again, often at the same address, on gear changes and zoning; so a copy stands in for one
 * stream at a time and is built again from that stream as it is now whenever a part takes it. With fix_shine off the
 * parts go back to their own streams.
 *
 * The part: skin_part_b (1002da20 in 2025-11-12) skins each part of a character on the CPU into world space, calling
 * its skinning (geometry_parent) with mov ecx,ebx / call; "skin_part" (meta/builds.json "hooks") is that call, the
 * part in ecx. Part +0x36 is its command stream: records of a 16-bit op and a 16-bit count, 0x80n1 a material (0x2e
 * bytes), 0x80n0 a texture (0x12, its name at +6), 0x54 a triangle list (4 + count x 30 bytes), 0xffff the end.
 *
 * The sites are checked against the game's bytes (portfix's patterns) before anything is hooked; a build without
 * them, or whose bytes differ, runs without these fixes and says so. */
#include "ps2fix.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "d3d8.h"
#include "gwin.h"
#include "runtime.h"
#include "shinesplit.h"

#ifdef FFXI_HOOK_SKIN_PART
extern GuestFn rt_hook_skin_part;
#endif

/* committed guest pages over [p, p + n) */
static int readable(uint32_t p, uint32_t n)
{
    if (p < 0x10000 || p + n < p)
        return 0;
    for (uint32_t a = p & ~0xFFFu; a <= ((p + n - 1) & ~0xFFFu); a += 0x1000)
    {
        if (!gwin_is_committed(a))
            return 0;
        if (a == 0xFFFFF000u)
            break;
    }
    return 1;
}

/* the game's bytes at `at` match a pattern of hex bytes, ?? any */
static int bytes_at(uint32_t at, const char* pat)
{
    size_t n = (strlen(pat) + 1) / 3;
    if (!readable(at, (uint32_t)n))
        return 0;
    const uint8_t* p = (const uint8_t*)GUEST_PTR(at);
    for (size_t i = 0; i < n; ++i, pat += 3)
        if (pat[0] != '?' && (uint8_t)strtoul((char[3]){ pat[0], pat[1], 0 }, NULL, 16) != p[i])
            return 0;
    return 1;
}

static uint16_t rd16_at(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* --- shine batches -------------------------------------------------------------------------------------------- */
typedef struct SplitCopy
{
    uint32_t buf, size; /* the rebuilt stream in guest memory (never freed: a part may still point at it) */
    uint32_t key;       /* the SHINE_SPLITS entry it was built for */
    uint32_t from;      /* the stream it stands in for now */
} SplitCopy;

typedef struct SplitPart
{
    uint32_t part, from, copy;
} SplitPart;

static SplitCopy* g_copies;
static uint32_t g_ncopies, g_capcopies;
static SplitPart* g_parts; /* parts pointed at a copy, to put back */
static uint32_t g_nparts, g_capparts;
/* parts whose stream didn't match, and that stream: a cache, an entry lost only means the stream is read again */
static struct
{
    uint32_t part, stream;
} g_seen[4096];
static int g_split_log;

static uint32_t fnv(const uint8_t* p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x01000193u;
    return h;
}

typedef struct SplitInfo
{
    uint32_t mats[64], nmats, tex; /* guest addresses of the records */
    uint32_t tris, ntris;          /* the triangle records, gathered (malloc'd) in tris_buf */
    uint8_t* tris_buf;
    const ShineSplit* hit;
    uint32_t key;
} SplitInfo;

/* the stream at s: its records, and the SHINE_SPLITS entry it matches; 0 when it is none of them */
static int split_parse(uint32_t s, SplitInfo* in)
{
    memset(in, 0, sizeof *in);
    uint16_t gm[64], gt[64]; /* (material, triangles), consecutive draws of one material merged */
    uint32_t ng = 0, cap = 0, p = 0;
    for (;;)
    {
        if (!readable(s + p, 4) || p > 0x40000)
            return 0;
        const uint8_t* r = (const uint8_t*)GUEST_PTR(s + p);
        uint16_t op = rd16_at(r), n = rd16_at(r + 2);
        if (op == 0xFFFF)
            break;
        if (op & 0x8000)
        {
            int k = op >> 4 & 0xF;
            if (k == 0)
            {
                if (!readable(s + p, 0x12))
                    return 0;
                if (in->tex && memcmp(GUEST_PTR(in->tex + 6), r + 6, 12))
                    return 0; /* several textures: not one of these */
                in->tex = s + p, p += 0x12;
            }
            else if (k == 1 && in->nmats < 64)
                in->mats[in->nmats++] = s + p, p += 0x2E;
            else
                return 0;
        }
        else if (op == 0x54)
        {
            uint32_t len = 30u * n;
            if (!in->nmats || !readable(s + p, 4 + len) || ng >= 64)
                return 0;
            uint16_t mi = (uint16_t)(in->nmats - 1);
            if (ng && gm[ng - 1] == mi)
                gt[ng - 1] = (uint16_t)(gt[ng - 1] + n);
            else
                gm[ng] = mi, gt[ng] = n, ++ng;
            if (in->ntris + len > cap)
            {
                cap = (in->ntris + len) * 2;
                uint8_t* b = (uint8_t*)realloc(in->tris_buf, cap);
                if (!b)
                    return 0;
                in->tris_buf = b;
            }
            memcpy(in->tris_buf + in->ntris, GUEST_PTR(s + p + 4), len);
            in->ntris += len, p += 4 + len;
        }
        else
            return 0;
    }
    if (!in->tex || in->nmats < 2 || !ng)
        return 0;
    uint16_t total = (uint16_t)(in->ntris / 30), nm = (uint16_t)in->nmats;
    const uint8_t* name = (const uint8_t*)GUEST_PTR(in->tex + 6);
    size_t len = 12, lead = 0;
    while (len > 0 && name[len - 1] == ' ')
        --len;
    while (lead < len && name[lead] == ' ')
        ++lead;
    uint32_t key = fnv(name + lead, len - lead, 0x811C9DC5u);
    uint8_t w[4];
    memcpy(w, &total, 2), memcpy(w + 2, &nm, 2), key = fnv(w, 4, key);
    for (uint32_t i = 0; i < ng; ++i)
        memcpy(w, &gm[i], 2), memcpy(w + 2, &gt[i], 2), key = fnv(w, 4, key);
    key = fnv(in->tris_buf, in->ntris, key);
    for (size_t i = 0; i < sizeof SHINE_SPLITS / sizeof SHINE_SPLITS[0]; ++i)
        if (SHINE_SPLITS[i].key == key && SHINE_SPLITS[i].n > 0)
            in->hit = &SHINE_SPLITS[i];
    if (!in->hit)
        return 0;
    uint32_t sum = 0;
    for (int i = 0; i < in->hit->n; ++i)
    {
        if (in->hit->mat[i] >= nm)
            return 0;
        sum += in->hit->tris[i];
    }
    in->key = key;
    return sum == total;
}

/* the copy's bytes, built into c from the stream as it is now (into its old buffer when the size is the same) */
static int split_build(const SplitInfo* in, SplitCopy* c)
{
    uint32_t size = 2, at = 0;
    for (int i = 0; i < in->hit->n; ++i)
        size += 0x2E + 0x12 + 4u * ((in->hit->tris[i] + 127u) / 128u) + 30u * in->hit->tris[i];
    uint8_t* out = (uint8_t*)malloc(size);
    if (!out)
        return 0;
    uint32_t o = 0;
    for (int i = 0; i < in->hit->n; ++i)
    {
        memcpy(out + o, GUEST_PTR(in->mats[in->hit->mat[i]]), 0x2E), o += 0x2E;
        memcpy(out + o, GUEST_PTR(in->tex), 0x12), o += 0x12;
        for (uint32_t left = in->hit->tris[i]; left > 0;)
        {
            uint16_t k = (uint16_t)(left < 128 ? left : 128), op = 0x54;
            memcpy(out + o, &op, 2), memcpy(out + o + 2, &k, 2), o += 4;
            memcpy(out + o, in->tris_buf + 30u * at, 30u * k), o += 30u * k;
            at += k, left -= k;
        }
    }
    out[o++] = 0xFF, out[o++] = 0xFF;
    if (!c->buf || c->size != size)
    {
        uint32_t b = gheap_alloc(size, 0);
        if (!b)
        {
            free(out);
            return 0;
        }
        c->buf = b, c->size = size;
    }
    memcpy(GUEST_PTR(c->buf), out, size);
    free(out);
    c->key = in->key;
    return 1;
}

/* a copy no live part points at any more, free to stand in for another stream */
static int split_copy_idle(const SplitCopy* c)
{
    for (uint32_t i = 0; i < g_nparts; ++i)
        if (g_parts[i].copy == c->buf && readable(g_parts[i].part + 0x36, 4) && rd32(g_parts[i].part + 0x36) == c->buf)
            return 0;
    return 1;
}

static void split_part(uint32_t part)
{
    if (!readable(part + 0x36, 4))
        return;
    uint32_t s = rd32(part + 0x36);
    if (!s)
        return;
    for (uint32_t i = 0; i < g_ncopies; ++i) /* already on a copy of ours: every frame, the usual case */
        if (g_copies[i].buf == s)
            return;
    uint32_t slot = ((part >> 2) * 2654435761u) >> 20; /* 0..4095 */
    if (g_seen[slot].part == part && g_seen[slot].stream == s) /* didn't match last time, same stream */
        return;
    SplitInfo in;
    int ok = split_parse(s, &in);
    if (!ok)
    {
        free(in.tris_buf);
        g_seen[slot].part = part, g_seen[slot].stream = s;
        return;
    }
    if (g_seen[slot].part == part)
        g_seen[slot].part = 0;
    /* the copy standing in for this stream; else an idle one built for the same mesh; else a new one */
    SplitCopy* c = NULL;
    for (uint32_t i = 0; i < g_ncopies && !c; ++i)
        if (g_copies[i].from == s && g_copies[i].key == in.key)
            c = &g_copies[i];
    for (uint32_t i = 0; i < g_ncopies && !c; ++i)
        if (g_copies[i].key == in.key && split_copy_idle(&g_copies[i]))
            c = &g_copies[i];
    if (!c)
    {
        if (g_ncopies == g_capcopies)
        {
            uint32_t cap = g_capcopies ? g_capcopies * 2 : 32;
            SplitCopy* n = (SplitCopy*)realloc(g_copies, cap * sizeof *n);
            if (!n)
            {
                free(in.tris_buf);
                return;
            }
            g_copies = n, g_capcopies = cap;
        }
        c = &g_copies[g_ncopies++];
        memset(c, 0, sizeof *c);
    }
    int fresh = !c->from;
    if (!split_build(&in, c))
    {
        free(in.tris_buf);
        return;
    }
    c->from = s;
    wr32(part + 0x36, c->buf);
    uint32_t i = 0;
    while (i < g_nparts && g_parts[i].part != part)
        ++i;
    if (i == g_nparts)
    {
        if (g_nparts == g_capparts)
        {
            uint32_t cap = g_capparts ? g_capparts * 2 : 64;
            SplitPart* n = (SplitPart*)realloc(g_parts, cap * sizeof *n);
            if (n)
                g_parts = n, g_capparts = cap;
        }
        if (i < g_capparts)
            ++g_nparts;
    }
    if (i < g_nparts)
        g_parts[i].part = part, g_parts[i].from = s, g_parts[i].copy = c->buf;
    char name[13] = { 0 };
    memcpy(name, GUEST_PTR(in.tex + 6), 12);
    const char* nm = name;
    while (*nm == ' ')
        ++nm;
    static int said;
    if (!said++ || g_split_log)
        rt_log("[recomp] ps2fix: shine batches: %s drawn in its PS2 batches, %u triangles in %d (%s copy for part %08x)"
               " (fix_shine)\n", nm, in.ntris / 30, in.hit->n, fresh ? "new" : "rebuilt", part);
    free(in.tris_buf);
}

/* the parts back on their own streams (those still on the copy they were given) */
static void split_undo(void)
{
    for (uint32_t i = 0; i < g_nparts; ++i)
        if (readable(g_parts[i].part + 0x36, 4) && rd32(g_parts[i].part + 0x36) == g_parts[i].copy)
            wr32(g_parts[i].part + 0x36, g_parts[i].from);
    g_nparts = 0;
    memset(g_seen, 0, sizeof g_seen);
}

#ifdef FFXI_HOOK_SKIN_PART
static void skin_part(Guest* g)
{
    if (d3d8_fixes() & D3D8_FIX_SHINE)
        split_part(g->ecx);
    else if (g_nparts)
        split_undo();
}
#endif

void ps2fix_setup(void)
{
    const char* log = getenv("FFXI_PS2FIXLOG");
    g_split_log = log && log[0] && log[0] != '0';
#ifdef FFXI_HOOK_SKIN_PART
    /* skin_part_b's prologue (sub esp,50h / push ebx,ebp,esi,edi / mov ebx,ecx / push 0 / mov [esp+20h],ebx / call)
     * within 0x400 before the call, which is mov ecx,ebx / call <skinning>, the skinning 0x15c0 past its start */
    uint32_t at = FFXI_HOOK_SKIN_PART, fn = 0;
    for (uint32_t f = at - 2; f + 0x400 > at && !fn; --f)
        if (bytes_at(f, "83 EC 50 53 55 56 57 8B D9 6A 00 89 5C 24 20 E8"))
            fn = f;
    if (!fn || !bytes_at(at - 2, "8B CB E8") || at + 5 + rd32(at + 1) != fn + 0x15C0)
    {
        rt_log("[recomp] ps2fix: build %s's skinning call isn't where expected; shine batches off\n", FFXI_BUILD);
        return;
    }
    rt_hook_skin_part = skin_part;
#else
    (void)split_part, (void)split_undo, (void)bytes_at;
    rt_log("[recomp] ps2fix: build %s has no skinning hook; shine batches off\n", FFXI_BUILD);
#endif
}
