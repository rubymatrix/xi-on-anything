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
        gi_distance, rt, contact_steps, scene, scene_shadows, scene_shadow_size, scene_shadow_far, scene_shadow_step, scene_shadow_debug,
        scene_shadow_fit, scene_shadow_fit_size, scene_shadow_char_far, scene_shadow_fit_reach,
        post_bloom, bloom_threshold, bloom_knee, bloom_emissive_fx, bloom_emissive_lights, bloom_radius,
        post_dof, dof_focus, dof_range, dof_ramp, dof_near, water_lift, scene_shadow_envmap, post_shafts, shafts_density, shafts_q, shafts_g, shafts_dist, scene_sun_test, scene_sun_test_turn, shafts_debug, shafts_temporal, scene_spec, scene_spec_lamps, scene_spec_wet, scene_spec_aa, scene_hdr, tonemap, exposure, tone_white, tone_knee,
        scene_gi, scene_gi_q, scene_gi_sky, scene_gi_bounce, scene_gi_debug,
        scene_ao, scene_ao_q, scene_ao_radius, scene_ao_chars, scene_ao_debug;
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
    GFX_PASS_WORLD,    /* the scene renderer's zone geometry (gfx_world_draw), in a pass of its own while timed */
    GFX_PASS_CHARS,    /* the scene renderer's characters */
    GFX_PASS_SHADOW_ZONE,  /* the scene renderer's shadows: the zone's layer, a cascade drawn again (gfx_shadow_begin) */
    GFX_PASS_SHADOW_CHARS, /* the characters' layer, every frame */
    GFX_PASS_POST_BLOOM,   /* post_bloom: the glow's draws again, the mip chain down and up, onto the scene */
    GFX_PASS_POST_DOF,     /* post_dof: circles of confusion at half size, the gather, onto the scene */
    GFX_PASS_SCENE_AO,     /* scene_ao: the scene renderer's occlusion, its depth at half size and its blend (gfx_ao_metal.inc) */
    GFX_PASS_SCENE_AO_TRACE, /* its GTAO or rays */
    GFX_PASS_POST_SHAFTS,  /* post_shafts: the march, its resolve over frames, onto the scene */
    GFX_PASS_HDR,          /* scene_hdr: the target made float (once), the tonemap */
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
