/* The scene renderer's taps on the game: the zone drawn from the game's own data (gfx_world_draw), not from
 * its D3D8 draws. Off unless the scene setting asks for it (fx.txt "scene", FFXI_FX_SCENE): 1 lit per vertex
 * as the game lights it, 2 per pixel.
 *
 * The game walks its zone (CTsZoneMap, 1017b510 in 2026-09-03) and, for each placed instance that passes its
 * distance and frustum tests, picks a model by distance; host64's level-of-detail hook sits just after that
 * pick (1017cb9c) and hands it here. An instance this takes is recorded - its world matrix, model, raster
 * flags, environment and the point lights the game would have enabled for it - and its model is returned as
 * 0, so the game draws nothing of it. The batch goes to the back end before the next piece of work that
 * would follow it (d3d8_set_world_flush): the game's doors, characters and effects come after it into the
 * same target and depth, as they did after the game's own zone draws. What it can't draw - morphing (kind 2)
 * and bump-mapped (kind 3) models, a submesh without a texture or buffer - it leaves to the game.
 *
 * Lighting comes from the same places the game's fixed-function draw takes it (1017c9c0): per instance the
 * environment object's sun (or sun and moon) light, ambient and fog through the game's own getters, the
 * windowed brightness factor through its own 1000be50, and the four point lights from the instance's handles
 * into the zone's light list and the light pool. The game keeps a point light enabled in its D3D slot while
 * the next instances' handles are unchanged or name a light no effect has lit: that is followed here, in the
 * order the game draws.
 *
 * Every address is the 2026-09-03 build's (meta/builds.json "scene_*"); a build without them never turns
 * this on. Checked in the client: docs (FFXI Scene Data and a Replacement Renderer, Phase 0). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "d3d8.h"
#include "gfx.h"
#include "gthread.h"
#include "gwin.h"
#include "scene.h"

#if defined(FFXI_SCENE_MGR) && defined(FFXI_SCENE_DEV) && defined(FFXI_SCENE_LIGHT_FACTOR) && defined(FFXI_SCENE_FOG) && \
    defined(FFXI_SCENE_SUN) && defined(FFXI_SCENE_SUN_PAIR) && defined(FFXI_SCENE_AMBIENT) && \
    defined(FFXI_SCENE_ZBIAS_BLEND) && defined(FFXI_SCENE_ZBIAS_OPAQUE) && defined(FFXI_HOOK_LOD_PICK)
#define HAVE_SCENE 1
#endif

/* CTsZoneMap (mgr) and its instances, models and submeshes (2026-09-03) */
enum
{
    MGR_VIEW = 0x38d9c,      /* D3DMATRIX view, projection after it */
    MGR_PROJ = 0x38ddc,
    MGR_MOVING = 0x38e1c,    /* the moving sub-map's matrix (a ship) */
    MGR_SECOND = 0x394a4,    /* a second directional light */
    MGR_DEF_SUN = 0x39180,   /* with no environment: D3DLIGHT8 sun, second light, ambient, fog colour, end, start */
    MGR_DEF_SUN2 = 0x391e8,
    MGR_DEF_AMBIENT = 0x39250,
    MGR_DEF_FOGCOL = 0x39254,
    MGR_DEF_FOGEND = 0x39258,
    MGR_DEF_FOGSTART = 0x3925c,
    INST_DEFERRED = 0x0f, /* byte: drawn after the walk (translucent) */
    INST_NORMALIZE = 0x0e,
    INST_WORLD = 0x40,
    INST_INV = 0x80,
    INST_MIRRORED = 0xd4, /* word */
    INST_MODE = 0xd6,     /* word: 1 alpha test, 2 clamped addressing */
    INST_ENV = 0xe0,
    INST_LIGHTS = 0xe4,   /* 4 handles, 1-based into the light list */
    MODEL_KIND = 0x1c,    /* 0 list, 1 strip, 2 morphing, 3 bump */
    MODEL_PRIM = 0x2c,
    MODEL_NGROUPS = 0x44,
    MODEL_GROUPS = 0x60,
    SUB_TEX = 0x04,
    SUB_PRIMS = 0x18,
    SUB_BLEND = 0x20,
    SUB_TWOSIDED = 0x24,
    SUB_VB = 0x28,
};

typedef struct Pick
{
    uint32_t inst, env;
    float world[16], inv[16];
    uint32_t light[GFX_WORLD_LIGHTS]; /* pool entries (their D3DLIGHT8 at +8), 0 none */
    uint32_t flags;                   /* GFX_WORLD_NORMALIZE */
} Pick;

typedef struct List
{
    GfxWorldItem* v;
    uint32_t n, cap;
} List;

static int g_have, g_on;      /* the build and back end can; this frame's setting is on (1, 2) */
static int g_log;             /* FFXI_SCENELOG=1: counts every two seconds */
static Pick* g_picks;
static uint32_t g_npicks, g_cappicks;
static List g_opaque, g_blend, g_late; /* the walk's opaque submeshes, its blended ones, the deferred instances' */
static int g_pending, g_flushing;
static uint32_t g_scratch;           /* guest memory for the getters' results */
/* the game's point-light slots as the walk leaves them (1017c9c0): handles last seen and lights enabled */
static uint32_t g_slot_list, g_slot_handle[GFX_WORLD_LIGHTS], g_slot_light[GFX_WORLD_LIGHTS];
static GfxWorldInst* g_inst;
static GfxWorldLight* g_lights;
static uint32_t g_capinst, g_caplights;
static struct
{
    uint32_t picks, left, items, flushes, fx_calls, frames;
} g_count;

static void list_add(List* l, const GfxWorldItem* it)
{
    if (l->n == l->cap)
    {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->v = (GfxWorldItem*)realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = *it;
}

static void mat_mul(float* out, const float* a, const float* b)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}

static void mat_inv(float* out, const float* m) /* general 4x4 (the moving sub-map's instances) */
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    det = det != 0.0f ? 1.0f / det : 0.0f;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] * det;
}

