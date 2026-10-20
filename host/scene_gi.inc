/* --- the bounce light (the scene_gi setting; gfx_gi_zone, gfx_gi_frame, gfx_gi_metal.inc) ---------------------------
 * Included by scene.c after the shadows, whose zone key and instance layout it shares.
 *
 * The zone goes to the back end once it has stood still for a second (its instance arrays unchanged: the zone is
 * loaded): every instance the shadows take (not hidden ones, the moving sub-map, nor deferred translucent ones; doors
 * and lifts as they stood then), each at its coarsest model, its opaque submeshes of kinds 0 and 1. A new zone drops
 * the old one at once.
 *
 * Each frame the lamps lit now come from the light pool (all of them, not only those the drawn instances name: the
 * ones behind the camera light what it sees too), with the windowed brightness factor as the game sets them; the sun,
 * moon and ambient are the batch's first instance's environment, as the zone is lit. */
#define GI_POOL_LIGHTS 256
#define GI_LAMPS 64

static struct
{
    uint32_t given;  /* the zone (shadow_zone_key) the back end has, 0 none */
    uint32_t seen, stable; /* the zone seen, for how many frames in a row */
    float* world;
    uint32_t nworld, capworld;
    List items;
    uint32_t frame;   /* the frame the lamps were read in */
    GfxWorldLight lamps[GI_LAMPS];
    uint32_t nlamps;
    float sun_dir[4], sun_col[4], sun2_dir[4], sun2_col[4], ambient[4]; /* the last batch's environment */
    int have_env;
    uint32_t zones;
} g_gi;

/* every static instance's coarsest model, into g_gi.world (world and inverse, 32 floats an instance) and g_gi.items */
static void gi_gather(uint32_t mgr)
{
    g_gi.nworld = 0, g_gi.items.n = 0;
    for (uint32_t s = 0; s < 2; ++s)
    {
        uint32_t slot = mgr + SLOT_BASE + s * SLOT_SIZE, state = rd32(slot), n = rd32(slot + SLOT_COUNT), arr = rd32(slot + SLOT_INSTS);
        if (!state || state == 3 || !arr || n > 65536)
            continue;
        for (uint32_t k = 0; k < n; ++k)
        {
            uint32_t inst = arr + k * INST_SIZE;
            if (rd32(inst + INST_STATE) == 1 || rd8(inst + INST_DEFERRED))
                continue;
            uint32_t model = rd32(inst + 0x10);
            if (!model)
                model = rd32(inst + 0x14);
            if (!model)
                model = rd32(inst + 0x18);
            if (!model)
                continue;
            uint32_t kind = rd32(model + MODEL_KIND), prim = rd32(model + MODEL_PRIM);
            uint32_t ngroups = rd32(model + MODEL_NGROUPS), groups = rd32(model + MODEL_GROUPS);
            if (kind > 1 || (prim != GFX_TRIANGLELIST && prim != GFX_TRIANGLESTRIP) || !groups || ngroups > 64)
                continue;
            int added = 0;
            for (uint32_t gi = 0; gi < ngroups; ++gi)
            {
                uint32_t g = groups + gi * 0x24, nsub = rd32(g), subs = rd32(g + 4);
                for (uint32_t si = 0; si < nsub && subs && nsub < 4096; ++si)
                {
                    uint32_t sb = subs + si * 0xc0, vb = rd32(sb + SUB_VB), count = rd32(sb + SUB_PRIMS);
                    if (!count || !vb || rd16(sb + SUB_BLEND))
                        continue;
                    uint32_t size = 0;
                    GfxBuf* b = d3d8_world_vbuf(vb, &size);
                    if (!b || size < (prim == GFX_TRIANGLESTRIP ? count + 2 : count * 3) * 36u)
                        continue;
                    GfxWorldItem it;
                    memset(&it, 0, sizeof it);
                    uint32_t texo = rd32(sb + SUB_TEX), iface = texo ? rd32(texo + 0x40) : 0;
                    if (texo && !iface)
                        iface = rd32(texo + 0x44);
                    it.tex = iface ? d3d8_world_texture(iface) : NULL;
                    it.vb = b, it.count = count, it.prim = prim, it.inst = g_gi.nworld;
                    it.flags = rd16(sb + SUB_TWOSIDED) ? GFX_WORLD_TWOSIDED : 0;
                    list_add(&g_gi.items, &it);
                    added = 1;
                }
            }
            if (!added)
                continue;
            if (g_gi.nworld == g_gi.capworld)
            {
                g_gi.capworld = g_gi.capworld ? g_gi.capworld * 2 : 4096;
                g_gi.world = (float*)realloc(g_gi.world, (size_t)g_gi.capworld * 128);
            }
            float* w = g_gi.world + 32 * (size_t)g_gi.nworld++;
            read_mat(w, inst + INST_WORLD);
            read_mat(w + 16, inst + INST_INV);
        }
    }
}

