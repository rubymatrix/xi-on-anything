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
        gi_distance, rt, occlusion;
} GfxFxSettings;

extern GfxFxSettings g_fxs;

/* Every setting from its default and the environment, and where the settings file is (gfx_init). */
void fx_config(void);
/* The settings file again when it changed since the last look (from Present, twice a second). */
void fx_reload(void);