static void read_mat(float* m, uint32_t a)
{
    for (int i = 0; i < 16; ++i)
        m[i] = rdf32(a + 4u * i);
}

#ifdef HAVE_SCENE
/* the walk's point lights, as 1017c9c0 enables them: a slot changes only when the instance's handle differs
 * from the last one seen there; a handle of 0 turns it off, one naming a lit light ('c' lights and those no
 * effect has registered are not) turns it to that light, and any other leaves it as it was */
static void slot_lights(uint32_t inst, uint32_t list, int deferred, uint32_t* out)
{
    if (list != g_slot_list && !deferred)
    {
        g_slot_list = list; /* a new slot (MZB) of the walk: 1017b510 turns the four off */
        memset(g_slot_handle, 0, sizeof g_slot_handle), memset(g_slot_light, 0, sizeof g_slot_light);
    }
    for (int k = 0; k < GFX_WORLD_LIGHTS; ++k)
    {
        uint32_t h = rd32(inst + INST_LIGHTS + 4u * k);
        if (h == g_slot_handle[k])
            continue;
        g_slot_handle[k] = h;
        if (!h)
        {
            g_slot_light[k] = 0;
            continue;
        }
        uint32_t e = list ? rd32(list + 4 + (h - 1) * 0x4c) : 0;
        if (e && rd32(e + 8) && rd8(e) != 'c')
            g_slot_light[k] = e;
    }
    memcpy(out, g_slot_light, sizeof g_slot_light);
}

uint32_t scene_zone_pick(uint32_t inst, uint32_t model, uint32_t lights, int moving)
{
    if (!g_on || !model || !inst)
        return model;
    int deferred = rd8(inst + INST_DEFERRED) != 0;
    Pick p;
    slot_lights(inst, lights, deferred, p.light); /* every drawn instance moves the slots, ours or the game's */
    ++g_count.picks;
    uint32_t kind = rd32(model + MODEL_KIND), prim = rd32(model + MODEL_PRIM);
    uint32_t ngroups = rd32(model + MODEL_NGROUPS), groups = rd32(model + MODEL_GROUPS);
    if (kind > 1 || (prim != GFX_TRIANGLELIST && prim != GFX_TRIANGLESTRIP) || !groups || ngroups > 64)
    {
        ++g_count.left;
        return model;
    }
    uint16_t mirrored = rd16(inst + INST_MIRRORED), mode = rd16(inst + INST_MODE);
    /* every submesh must be drawable before the game is told to skip the instance */
    GfxWorldItem items[256];
    uint32_t n = 0;
    for (uint32_t gi = 0; gi < ngroups; ++gi)
    {
        uint32_t g = groups + gi * 0x24, nsub = rd32(g), subs = rd32(g + 4);
        for (uint32_t si = 0; si < nsub && subs; ++si)
        {
            uint32_t s = subs + si * 0xc0, texo = rd32(s + SUB_TEX), vb = rd32(s + SUB_VB), count = rd32(s + SUB_PRIMS);
            if (!count)
                continue;
            uint32_t iface = texo ? rd32(texo + 0x40) : 0;
            if (texo && !iface)
                iface = rd32(texo + 0x44);
            GfxTex* t = iface ? d3d8_world_texture(iface) : NULL;
            uint32_t size = 0;
            GfxBuf* b = vb ? d3d8_world_vbuf(vb, &size) : NULL;
            uint32_t need = (prim == GFX_TRIANGLESTRIP ? count + 2 : count * 3) * 36u;
            if (!t || !b || size < need || n == sizeof items / sizeof items[0])
            {
                ++g_count.left;
                return model;
            }
            GfxWorldItem* it = &items[n++];
            it->vb = b, it->tex = t, it->first = 0, it->count = count, it->prim = prim, it->inst = g_npicks;
            d3d8_world_sampler(iface, &it->samp); /* the walk set the filters before its first instance */
            it->flags = (rd16(s + SUB_BLEND) ? GFX_WORLD_BLEND : 0) | (rd16(s + SUB_TWOSIDED) ? GFX_WORLD_TWOSIDED : 0) |
                (mirrored ? GFX_WORLD_MIRRORED : 0) | (mode == 1 ? GFX_WORLD_ATEST : 0) | (mode == 2 ? GFX_WORLD_CLAMP : 0) |
                (rd8(inst + INST_NORMALIZE) ? GFX_WORLD_NORMALIZE : 0);
        }
    }
    p.inst = inst, p.env = rd32(inst + INST_ENV), p.flags = 0;
    read_mat(p.world, inst + INST_WORLD);
    read_mat(p.inv, inst + INST_INV);
    if (moving)
    {
        float mv[16];
        read_mat(mv, rd32(FFXI_SCENE_MGR) + MGR_MOVING);
        mat_mul(p.world, p.world, mv);
        mat_inv(p.inv, p.world);
    }
    if (g_npicks == g_cappicks)
    {
        g_cappicks = g_cappicks ? g_cappicks * 2 : 1024;
        g_picks = (Pick*)realloc(g_picks, g_cappicks * sizeof *g_picks);
    }
    g_picks[g_npicks++] = p;
    for (uint32_t i = 0; i < n; ++i)
        list_add(deferred ? &g_late : (items[i].flags & GFX_WORLD_BLEND) ? &g_blend : &g_opaque, &items[i]);
    g_count.items += n;
    g_pending = 1;
    return 0;
}

/* --- lighting, through the game's own getters ----------------------------------------------------------- */
static void color4(float* out, uint32_t c) /* D3DCOLOR */
{
    out[0] = (float)((c >> 16) & 255) / 255.0f, out[1] = (float)((c >> 8) & 255) / 255.0f, out[2] = (float)(c & 255) / 255.0f;
    out[3] = (float)(c >> 24) / 255.0f;
}

/* a D3DLIGHT8 in guest memory as D3D sets it through the game's SetLight wrapper (1000bf30): with the windowed
 * brightness factor (1000be50) on a copy */
static void light_factor(uint32_t l)
{
    uint32_t dev = rd32(FFXI_SCENE_DEV);
    if (dev)
        guest_thiscall(FFXI_SCENE_LIGHT_FACTOR, dev, 1, (uint32_t[]){ l });
}

