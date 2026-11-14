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
 * Disintegrate (fix_disintegrate; d3d8.c draws it). Effects such as the Shadow Lord's change of form (16/98) sweep a
 * particle over a model, and its op 0x65 (record 65 07 00 00 | generator handle | radius at +0xc, animated by the
 * particle's op 0x6a) marks a sphere around it each frame for the actor the generator plays on (generator +0x3c): on
 * the PS2 the body's triangles inside it break off. The PC's per-particle interpreter sends op 0x65 to its do-nothing
 * case. "particle_op" is the interpreter's dispatch (mov eax,[ebp] / and eax,0FFh / dec eax / cmp eax,6Dh / ja /
 * jmp [eax*4+table]: ebx the particle, ebp the op's record, [esp+48h] the generator); for op 0x65 the sphere goes to
 * d3d8.c with the actor's bone palette ([[actor+0x6b8]+0x14]), centred on the particle's world position (the game's
 * "particle_world_pos", thiscall (particle, float out[3])). On frames that took triangles d3d8_disint_smoke says
 * where to make the purple smoke: the game's own "particle_emit" (thiscall (generator, parent)) emits generator mt01
 * (op 0x3c in the creation list of the generator at record +8, g000) on a copy of this particle moved onto a taken
 * triangle (its local position solved by three probes of the world position and Cramer's rule). The PC never
 * registers g000's own haze element, so that is d3d8.c's too. As each part is skinned, skin_part tells d3d8.c whose it
 * was (the palette the skinning reads, "scene_palette") and its vertex buffers: mesh (part +0x5e) +0x44 positions,
 * +0x48 normals, and for a part with bit 0 of +0x34 the mirrored half's at +0x50 and +0x54.
 *
 * The sites are checked against the game's bytes (portfix's patterns) before anything is hooked; a build without
 * them, or whose bytes differ, runs without these fixes and says so. */
#include "ps2fix.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "d3d8.h"
#include "gthread.h"
#include "gwin.h"
#include "runtime.h"
#include "shinesplit.h"

#ifdef FFXI_HOOK_SKIN_PART
extern GuestFn rt_hook_skin_part;
#endif
#if defined(FFXI_HOOK_PARTICLE_OP) && defined(FFXI_PARTICLE_WORLD_POS) && defined(FFXI_SCENE_PALETTE) && \
    defined(FFXI_HOOK_SKIN_PART)
#define HAVE_DISINT 1
extern GuestFn rt_hook_particle_op;
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

/* --- disintegrate ---------------------------------------------------------------------------------------------- */
static int g_split_ok, g_disint_ok, g_emit_ok;

#ifdef HAVE_DISINT
static uint32_t g_out, g_parent; /* guest memory: a world position, the smoke's stand-in parent */

static int world_pos(uint32_t particle, float out[3])
{
    guest_thiscall(FFXI_PARTICLE_WORLD_POS, particle, 1, (uint32_t[]){ g_out });
    memcpy(out, GUEST_PTR(g_out), 12);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static float det3(const float k[3][3])
{
    return k[0][0] * (k[1][1] * k[2][2] - k[1][2] * k[2][1]) - k[0][1] * (k[1][0] * k[2][2] - k[1][2] * k[2][0]) +
        k[0][2] * (k[1][0] * k[2][1] - k[1][1] * k[2][0]);
}

/* the purple smoke, where d3d8.c says triangles were taken: mt01 emitted on a copy of the particle moved there */
static void smoke(uint32_t particle, uint32_t rec)
{
    float at[4][3];
    int n = d3d8_disint_smoke(at);
    if (!n || !g_emit_ok)
        return;
    uint32_t h, gen = 0, list, op;
    if (readable(rec + 8, 4) && (h = rd32(rec + 8)) && readable(h, 4) && (gen = rd32(h)) && readable(gen + 0xE4, 4) &&
        (list = rd32(gen + 0xE4)))
    {
        gen = 0;
        for (int i = 0; i < 64 && readable(list, 8) && ((op = rd32(list)) & 0xFF) && (op >> 8 & 0x1F); ++i, list += 4 * (op >> 8 & 0x1F))
            if ((op & 0xFF) == 0x3C && (h = rd32(list + 4)) && readable(h, 4))
            {
                gen = rd32(h);
                break;
            }
    }
    else
        gen = 0;
    if (!gen || !readable(gen, 0xE8) || !readable(particle, 0x200))
        return;
    for (int e = 0; e < n; ++e)
    {
        /* the parent's position (+0x54) is in its own frame: the one that puts it at the triangle. The world position
         * is affine in it, so three unit steps give the matrix. */
        memcpy(GUEST_PTR(g_parent), GUEST_PTR(particle), 0x200);
        uint32_t local = g_parent + 0x54;
        float l0[3], w0[3], m[3][3];
        memcpy(l0, GUEST_PTR(local), 12);
        if (!world_pos(g_parent, w0))
            continue;
        for (int c = 0; c < 3; ++c)
        {
            float w[3], l[3];
            memcpy(l, l0, 12), l[c] += 1.0f, memcpy(GUEST_PTR(local), l, 12);
            world_pos(g_parent, w);
            for (int r = 0; r < 3; ++r)
                m[r][c] = w[r] - w0[r];
        }
        float det = det3((const float(*)[3])m);
        if (!(fabsf(det) > 1e-9f))
            continue;
        float d[3] = { at[e][0] - w0[0], at[e][1] - w0[1], at[e][2] - w0[2] }, l[3];
        for (int c = 0; c < 3; ++c) /* Cramer's rule */
        {
            float k[3][3];
            memcpy(k, m, sizeof k);
            for (int r = 0; r < 3; ++r)
                k[r][c] = d[r];
            l[c] = l0[c] + det3((const float(*)[3])k) / det;
        }
        memcpy(GUEST_PTR(local), l, 12);
        wr32(gen + 0xCC, 0); /* the generator's countdown: emit now */
#ifdef FFXI_PARTICLE_EMIT
        guest_thiscall(FFXI_PARTICLE_EMIT, gen, 1, (uint32_t[]){ g_parent });
#endif
    }
}

static void particle_op(Guest* g)
{
    if (rd8(g->ebp) != 0x65 || !(d3d8_fixes() & D3D8_FIX_DISINTEGRATE))
        return;
    uint32_t particle = g->ebx, rec = g->ebp, gen = rd32(g->esp + 0x48), actor, holder, bones;
    if (!readable(gen + 0x3C, 4) || !readable(rec + 0xC, 4) || !(actor = rd32(gen + 0x3C)) || !readable(actor + 0x6B8, 4) ||
        !(holder = rd32(actor + 0x6B8)) || !readable(holder + 0x14, 4) || (bones = rd32(holder + 0x14)) < 0x10000)
        return;
    float r, c[3];
    uint32_t ru = rd32(rec + 0xC);
    memcpy(&r, &ru, 4);
    if (!(r > 0.0f) || r > 1000.0f || !world_pos(particle, c))
        return;
    d3d8_disint_sphere(bones, c, r);
    static int said;
    if (!said++)
        rt_log("[recomp] ps2fix: disintegrate: an effect's op 0x65 marks the model (fix_disintegrate)\n");
    smoke(particle, rec);
}
#endif

#ifdef FFXI_HOOK_SKIN_PART
static void skin_part(Guest* g)
{
    unsigned fx = d3d8_fixes();
    if (g_split_ok && (fx & D3D8_FIX_SHINE))
        split_part(g->ecx);
    else if (g_nparts)
        split_undo();
#ifdef HAVE_DISINT
    if (g_disint_ok && (fx & D3D8_FIX_DISINTEGRATE))
    {
        uint32_t part = g->ecx, mesh = readable(part + 0x5E, 4) ? rd32(part + 0x5E) : 0, vb[4] = { 0, 0, 0, 0 };
        if (mesh && readable(mesh + 0x44, 0x14) && readable(part + 0x34, 1))
        {
            vb[0] = rd32(mesh + 0x44), vb[2] = rd32(mesh + 0x48);
            if ((rd8(part + 0x34) & 1) && rd32(mesh + 0x50) && rd32(mesh + 0x54))
                vb[1] = rd32(mesh + 0x50), vb[3] = rd32(mesh + 0x54);
        }
        d3d8_disint_part(rd32(FFXI_SCENE_PALETTE), vb);
    }
#endif
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
        rt_log("[recomp] ps2fix: build %s's skinning call isn't where expected; shine batches and disintegrate off\n",
            FFXI_BUILD);
        return;
    }
    g_split_ok = 1;
#ifdef HAVE_DISINT
    /* skin_part_b reads the palette with mov eax,[palette] 20 bytes in; the dispatch's table has op 0x65 at the
     * do-nothing case op 0x47 shares (unless something else took it) */
    uint32_t op = FFXI_HOOK_PARTICLE_OP, table = rd32(op + 21);
    if (rd8(fn + 20) != 0xA1 || rd32(fn + 21) != FFXI_SCENE_PALETTE)
        rt_log("[recomp] ps2fix: build %s's skinning palette isn't where expected; disintegrate off\n", FFXI_BUILD);
    else if (!bytes_at(op, "8B 45 00 25 FF 00 00 00 48 83 F8 6D 0F 87 ?? ?? ?? ?? FF 24 85") || !readable(table, 4 * 0x6E) ||
        rd32(table + 4 * (0x65 - 1)) != rd32(table + 4 * (0x47 - 1)))
        rt_log("[recomp] ps2fix: build %s's particle op dispatch isn't where expected; disintegrate off\n", FFXI_BUILD);
    else if (!bytes_at(FFXI_PARTICLE_WORLD_POS, "83 EC 0C 56 8B F1 8D 46 54 8B 4E 54"))
        rt_log("[recomp] ps2fix: build %s's particle position isn't where expected; disintegrate off\n", FFXI_BUILD);
    else
    {
        g_out = gheap_alloc(16, 1), g_parent = gheap_alloc(0x200, 1);
        g_disint_ok = g_out && g_parent;
#ifdef FFXI_PARTICLE_EMIT
        g_emit_ok = bytes_at(FFXI_PARTICLE_EMIT, "83 EC 4C 53 55 56 8B F1 57 C6 44 24 13 00 F6 86 DC 00 00 00 01");
#endif
        if (!g_emit_ok)
            rt_log("[recomp] ps2fix: build %s's particle emit isn't where expected; disintegrate without its smoke\n",
                FFXI_BUILD);
        if (g_disint_ok)
            rt_hook_particle_op = particle_op;
    }
#else
    (void)g_disint_ok, (void)g_emit_ok;
    rt_log("[recomp] ps2fix: build %s has no particle op hook; disintegrate off\n", FFXI_BUILD);
#endif
    rt_hook_skin_part = skin_part;
#else
    (void)split_part, (void)split_undo, (void)bytes_at, (void)g_split_ok, (void)g_disint_ok, (void)g_emit_ok;
    rt_log("[recomp] ps2fix: build %s has no skinning hook; shine batches and disintegrate off\n", FFXI_BUILD);
#endif
}
