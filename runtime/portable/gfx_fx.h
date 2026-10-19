/* The scene effects' settings, shared by the back ends that have the effects (gfx_metal.m,
 * gfx_vulkan.c): FFXI_FX_* in the environment, then the settings file (FFXI_FX_FILE, default fx.txt
 * in the cache folder) while the game runs. fx = 0 is the game as it was. The keys and defaults are
 * in gfx_fx.c; gfx_fx_set and gfx_fx_get (gfx.h) read and write them by key. */
#pragma once

typedef struct GfxFxSettings
{
    float fx, ao, radius, grade, sat, contrast, sharpen, filter, aniso, fog, fog_falloff, fog_height, fog_max, fog_sun,
        fog_g, bloom, threshold, rays, rays_decay, rays_length, light, shadow, shadow_length, sun, sun_distance, sun_soft, sun_face, sun_min, sun_direct, sun_casters, sun_near, temporal, debug, draw,
        draw_entities, fps, water, water_refract, water_clarity, water_soft, water_foam, water_foam_width, water_ripple, water_scale,
        water_reflect, water_spec, lod, aa, ao_quality, sun_detail, sun_dusk, gameshadows, moghouse, sun_prime, gi, gi_radius,
        gi_distance, rt, contact_steps;
} GfxFxSettings;

extern GfxFxSettings g_fxs;

/* Every setting from its default and the environment, and where the settings file is (gfx_init). */
void fx_config(void);
/* The settings file again when it changed since the last look (from Present, twice a second). */
void fx_reload(void);

/* GPU time per pass (gfx_pass_timing, gfx.h): the back end marks each pass with one of these and adds
 * what its timestamps say once the frame is done, from any thread. */
enum
{
    GFX_PASS_SUN,      /* the sun's maps: its shadow cascades and the bounce light's (the cache's copies not counted) */
    GFX_PASS_LINZ,     /* the occlusion's linear depth and its three mips (fx_linz, fx_zmip) */
    GFX_PASS_AO,       /* fx_ao: the occlusion, contact shadows and the sun maps' lookup */
    GFX_PASS_AO_RT,    /* rt_ao: the occlusion from rays (rt = 1) */
    GFX_PASS_BLUR,     /* fx_blur, both directions */
    GFX_PASS_TEMPORAL, /* fx_temporal: the occlusion over frames */
    GFX_PASS_GI,       /* the bounce light: gathered or traced, its blurs and its temporal pass */
    GFX_PASS_BLOOM,    /* fx_bright, the two levels' downsample and Gaussian passes */
    GFX_PASS_RAYS,     /* the sun shafts: fx_raymask, fx_rays and their blur */
    GFX_PASS_COMP,     /* fx_comp: everything onto the scene, fog and grading (and rt_clay's debug view) */
    GFX_PASS_FXAA,     /* scene_aa */
    GFX_PASS_PRESENT,  /* the back buffer to the window, sharpened (sharpen > 0), with the frame-rate overlay */
    GFX_NPASSES
};
/* timing is on: FFXI_PROFILE=1, or /fx prof */
int gfx_pass_timing_on(void);
/* one pass's GPU time, and the frame's GPU time (the command buffers', start to end, summed) */
void gfx_pass_add(int pass, uint64_t ns);
void gfx_pass_gpu(uint64_t ns);
/* a frame's timestamps were all added */
void gfx_pass_frame(void);
/* from Present: every two seconds the averages move to what gfx_pass_report shows, and to the log when
 * profiling */
void gfx_pass_tick(void);