static void dir_light(float* dir, float* col, uint32_t l) /* a directional D3DLIGHT8 at l */
{
    float d[3] = { rdf32(l + 0x40), rdf32(l + 0x44), rdf32(l + 0x48) };
    float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (rd32(l) != 3 || n <= 0.0f)
    {
        memset(dir, 0, 16), memset(col, 0, 16);
        return;
    }
    dir[0] = -d[0] / n, dir[1] = -d[1] / n, dir[2] = -d[2] / n, dir[3] = 1.0f;
    col[0] = rdf32(l + 4), col[1] = rdf32(l + 8), col[2] = rdf32(l + 12), col[3] = 1.0f;
}

typedef struct EnvLight
{
    uint32_t env;
    float sun_dir[4], sun_col[4], sun2_dir[4], sun2_col[4], ambient[4], fog[4], fog_col[4];
} EnvLight;

static void env_light(EnvLight* e, uint32_t env, uint32_t mgr)
{
    memset(e, 0, sizeof *e);
    e->env = env;
    uint32_t L1 = g_scratch, L2 = g_scratch + 0x80, V = g_scratch + 0x100; /* two lights; colour, end, start, ambient */
    int second = rd32(mgr + MGR_SECOND) != 0;
    uint32_t amb, fogc;
    float fend, fstart;
    if (env)
    {
        guest_thiscall(FFXI_SCENE_FOG, env, 4, (uint32_t[]){ V, V + 4, V + 8, 0 });
        fogc = rd32(V), fend = rdf32(V + 4), fstart = rdf32(V + 8);
        if (second)
            guest_thiscall(FFXI_SCENE_SUN_PAIR, env, 2, (uint32_t[]){ L1, L2 });
        else
            guest_thiscall(FFXI_SCENE_SUN, env, 2, (uint32_t[]){ L1, 0 });
        guest_thiscall(FFXI_SCENE_AMBIENT, env, 2, (uint32_t[]){ V + 12, 0 });
        amb = rd32(V + 12);
    }
    else
    {
        memcpy(GUEST_PTR(L1), GUEST_PTR(mgr + MGR_DEF_SUN), 0x68);
        memcpy(GUEST_PTR(L2), GUEST_PTR(mgr + MGR_DEF_SUN2), 0x68);
        amb = rd32(mgr + MGR_DEF_AMBIENT), fogc = rd32(mgr + MGR_DEF_FOGCOL);
        fend = rdf32(mgr + MGR_DEF_FOGEND), fstart = rdf32(mgr + MGR_DEF_FOGSTART);
    }
    light_factor(L1);
    dir_light(e->sun_dir, e->sun_col, L1);
    if (second)
    {
        light_factor(L2);
        dir_light(e->sun2_dir, e->sun2_col, L2);
    }
    color4(e->ambient, amb);
    color4(e->fog_col, fogc);
    e->fog[0] = fstart, e->fog[1] = fend > fstart ? fend : fstart + 1.0f, e->fog[2] = 1.0f;
}

/* a pool light (its D3DLIGHT8 at entry + 8) into the frame's list, once */
static int32_t frame_light(uint32_t entry, uint32_t* seen, uint32_t* nlights)
{
    for (uint32_t i = 0; i < *nlights; ++i)
        if (seen[i] == entry)
            return (int32_t)i;
    if (*nlights == g_caplights)
        return -1;
    uint32_t l = g_scratch + 0x180;
    memcpy(GUEST_PTR(l), GUEST_PTR(entry + 8), 0x68);
    light_factor(l);
    GfxWorldLight* w = &g_lights[*nlights];
    memset(w, 0, sizeof *w);
    w->diffuse[0] = rdf32(l + 4), w->diffuse[1] = rdf32(l + 8), w->diffuse[2] = rdf32(l + 12);
    w->ambient[0] = rdf32(l + 0x24), w->ambient[1] = rdf32(l + 0x28), w->ambient[2] = rdf32(l + 0x2c);
    w->pos[0] = rdf32(l + 0x34), w->pos[1] = rdf32(l + 0x38), w->pos[2] = rdf32(l + 0x3c), w->pos[3] = rdf32(l + 0x4c);
    w->att[0] = rdf32(l + 0x54), w->att[1] = rdf32(l + 0x58), w->att[2] = rdf32(l + 0x5c);
    if (rd32(l) != 1 || w->pos[3] <= 0.0f || w->att[2] <= 0.0f) /* what the SetLight wrapper refuses */
        return -1;
    seen[*nlights] = entry;
    return (int32_t)(*nlights)++;
}

#if defined(FFXI_WRAP_MESH_DRAW) && defined(FFXI_SCENE_RES_MGR) && defined(FFXI_SCENE_TEX_FIND)
static void chars_flush(const uint32_t vp[6], const uint32_t gvp[6]);
static void chars_reset(void);
#define CHARS_FLUSH(vp, gvp) chars_flush(vp, gvp)
#define CHARS_RESET() chars_reset()
#else
#define CHARS_FLUSH(vp, gvp) ((void)0)
#define CHARS_RESET() ((void)0)
#endif