/* the lamps lit now: the light pool's point lights with a colour, as the game's SetLight wrapper sets them */
static void gi_lamps(void)
{
    g_gi.nlamps = 0;
#ifdef FFXI_SCENE_LIGHT_POOL
    uint32_t l = g_scratch + 0x180;
    for (uint32_t i = 0; i < GI_POOL_LIGHTS && g_gi.nlamps < GI_LAMPS; ++i)
    {
        uint32_t e = FFXI_SCENE_LIGHT_POOL + i * 0x70u + 8u;
        if (rd32(e) != 1 || rdf32(e + 0x4c) <= 0.0f || rdf32(e + 0x5c) <= 0.0f)
            continue;
        if (rdf32(e + 4) <= 0.0f && rdf32(e + 8) <= 0.0f && rdf32(e + 12) <= 0.0f)
            continue;
        memcpy(GUEST_PTR(l), GUEST_PTR(e), 0x68);
        light_factor(l);
        GfxWorldLight* w = &g_gi.lamps[g_gi.nlamps++];
        memset(w, 0, sizeof *w);
        w->diffuse[0] = rdf32(l + 4), w->diffuse[1] = rdf32(l + 8), w->diffuse[2] = rdf32(l + 12);
        w->ambient[0] = rdf32(l + 0x24), w->ambient[1] = rdf32(l + 0x28), w->ambient[2] = rdf32(l + 0x2c);
        w->pos[0] = rdf32(l + 0x34), w->pos[1] = rdf32(l + 0x38), w->pos[2] = rdf32(l + 0x3c), w->pos[3] = rdf32(l + 0x4c);
        w->att[0] = rdf32(l + 0x54), w->att[1] = rdf32(l + 0x58), w->att[2] = rdf32(l + 0x5c);
    }
#endif
}

/* this batch's bounce light: the zone given once loaded, the lamps once a frame; env: the batch's first instance */
static void gi_frame(uint32_t mgr, const GfxWorldInst* env)
{
    float want = gfx_fx_get("scene_gi");
    uint32_t key = shadow_zone_key(mgr);
    if (key != g_gi.seen)
    {
        g_gi.seen = key, g_gi.stable = 0;
        if (g_gi.given)
            gfx_gi_zone(NULL), g_gi.given = 0;
    }
    else if (g_gi.stable < 1000000)
        ++g_gi.stable;
    if (want <= 0.0f)
    {
        gfx_gi_frame(NULL);
        return;
    }
    if (!g_gi.given && g_gi.stable >= 60)
    {
        gi_gather(mgr);
        GfxGiZone z;
        memset(&z, 0, sizeof z);
        /* world and inverse interleaved: the back end takes two arrays */
        static float* wl;
        static float* il;
        static uint32_t cap;
        if (cap < g_gi.nworld)
            cap = g_gi.nworld, wl = (float*)realloc(wl, (size_t)cap * 64), il = (float*)realloc(il, (size_t)cap * 64);
        for (uint32_t i = 0; i < g_gi.nworld; ++i)
            memcpy(wl + 16 * (size_t)i, g_gi.world + 32 * (size_t)i, 64), memcpy(il + 16 * (size_t)i, g_gi.world + 32 * (size_t)i + 16, 64);
        z.world = wl, z.world_inv = il, z.nworld = g_gi.nworld, z.item = g_gi.items.v, z.nitem = g_gi.items.n;
        rt_log("[recomp] scene: bounce light: zone %08x: %u instances, %u submeshes\n", key, g_gi.nworld, g_gi.items.n);
        gfx_gi_zone(&z);
        g_gi.given = key;
        ++g_gi.zones;
    }
    if (g_gi.frame != g_frame_no)
    {
        g_gi.frame = g_frame_no;
        gi_lamps();
    }
    if (env)
    {
        memcpy(g_gi.sun_dir, env->sun_dir, 16), memcpy(g_gi.sun_col, env->sun_col, 16);
        memcpy(g_gi.sun2_dir, env->sun2_dir, 16), memcpy(g_gi.sun2_col, env->sun2_col, 16);
        memcpy(g_gi.ambient, env->ambient, 16);
        g_gi.have_env = 1;
    }
    if (!g_gi.have_env)
    {
        gfx_gi_frame(NULL);
        return;
    }
    GfxGiFrame f;
    memset(&f, 0, sizeof f);
    f.cam[0] = rdf32(mgr + MGR_CAM), f.cam[1] = rdf32(mgr + MGR_CAM + 4), f.cam[2] = rdf32(mgr + MGR_CAM + 8);
    memcpy(f.sun_dir, g_gi.sun_dir, 16), memcpy(f.sun_col, g_gi.sun_col, 16);
    memcpy(f.sun2_dir, g_gi.sun2_dir, 16), memcpy(f.sun2_col, g_gi.sun2_col, 16), memcpy(f.ambient, g_gi.ambient, 16);
    f.light = g_gi.lamps, f.nlight = g_gi.nlamps;
    f.strength = want;
    f.sky = gfx_fx_get("scene_gi_sky");
    f.bounce = gfx_fx_get("scene_gi_bounce");
    f.debug = gfx_fx_get("scene_gi_debug");
    float q = gfx_fx_get("scene_gi_q");
    f.quality = q >= 2.5f ? 3 : q >= 1.5f ? 2 : 1;
    gfx_gi_frame(&f);
}