static void flush(void)
{
    if (!g_pending || g_flushing)
        return;
    g_pending = 0;
    g_flushing = 1;
    uint32_t mgr = rd32(FFXI_SCENE_MGR), vp[6], gvp[6];
    if (!mgr || !d3d8_world_begin(vp, gvp))
        goto done;
    if (g_capinst < g_npicks)
    {
        g_capinst = g_npicks + 256;
        g_inst = (GfxWorldInst*)realloc(g_inst, g_capinst * sizeof *g_inst);
    }
    if (!g_caplights)
    {
        g_caplights = 256;
        g_lights = (GfxWorldLight*)malloc(g_caplights * sizeof *g_lights);
    }
    float f_view[16], f_proj[16];
    read_mat(f_view, mgr + MGR_VIEW);
    read_mat(f_proj, mgr + MGR_PROJ);
    static EnvLight envs[64];
    uint32_t nenvs = 0, nlights = 0, seen[256];
    for (uint32_t i = 0; i < g_npicks; ++i)
    {
        const Pick* p = &g_picks[i];
        GfxWorldInst* w = &g_inst[i];
        const EnvLight* e = NULL;
        for (uint32_t k = 0; k < nenvs && !e; ++k)
            if (envs[k].env == p->env)
                e = &envs[k];
        if (!e)
        {
            EnvLight* ne = &envs[nenvs < 64 ? nenvs++ : 63];
            env_light(ne, p->env, mgr);
            e = ne;
        }
        float wv[16];
        mat_mul(wv, p->world, f_view);
        mat_mul(w->wvp, wv, f_proj);
        memcpy(w->world, p->world, sizeof w->world);
        memcpy(w->world_inv, p->inv, sizeof w->world_inv);
        memcpy(w->sun_dir, e->sun_dir, sizeof w->sun_dir), memcpy(w->sun_col, e->sun_col, sizeof w->sun_col);
        memcpy(w->sun2_dir, e->sun2_dir, sizeof w->sun2_dir), memcpy(w->sun2_col, e->sun2_col, sizeof w->sun2_col);
        memcpy(w->ambient, e->ambient, sizeof w->ambient);
        memcpy(w->fog, e->fog, sizeof w->fog), memcpy(w->fog_col, e->fog_col, sizeof w->fog_col);
        for (int k = 0; k < GFX_WORLD_LIGHTS; ++k)
            w->light[k] = p->light[k] ? frame_light(p->light[k], seen, &nlights) : -1;
    }
    uint32_t total = g_opaque.n + g_blend.n + g_late.n;
    static List all;
    all.n = 0;
    for (uint32_t i = 0; i < g_opaque.n; ++i)
        list_add(&all, &g_opaque.v[i]);
    for (uint32_t i = 0; i < g_blend.n; ++i)
        list_add(&all, &g_blend.v[i]);
    for (uint32_t i = 0; i < g_late.n; ++i)
        list_add(&all, &g_late.v[i]);
    GfxWorldFrame f;
    memset(&f, 0, sizeof f);
    memcpy(f.view, f_view, sizeof f_view);
    memcpy(f.proj, f_proj, sizeof f_proj);
    memcpy(f.vp, vp, sizeof vp);
    f.pixel[0] = gvp[2] ? 1.0f / (float)gvp[2] : 0.0f, f.pixel[1] = gvp[3] ? 1.0f / (float)gvp[3] : 0.0f;
    f.mode = g_on >= 2 ? 2 : 1;
    f.alpha_ref = 96.0f; /* the walk's ALPHAREF 0x60, GREATER */
    f.zbias_opaque = (int32_t)rd32(FFXI_SCENE_ZBIAS_OPAQUE), f.zbias_blend = (int32_t)rd32(FFXI_SCENE_ZBIAS_BLEND);
    f.inst = g_inst, f.ninst = g_npicks, f.item = all.v, f.nitem = total, f.light = g_lights, f.nlight = nlights;
    if (total)
        gfx_world_draw(&f);
    CHARS_FLUSH(vp, gvp); /* the characters after the zone, as the game draws them */
    ++g_count.flushes;
done:
    /* the next batch (a second walk this frame: the game's reflections, a menu's model view) starts empty */
    g_npicks = 0, g_opaque.n = g_blend.n = g_late.n = 0;
    CHARS_RESET();
    g_flushing = 0;
}


/* --- characters ----------------------------------------------------------------------------------------------------
 * The game draws each of a character's meshes (Os2, kind 0) through its mesh instance's slot 7 (1002a000, the "mesh_draw"
 * wrap): it resolves the resource, skins the bone-space vertices into world space on the CPU (1002f1d0) and walks the
 * mesh's command stream - materials (hide class, light scale, colour), texture names, triangle lists and strips - drawing
 * each with the game's per-actor lights and state. Taken here instead: the mesh's vertices go to the GPU once (both bones
 * and both mirror bones a vertex, in the order of the game's skinned buffers, duplicates included), the palette and the
 * per-actor state of this draw are copied, and the command stream becomes draw items that gfx_chars_draw skins and lights.
 * Meshes with an environment map, the shine pass, the silhouette shadows or a fading actor (drawn into its own target)
 * stay the game's. */
#if defined(FFXI_WRAP_MESH_DRAW) && defined(FFXI_SCENE_RES_MGR) && defined(FFXI_SCENE_RES_GET) && defined(FFXI_SCENE_PALETTE) && \
    defined(FFXI_SCENE_BONES) && defined(FFXI_SCENE_ACTOR_LIGHTS) && defined(FFXI_SCENE_ACTOR_AMBIENT) && \
    defined(FFXI_SCENE_ACTOR_MATERIAL) && defined(FFXI_SCENE_TINT) && defined(FFXI_SCENE_SHINE) && defined(FFXI_SCENE_SHADOW_MODE) && \
    defined(FFXI_SCENE_TEX_MGR) && defined(FFXI_SCENE_TEX_FIND)
#define HAVE_CHARS 1
extern GuestFn rt_wrap_mesh_draw;
extern const GuestFn rt_orig_mesh_draw;

typedef struct Mesh
{
    uint32_t res, src, n; /* the resource, its source vertices, vertices built */
    GfxBuf *vb, *uv;      /* the vertices; their texture coordinates (the game's buffer is a dynamic one) */
} Mesh;
enum
{
    MESH_SLOTS = 4096
};
static Mesh g_meshes[MESH_SLOTS];
static GfxCharItem* g_citems;
static GfxCharState* g_cstates;
static float* g_pal;
static uint32_t g_ncitems, g_capcitems, g_ncstates, g_capcstates, g_npal, g_cappal; /* g_npal in bones */
static float g_wf[3];  /* the windowed brightness factor per channel this frame (1000be50), 0 when it is off */
static struct
{
    uint32_t meshes, left, items, built, why[8]; /* why left: not kind 0, buffers, state, stream, vb, uv, ib, palette */
} g_ccount;

/* the hide flags a mesh's header byte (+0x33) sets on its model (CYyModel +0xa8 + class), as 1002dc10's jump table */
static void hide_set(uint32_t model, uint8_t v)
{
    static const uint8_t classes[][6] = { { 2, 1 }, { 3, 1, 2 }, { 4, 1, 2, 3 }, { 5, 1, 2, 3, 8, 4 }, { 6, 1, 2, 3, 8 },
        { 0x12, 5 }, { 0x22, 7 }, { 0x32, 6 } };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; ++i)
        if (classes[i][0] == v)
            for (int k = 1; k < 6 && classes[i][k]; ++k)
                wr8(model + 0xa8 + classes[i][k], 1);
}

static void cv_bones(GfxCharVertex* v, uint16_t da, uint16_t db, uint32_t remap)
{
    uint32_t ba = da & 0x7f, bb = db & 0x7f, ma = (da >> 7) & 0x7f, mb = (db >> 7) & 0x7f;
    if (remap)
        ba = rd16(remap + 2 * ba), bb = rd16(remap + 2 * bb), ma = rd16(remap + 2 * ma), mb = rd16(remap + 2 * mb);
    v->bones = (ba & 0xff) | (bb & 0xff) << 8 | (ma & 0xff) << 16 | (mb & 0xff) << 24;
    v->mirror = (uint32_t)(da >> 14) | (uint32_t)(db >> 14) << 2;
}

/* the mesh's vertices and texture coordinates on the GPU, built the first time it is drawn */
static GfxBuf* mesh_vb(uint32_t res, uint32_t vbuf, uint32_t total, GfxBuf** uv_out)
{
    uint32_t counts = rd32(res + 0x42), desc = rd32(res + 0x48), src = rd32(res + 0x4e);
    uint32_t nr = rd16(counts), nw = rd16(counts + 2), nd = rd32(vbuf + 0x60), dup = rd32(vbuf + 0x5c);
    uint32_t remap = (rd8(res + 0x32) & 0x80) ? rd32(res + 0x3c) : 0;
    uint32_t h = (res >> 4) * 2654435761u % MESH_SLOTS;
    for (uint32_t k = 0; k < 64; ++k)
    {
        Mesh* m = &g_meshes[(h + k) % MESH_SLOTS];
        if (m->res == res && m->src == src && m->n == total)
            return *uv_out = m->uv, m->vb;
        if (!m->res || k == 63)
        {
            uint32_t uvsize = 0, uvmem = d3d8_world_vmem(rd32(vbuf + 0x4c), &uvsize);
            if (!src || !desc || nr + nw + nd != total || total > 65536 || (nd && !dup) || !uvmem || uvsize < total * 8)
                return NULL;
            GfxCharVertex* v = (GfxCharVertex*)calloc(total, sizeof *v);
            for (uint32_t i = 0; i < nr; ++i)
            {
                uint32_t r = src + 24 * i;
                v[i].a[0] = rdf32(r), v[i].a[1] = rdf32(r + 4), v[i].a[2] = rdf32(r + 8), v[i].a[3] = 1.0f;
                v[i].na[0] = rdf32(r + 12), v[i].na[1] = rdf32(r + 16), v[i].na[2] = rdf32(r + 20);
                uint16_t d = rd16(desc + 4 * i); /* a descriptor is 4 bytes a vertex: a rigid one uses its first word */
                cv_bones(&v[i], d, d, remap);
            }
            for (uint32_t i = 0; i < nw; ++i)
            {
                uint32_t r = src + 24 * nr + 56 * i;
                GfxCharVertex* o = &v[nr + i];
                for (int c = 0; c < 4; ++c)
                    o->a[c] = rdf32(r + 8 * c), o->b[c] = rdf32(r + 8 * c + 4);
                for (int c = 0; c < 3; ++c)
                    o->na[c] = rdf32(r + 32 + 8 * c), o->nb[c] = rdf32(r + 32 + 8 * c + 4);
                cv_bones(o, rd16(desc + 4 * (nr + i)), rd16(desc + 4 * (nr + i) + 2), remap);
            }
            for (uint32_t i = 0; i < nd; ++i)
            {
                uint32_t s = rd16(dup + 2 * i);
                v[nr + nw + i] = s < nr + nw ? v[s] : v[0];
            }
            if (m->vb)
                gfx_buf_destroy(m->vb), gfx_buf_destroy(m->uv);
            m->vb = gfx_buf_create(total * (uint32_t)sizeof *v);
            gfx_buf_upload(m->vb, v, total * (uint32_t)sizeof *v);
            free(v);
            m->uv = gfx_buf_create(total * 8);
            gfx_buf_upload(m->uv, GUEST_PTR(uvmem), total * 8);
            m->res = res, m->src = src, m->n = total;
            ++g_ccount.built;
            return *uv_out = m->uv, m->vb;
        }
    }
    return NULL;
}

/* the material's texture factor, as 1002dc10 computes it from the material colour and the actor's tint */
static uint32_t tfactor(uint32_t c, uint32_t t)
{
    uint32_t g = (((c >> 7) & 0xfffffffeu) * (t >> 8)) & 0xff00u;
    uint32_t r = (((c >> 7) & 0xfffffe00u) * (t >> 16 & 0xffffu)) & 0xff0000u;
    uint32_t b = ((c * t) >> 7) & 0xffu;
    return g | r | b | 0x80000000u;
}

static void state_light(GfxCharLight* o, uint32_t l, float scale)
{
    memset(o, 0, sizeof *o);
    uint32_t type = rd32(l);
    float d[3] = { rdf32(l + 4) * scale, rdf32(l + 8) * scale, rdf32(l + 12) * scale };
    if (g_wf[0] > 0.0f && d[0] < 0.8f && d[1] < 0.8f && d[2] < 0.8f) /* the windowed factor, as the SetLight wrapper's */
        for (int c = 0; c < 3; ++c)
            d[c] = d[c] * g_wf[c] < 1.0f ? d[c] * g_wf[c] : 1.0f;
    o->diffuse[0] = d[0], o->diffuse[1] = d[1], o->diffuse[2] = d[2];
    o->ambient[0] = rdf32(l + 0x24), o->ambient[1] = rdf32(l + 0x28), o->ambient[2] = rdf32(l + 0x2c);
    if (type == 3)
    {
        float x = rdf32(l + 0x40), y = rdf32(l + 0x44), z = rdf32(l + 0x48), n = sqrtf(x * x + y * y + z * z);
        if (n > 0.0f)
            o->dir[0] = -x / n, o->dir[1] = -y / n, o->dir[2] = -z / n, o->dir[3] = 1.0f;
    }
    else if (type == 1 && rdf32(l + 0x4c) > 0.0f && rdf32(l + 0x5c) > 0.0f)
    {
        o->pos[0] = rdf32(l + 0x34), o->pos[1] = rdf32(l + 0x38), o->pos[2] = rdf32(l + 0x3c), o->pos[3] = rdf32(l + 0x4c);
        o->att[0] = rdf32(l + 0x54), o->att[1] = rdf32(l + 0x58), o->att[2] = rdf32(l + 0x5c);
        o->dir[3] = 2.0f;
    }
}

/* one mesh draw taken (1 and the draw items recorded) or left to the game (0) */
static int chars_take(Guest* g, uint32_t res, uint32_t model)
{
    if (!res || rd8(res + 0x30) > 1 || (rd8(res + 0x32) & 0x7f) != 0)
        return ++g_ccount.why[0], 0;
    uint32_t vbuf = rd32(res + 0x5e), cmd = rd32(res + 0x36), counts = rd32(res + 0x42);
    if (!vbuf || !cmd || !counts || !rd8(vbuf + 0x68) || !rd32(vbuf + 0x40) || !rd32(vbuf + 0x4c))
        return ++g_ccount.why[1], 0;
    uint32_t tint = rd32(FFXI_SCENE_TINT);
    if ((tint >> 24) != 0x80 || rd8(FFXI_SCENE_SHINE) || rd8(FFXI_SCENE_SHADOW_MODE) == 1)
        return ++g_ccount.why[2], 0; /* a fading actor (its own target), the shine pass, the silhouette shadows */
    uint32_t total = rd16(counts) + rd16(counts + 2) + rd32(vbuf + 0x60);
    /* the command stream once without drawing: no environment maps, a texture before every draw, nothing unknown */
    int have_tex = 0;
    for (uint32_t p = cmd, guard = 0; guard < 100000; ++guard)
    {
        uint16_t op = rd16(p);
        if (op == 0xffff)
            break;
        if (op & 0x8000)
        {
            uint32_t k = (op >> 4) & 15;
            if (k == 1)
            {
                if (rd16(p + 2 + 0xe) || rdf32(p + 2 + 0x28) != 0.0f)
                    return ++g_ccount.why[3], 0; /* environment-mapped */
                p += 0x2e;
            }
            else if (k == 0)
                have_tex = 1, p += 0x12;
            else
                return ++g_ccount.why[3], 0;
            continue;
        }
        uint32_t n = rd16(p + 2);
        if (op == 0x54 || op == 0x5453)
        {
            if (!have_tex)
                return ++g_ccount.why[3], 0;
            p += op == 0x54 ? 4 + n * 30 : 0x18 + n * 10;
        }
        else if (op == 0x43)
            p += 4 + n * 10;
        else if (op == 0x4353)
            p += 0xc + n * 2;
        else
            return ++g_ccount.why[3], 0;
    }
    GfxBuf* uv = NULL;
    GfxBuf* vb = mesh_vb(res, vbuf, total, &uv);
    uint32_t uvsize = total * 8, ibsize = 0, isize = 2;
    GfxBuf* ib = d3d8_world_ibuf(rd32(vbuf + 0x40), &ibsize, &isize);
    uint32_t pal = rd32(FFXI_SCENE_PALETTE), nb = rd32(FFXI_SCENE_BONES) & 0xff;
    if (!vb || !uv || !ib || uvsize < total * 8 || !pal || !nb || nb > 128)
    {
        static int told;
        if (g_log && told < 12 && (!uv || uvsize < total * 8))
            ++told, rt_log("[recomp] scene: characters: uv %p size %u for %u vertices (%u rigid, %u weighted, +0x60 %u, +0x58 %u)\n",
                (void*)uv, uvsize, total, rd16(counts), rd16(counts + 2), rd32(vbuf + 0x60), rd32(vbuf + 0x58));
        return ++g_ccount.why[!vb ? 4 : !uv || uvsize < total * 8 ? 5 : !ib ? 6 : 7], 0;
    }
    /* taken: the per-actor state of this draw */
    hide_set(model, rd8(res + 0x33));
    if (g_npal + nb > g_cappal)
    {
        g_cappal = (g_npal + nb) * 2 + 1024;
        g_pal = (float*)realloc(g_pal, (size_t)g_cappal * 64);
    }
    uint32_t palbase = g_npal;
    memcpy(g_pal + 16 * (size_t)g_npal, GUEST_PTR(pal), (size_t)nb * 64);
    g_npal += nb;
    float W[16], V[16], P[16], WV[16];
    if (!d3d8_get_transform(256, W) || !d3d8_get_transform(2, V) || !d3d8_get_transform(3, P))
        return 0;
    mat_mul(WV, W, V);
    uint32_t fog_on = 0, fog_col = 0, fs = 0, fe = 0;
    d3d8_get_render_state(28, &fog_on), d3d8_get_render_state(34, &fog_col), d3d8_get_render_state(36, &fs),
        d3d8_get_render_state(37, &fe);
    uint32_t amb = rd32(FFXI_SCENE_ACTOR_AMBIENT), mat = FFXI_SCENE_ACTOR_MATERIAL;
    int mirror = rd8(res + 0x34) & 1;
    uint32_t cur_state = ~0u, idx = 0, drawn = 0;
    GfxTex* tex = NULL;
    uint32_t tex_iface = 0;
    int hidden = 0;
    for (uint32_t p = cmd, guard = 0; guard < 100000; ++guard)
    {
        uint16_t op = rd16(p);
        if (op == 0xffff)
            break;
        if (op & 0x8000)
        {
            if (((op >> 4) & 15) == 1)
            {
                uint32_t rec = p + 2;
                uint32_t cls = (rd32(rec + 0xc) >> 8) & 15;
                if (rd8(vbuf + 0x69) && rd16(rec + 0x1c))
                    cls = 8;
                hidden = rd8(model + 0xa8 + cls) != 0;
                float f = rdf32(rec + 0x10);
                if (g_ncstates == g_capcstates)
                {
                    g_capcstates = g_capcstates ? g_capcstates * 2 : 256;
                    g_cstates = (GfxCharState*)realloc(g_cstates, g_capcstates * sizeof *g_cstates);
                }
                GfxCharState* s = &g_cstates[g_ncstates];
                memset(s, 0, sizeof *s);
                mat_mul(s->wvp, WV, P);
                memcpy(s->world, W, sizeof W);
                for (int k = 0; k < 4; ++k)
                    state_light(&s->light[k], FFXI_SCENE_ACTOR_LIGHTS + 0x68u * k, f);
                for (int c = 0; c < 3; ++c) /* the ambient bytes times the material's scale, truncated, at most 255 */
                {
                    float v = (float)((amb >> (8 * (2 - c))) & 255) * f;
                    uint32_t b = v > 0.0f ? (uint32_t)v : 0;
                    s->ambient[c] = (float)(b > 255 ? 255 : b) / 255.0f;
                }
                for (int c = 0; c < 4; ++c)
                    s->mat_diffuse[c] = rdf32(mat + 4u * c), s->mat_ambient[c] = rdf32(mat + 16 + 4u * c),
                    s->mat_emissive[c] = rdf32(mat + 48 + 4u * c);
                uint32_t tf = tfactor(rd32(rec), tint);
                s->tfactor[0] = (float)((tf >> 16) & 255) / 255.0f, s->tfactor[1] = (float)((tf >> 8) & 255) / 255.0f;
                s->tfactor[2] = (float)(tf & 255) / 255.0f, s->tfactor[3] = (float)(tf >> 24) / 255.0f;
                float fsf, fef;
                memcpy(&fsf, &fs, 4), memcpy(&fef, &fe, 4);
                s->fog[0] = fsf, s->fog[1] = fef > fsf ? fef : fsf + 1.0f, s->fog[2] = fog_on ? 1.0f : 0.0f;
                s->fog_col[0] = (float)((fog_col >> 16) & 255) / 255.0f, s->fog_col[1] = (float)((fog_col >> 8) & 255) / 255.0f;
                s->fog_col[2] = (float)(fog_col & 255) / 255.0f;
                cur_state = g_ncstates++;
                p += 0x2e;
            }
            else
            {
                if (!(op & 1)) /* resolved once by name, as the game does, and cached in the stream */
                {
                    uint32_t o = guest_thiscall(FFXI_SCENE_TEX_FIND, rd32(FFXI_SCENE_TEX_MGR), 2, (uint32_t[]){ p + 2, res });
                    wr8(p, rd8(p) | 1);
                    wr32(p + 2, o);
                }
                uint32_t o = rd32(p + 2);
                tex_iface = o ? (rd32(o + 0x40) ? rd32(o + 0x40) : rd32(o + 0x44)) : 0;
                tex = tex_iface ? d3d8_world_texture(tex_iface) : NULL;
                p += 0x12;
            }
            continue;
        }
        uint32_t n = rd16(p + 2);
        if (op == 0x54 || op == 0x5453)
        {
            uint32_t nidx = op == 0x54 ? n * 3 : n + 2;
            if (!hidden && tex && cur_state != ~0u && idx + nidx <= ibsize / isize)
                for (int m = 0; m <= mirror; ++m)
                {
                    if (g_ncitems == g_capcitems)
                    {
                        g_capcitems = g_capcitems ? g_capcitems * 2 : 1024;
                        g_citems = (GfxCharItem*)realloc(g_citems, g_capcitems * sizeof *g_citems);
                    }
                    GfxCharItem* it = &g_citems[g_ncitems++];
                    it->vb = vb, it->uv = uv, it->ib = ib, it->index_size = isize;
                    it->first = idx, it->count = n, it->prim = op == 0x54 ? GFX_TRIANGLELIST : GFX_TRIANGLESTRIP;
                    it->palette = palbase, it->state = cur_state, it->mirror = (uint32_t)m, it->tex = tex;
                    d3d8_world_sampler(tex_iface, &it->samp);
                    ++g_ccount.items;
                }
            idx += nidx, drawn += nidx;
            p += op == 0x54 ? 4 + n * 30 : 0x18 + n * 10;
        }
        else if (op == 0x43)
            p += 4 + n * 10;
        else
            p += 0xc + n * 2;
    }
    /* what 1002dc10 leaves behind: the model skinned this frame, the index count in its fifth argument */
    wr8(model + 0xb8, 1);
    uint32_t a5 = rt_arg(g, 4), a6 = rt_arg(g, 5);
    if (a5)
        wr32(a5, drawn);
    if (a6)
        wr32(a6, 0);
    g_pending = 1;
    return 1;
}

static int g_chars_off; /* FFXI_SCENE_CHARS=0: the zone only */

static void mesh_draw(Guest* g)
{
    if (!g_on || g_flushing || g_chars_off)
    {
        rt_orig_mesh_draw(g);
        return;
    }
    uint32_t mesh = g->ecx, model = rt_arg(g, 1), handle = rd32(mesh + 8);
    uint32_t mgr = rd32(FFXI_SCENE_RES_MGR);
    uint32_t res = mgr && handle ? guest_thiscall(FFXI_SCENE_RES_GET, mgr, 1, (uint32_t[]){ handle }) : 0;
    ++g_ccount.meshes;
    if (!chars_take(g, res, model))
    {
        ++g_ccount.left;
        rt_orig_mesh_draw(g);
        return;
    }
    rt_return(g, 0, 0x18);
}

static void chars_reset(void) { g_ncitems = g_ncstates = g_npal = 0; }

static void chars_flush(const uint32_t vp[6], const uint32_t gvp[6])
{
    if (!g_ncitems)
        return;
    GfxCharFrame f;
    memset(&f, 0, sizeof f);
    memcpy(f.vp, vp, sizeof f.vp);
    f.pixel[0] = gvp[2] ? 1.0f / (float)gvp[2] : 0.0f, f.pixel[1] = gvp[3] ? 1.0f / (float)gvp[3] : 0.0f;
    f.alpha_ref = 69.0f; /* the model state's ALPHAREF 0x45, GREATER */
    f.palette = g_pal, f.nbones = g_npal, f.state = g_cstates, f.nstate = g_ncstates, f.item = g_citems, f.nitem = g_ncitems;
    gfx_chars_draw(&f);
}

/* this frame's windowed brightness factor per channel (1000be50 on a grey light), 0 when the game isn't windowed */
static void chars_frame(void)
{
    uint32_t l = g_scratch + 0x180, dev = rd32(FFXI_SCENE_DEV);
    memset(GUEST_PTR(l), 0, 0x68);
    wr32(l, 3);
    float half = 0.5f, one = 1.0f;
    for (int c = 0; c < 3; ++c)
        memcpy(GUEST_PTR(l + 4 + 4u * c), &half, 4);
    memcpy(GUEST_PTR(l + 0x44), &one, 4);
    g_wf[0] = g_wf[1] = g_wf[2] = 0.0f;
    if (dev && (guest_thiscall(FFXI_SCENE_LIGHT_FACTOR, dev, 1, (uint32_t[]){ l }) & 0xff))
        for (int c = 0; c < 3; ++c)
            g_wf[c] = rdf32(l + 4 + 4u * c) / 0.5f;
}
#endif /* characters */

#ifdef FFXI_HOOK_FX_DRAW
extern GuestFn rt_hook_fx_draw;
/* 10046070, the one draw entry of every effect element (ordering-table flush): counted for the log */
static void fx_draw(Guest* g)
{
    (void)g;
    ++g_count.fx_calls;
}
#endif
#endif /* HAVE_SCENE */

void scene_setup(void)
{
    const char* log = getenv("FFXI_SCENELOG");
    g_log = log && log[0] && log[0] != '0';
#ifdef HAVE_SCENE
    g_have = 1;
    g_scratch = gheap_alloc(0x200, 1);
    d3d8_set_world_flush(flush);
#ifdef HAVE_CHARS
    rt_wrap_mesh_draw = mesh_draw;
    const char* ch = getenv("FFXI_SCENE_CHARS");
    g_chars_off = ch && ch[0] == '0';
#endif
#ifdef FFXI_HOOK_FX_DRAW
    if (g_log)
        rt_hook_fx_draw = fx_draw;
#endif
#else
    if (gfx_fx_get("scene") != 0.0f)
        rt_log("[recomp] scene: build %s has no scene renderer addresses; the game draws its zone\n", FFXI_BUILD);
#endif
}

void scene_frame(void)
{
#ifdef HAVE_SCENE
    if (g_pending)
        flush(); /* (d3d8 flushes before Present; a frame that never drew after its walk) */
    g_npicks = 0, g_opaque.n = g_blend.n = g_late.n = 0, g_pending = 0;
    CHARS_RESET();
    g_slot_list = 0;
    float s = gfx_fx_get("scene");
    int want = s >= 0.5f ? (s >= 1.5f ? 2 : 1) : 0;
#if defined(FFXI_SCENE_ZONE_DRAW) && defined(FFXI_SCENE_ZONE_DRAW_END) && defined(FFXI_SCENE_CHAR_DRAW) && \
    defined(FFXI_SCENE_CHAR_DRAW_END)
    /* the game's own zone and character draws (what's left to the game) leave the batch be; -1: the game draws its zone
     * and characters, each run of their draws timed as a pass of its own (to compare) */
    if (want || s <= -0.5f)
        d3d8_world_ranges(FFXI_SCENE_ZONE_DRAW, FFXI_SCENE_ZONE_DRAW_END, FFXI_SCENE_CHAR_DRAW, FFXI_SCENE_CHAR_DRAW_END, s <= -0.5f);
    else
        d3d8_world_ranges(0, 0, 0, 0, 0);
#endif
    if (want && g_have && !gfx_world_supported())
        want = 0;
    if (want != g_on)
        rt_log("[recomp] scene: the zone drawn by %s\n", want == 2 ? "the scene renderer, lit per pixel" :
            want == 1 ? "the scene renderer, lit per vertex" : "the game");
    g_on = want;
#ifdef HAVE_CHARS
    if (g_on)
        chars_frame();
#endif
    ++g_count.frames;
    if (g_log && g_count.frames >= 120)
    {
        rt_log("[recomp] scene: %u frames: %.0f instances taken, %.0f left to the game, %.0f submeshes, %.1f batches, "
               "%.0f effect draws a frame\n",
            g_count.frames, (double)(g_count.picks - g_count.left) / g_count.frames, (double)g_count.left / g_count.frames,
            (double)g_count.items / g_count.frames, (double)g_count.flushes / g_count.frames,
            (double)g_count.fx_calls / g_count.frames);
        memset(&g_count, 0, sizeof g_count);
#ifdef HAVE_CHARS
        rt_log("[recomp] scene: characters: %u mesh draws taken, %u left to the game (kind %u, buffers %u, state %u, stream %u, "
               "vb %u, uv %u, ib %u, palette %u), %u draw items, %u meshes built\n",
            g_ccount.meshes - g_ccount.left, g_ccount.left, g_ccount.why[0], g_ccount.why[1], g_ccount.why[2], g_ccount.why[3],
            g_ccount.why[4], g_ccount.why[5], g_ccount.why[6], g_ccount.why[7], g_ccount.items, g_ccount.built);
        memset(&g_ccount, 0, sizeof g_ccount);
#endif
    }
#endif
}
