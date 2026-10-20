/* The scene effects' settings (gfx_fx.h). */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cachedir.h"
#include "gfx.h"
#include "gfx_fx.h"

#ifdef __APPLE__
#define FX_MTIME(st) (st).st_mtimespec
#else
#define FX_MTIME(st) (st).st_mtim
#endif

GfxFxSettings g_fxs;

/* every setting: its key in the file, FFXI_FX_<KEY> in the environment, and its default */
static const struct
{
    const char* key;
    size_t at;
    float def;
} FX_SETTINGS[] = {
    { "fx", offsetof(GfxFxSettings, fx), 1.0f },
    { "ao", offsetof(GfxFxSettings, ao), 0.8f },
    { "radius", offsetof(GfxFxSettings, radius), 1.0f },
    { "grade", offsetof(GfxFxSettings, grade), 1.0f },
    { "sat", offsetof(GfxFxSettings, sat), 1.12f },
    { "contrast", offsetof(GfxFxSettings, contrast), 0.2f },
    { "sharpen", offsetof(GfxFxSettings, sharpen), 0.3f },
    { "filter", offsetof(GfxFxSettings, filter), 1.0f },
    { "aniso", offsetof(GfxFxSettings, aniso), 16.0f },
    { "fog", offsetof(GfxFxSettings, fog), 0.004f },
    { "fog_falloff", offsetof(GfxFxSettings, fog_falloff), 0.08f },
    { "fog_height", offsetof(GfxFxSettings, fog_height), 2.0f },
    { "fog_max", offsetof(GfxFxSettings, fog_max), 0.5f },
    { "fog_sun", offsetof(GfxFxSettings, fog_sun), 0.5f },
    { "fog_g", offsetof(GfxFxSettings, fog_g), 0.6f },
    { "bloom", offsetof(GfxFxSettings, bloom), 0.3f },
    { "threshold", offsetof(GfxFxSettings, threshold), 0.75f },
    { "rays", offsetof(GfxFxSettings, rays), 0.6f },
    { "rays_decay", offsetof(GfxFxSettings, rays_decay), 0.965f },
    { "rays_length", offsetof(GfxFxSettings, rays_length), 0.85f },
    { "light", offsetof(GfxFxSettings, light), 2.0f },
    { "shadow", offsetof(GfxFxSettings, shadow), 0.3f },
    { "shadow_length", offsetof(GfxFxSettings, shadow_length), 0.6f },
    { "sun", offsetof(GfxFxSettings, sun), 0.45f },
    { "sun_distance", offsetof(GfxFxSettings, sun_distance), 100.0f },
    { "sun_soft", offsetof(GfxFxSettings, sun_soft), 0.0f },
    { "sun_face", offsetof(GfxFxSettings, sun_face), 0.0f },
    /* casters nearer the surface than this ignored: a character's clothes are shells a few
     * centimetres over its body (collar, sleeves, hood), and at 0 they shaded it in blocks, cut along
     * its polygons where the face turned from the sun. The ground's shadow of the feet stays */
    { "sun_min", offsetof(GfxFxSettings, sun_min), 0.15f },
    { "sun_direct", offsetof(GfxFxSettings, sun_direct), 0.5f },
    /* who casts: 0 everything, 1 characters only (the zone's baked lighting has its shadows), 2 the
     * zone only (characters keep the game's own blob shadows) */
    { "sun_casters", offsetof(GfxFxSettings, sun_casters), 0.0f },
    /* long shadows kept at dawn and dusk, fading only in the sun's last three degrees (0: from fifteen) */
    { "sun_dusk", offsetof(GfxFxSettings, sun_dusk), 1.0f },
    /* not an effect: the game's own character shadows (d3d8.c game_shadow_hidden, and its Config >
     * Shadows held at Off: host/modern.c game_shadows_follow): 0 off while the sun's are on, 1
     * always, 2 never */
    { "gameshadows", offsetof(GfxFxSettings, gameshadows), 0.0f },
    /* how much of the sun's shadows stay indoors - a Mog House (gfx_set_moghouse), a zone with no sky
     * (GfxScene.indoors) - 0 none to 1 all; the key keeps its old name for the fx.txt files that have it */
    { "moghouse", offsetof(GfxFxSettings, moghouse), 0.0f },
    /* how far round the camera the game is asked to draw the zone out of view, once after a zone-in and
     * again as the camera moves on, so what stands behind it casts (gfx_sun_prime): the far map reaches
     * some 170 units out and takes casters 96 beyond its sides; 0 never */
    { "sun_prime", offsetof(GfxFxSettings, sun_prime), 300.0f },
    /* the near map's reach past the player */
    { "sun_near", offsetof(GfxFxSettings, sun_near), 10.0f },
    /* the near map's texels across, 512 to 8192 (64 MB at 4096, 256 MB at 8192); 0 and 1 are the
     * old switch, 4096 and 8192 (sun_near_size) */
    { "sun_detail", offsetof(GfxFxSettings, sun_detail), 4096.0f },
    /* the occlusion's taps a frame: 0 low (6), 1 medium (10), 2 high (16); the temporal pass gathers
     * them over frames */
    { "ao_quality", offsetof(GfxFxSettings, ao_quality), 1.0f },
    { "temporal", offsetof(GfxFxSettings, temporal), 0.85f },
    { "debug", offsetof(GfxFxSettings, debug), 0.0f },
    /* not effects: the host's draw distances (host64 --draw-distance), live while tuning; 0 leaves them */
    { "draw", offsetof(GfxFxSettings, draw), 0.0f },
    { "draw_entities", offsetof(GfxFxSettings, draw_entities), 0.0f },
    /* not an effect either: the frame-rate overlay, shown (1) or hidden (0) */
    { "fps", offsetof(GfxFxSettings, fps), 1.0f },
    /* the game's water (gfx_msl.c WATER_MSL): on (1) or as the game drew it (0), how far the ripples bend
     * what is behind it (a fraction of the height), the depth in world units where the game's color takes
     * over, how deep its edge fades in, how much foam and down to what depth, the ripples' tilt and how
     * many per unit, the sky's reflection, the sun's highlight */
    { "water", offsetof(GfxFxSettings, water), 1.0f },
    { "water_refract", offsetof(GfxFxSettings, water_refract), 0.015f },
    { "water_clarity", offsetof(GfxFxSettings, water_clarity), 3.0f },
    { "water_soft", offsetof(GfxFxSettings, water_soft), 0.15f },
    { "water_foam", offsetof(GfxFxSettings, water_foam), 0.6f },
    { "water_foam_width", offsetof(GfxFxSettings, water_foam_width), 0.8f },
    { "water_ripple", offsetof(GfxFxSettings, water_ripple), 0.25f },
    { "water_scale", offsetof(GfxFxSettings, water_scale), 0.6f },
    { "water_reflect", offsetof(GfxFxSettings, water_reflect), 0.6f },
    { "water_spec", offsetof(GfxFxSettings, water_spec), 2.0f },
    /* not an effect: the host's level of detail (host64 --lod), 1 near models at every distance, 2 as the
     * game picks, 0 as the command line says */
    { "lod", offsetof(GfxFxSettings, lod), 0.0f },
    /* not an effect: anti-aliasing of the finished scene, 0 none, 1 FXAA (scene_aa); with or without the effects */
    { "aa", offsetof(GfxFxSettings, aa), 0.0f },
    /* the sun's light thrown on by what it lights (bounce light): its strength (0 none), how far a lit
     * surface throws it, and how far from the camera it is gathered (the map it is gathered from); off
     * unless asked for (Config > Modern's Bounce Light) */
    { "gi", offsetof(GfxFxSettings, gi), 0.0f },
    { "gi_radius", offsetof(GfxFxSettings, gi_radius), 6.0f },
    { "gi_distance", offsetof(GfxFxSettings, gi_distance), 48.0f },
    /* ray tracing: the bounce light and the occlusion traced through the frame's casters (rt_capture), where
     * the GPU can; off unless asked for */
    { "rt", offsetof(GfxFxSettings, rt), 0.0f },
    /* the contact shadows' steps along the sun a frame, 1 to 16, offset each frame as the occlusion's taps
     * are and gathered by the same temporal pass; 0 follows ao_quality: 8 low, 12 medium, 16 high (16
     * every frame was nearly a quarter of the effects' cost on a ROG Ally) */
    { "contact_steps", offsetof(GfxFxSettings, contact_steps), 0.0f },
    /* not an effect: the scene renderer (host/scene.c) draws the zone from the game's own data in place of
     * the game's draws, 0 off (the game's draws, as before), 1 lit per vertex as the game lights it, 2 lit
     * per pixel; with or without the effects */
    { "scene", offsetof(GfxFxSettings, scene), 0.0f },
    /* the scene renderer's sun and moon shadows (host/scene.c, gfx_shadow_begin): 0 off, else how much of the
     * sun's or moon's light a shadow takes (1 all of it); with them on the game's own character shadows stand aside.
     * Their maps' size in texels, how far from the camera they reach in world units (four cascades out to it), and the
     * light's step in degrees: the sun's direction is held until it has turned this far, then all four are drawn again */
    { "scene_shadows", offsetof(GfxFxSettings, scene_shadows), 0.0f },
    { "scene_shadow_size", offsetof(GfxFxSettings, scene_shadow_size), 2048.0f },
    { "scene_shadow_far", offsetof(GfxFxSettings, scene_shadow_far), 160.0f },
    { "scene_shadow_step", offsetof(GfxFxSettings, scene_shadow_step), 0.25f },
    /* 1: the zone shows the shadow alone, tinted by its cascade (red, green, blue, yellow from the camera out) */
    { "scene_shadow_debug", offsetof(GfxFxSettings, scene_shadow_debug), 0.0f },
    /* the characters' first shadow layer fitted round the characters near the camera (1), or cascade 0's square (0) */
    { "scene_shadow_fit", offsetof(GfxFxSettings, scene_shadow_fit), 1.0f },
    /* that layer's texels across (256 to 4096) */
    { "scene_shadow_fit_size", offsetof(GfxFxSettings, scene_shadow_fit_size), 1024.0f },
    /* the characters the fitted layer is fitted round: those within this of the camera (units; the camera is some six
     * behind the player) */
    { "scene_shadow_fit_reach", offsetof(GfxFxSettings, scene_shadow_fit_reach), 8.0f },
    /* environment-mapped character meshes (shiny armour), which the game draws, cast in the scene renderer's shadows */
    { "scene_shadow_envmap", offsetof(GfxFxSettings, scene_shadow_envmap), 1.0f },
    /* how far from the camera characters cast (units) */
    { "scene_shadow_char_far", offsetof(GfxFxSettings, scene_shadow_char_far), 1000.0f },
    /* the post bloom (gfx_metal.m post_bloom), with or without the effects and the scene renderer: its strength, 0 off
     * (1 is the tuned look); what glows is the world's additive draws (lamps, spell effects; not the sky's own layers) and
     * where the sky is brighter than the threshold (with a soft knee: the sun); how far it spreads */
    { "post_bloom", offsetof(GfxFxSettings, post_bloom), 0.0f },
    { "bloom_threshold", offsetof(GfxFxSettings, bloom_threshold), 0.95f },
    { "bloom_knee", offsetof(GfxFxSettings, bloom_knee), 0.05f },
    /* emissive weights, past 1 in the bloom's float chain: the world's additive draws (spells, abilities, weather
     * particles, lamp and torch glows - one effect system, not told apart), and the sun, moon and bright sky */
    { "bloom_emissive_fx", offsetof(GfxFxSettings, bloom_emissive_fx), 1.2f },
    { "bloom_emissive_lights", offsetof(GfxFxSettings, bloom_emissive_lights), 1.5f },
    { "bloom_radius", offsetof(GfxFxSettings, bloom_radius), 1.0f },
    /* the depth of field (post_dof): its strength, 0 off (1: the backdrop's blur up to some 8 pixels of a 2048 scene);
     * focus at the player (eased), or dof_focus units out when above 0; sharp to dof_range units behind the focus,
     * fully blurred dof_ramp beyond; dof_near, 0 to 1, blurs what stands well before the focus */
    { "post_dof", offsetof(GfxFxSettings, post_dof), 0.0f },
    { "dof_focus", offsetof(GfxFxSettings, dof_focus), 0.0f },
    { "dof_range", offsetof(GfxFxSettings, dof_range), 50.0f },
    { "dof_ramp", offsetof(GfxFxSettings, dof_ramp), 150.0f },
    { "dof_near", offsetof(GfxFxSettings, dof_near), 0.0f },
    /* the sun shafts (gfx_metal.m post_shafts; with the scene renderer's shadows): strength, 0 off (1 tuned); the haze's
     * density against the game's fog (1: as its fog end says); 2 a third of the scene's size and 16 steps, 1 a fifth and 10; the
     * scattering's forward lobe (0.6); how far the rays go (units) */
    { "post_shafts", offsetof(GfxFxSettings, post_shafts), 0.0f },
    { "shafts_density", offsetof(GfxFxSettings, shafts_density), 1.0f },
    { "shafts_q", offsetof(GfxFxSettings, shafts_q), 2.0f },
    { "shafts_g", offsetof(GfxFxSettings, shafts_g), 0.6f },
    { "shafts_dist", offsetof(GfxFxSettings, shafts_dist), 120.0f },
    /* specular highlights on the scene renderer's zone (scene 2) and characters, with its shadows (gfx_metal.m
     * SPEC_MSL): strength, 0 off (1 tuned); the lamps' share; wetness 0 to 1 (rain); the anti-aliasing's width */
    { "scene_spec", offsetof(GfxFxSettings, scene_spec), 0.0f },
    { "scene_spec_lamps", offsetof(GfxFxSettings, scene_spec_lamps), 1.0f },
    { "scene_spec_wet", offsetof(GfxFxSettings, scene_spec_wet), 0.0f },
    { "scene_spec_aa", offsetof(GfxFxSettings, scene_spec_aa), 1.0f },
    /* the lamps (gfx_metal.m lamps_bin; scene 2): every lit lamp of the light pool per pixel on the zone and characters,
     * faded at its range; strength (1: the game's own), 0 off; how far from the camera they light; a sprite glow at each
     * light's place (0 none: the game's light sits below its lantern, where it lights the ground, so the sprite floats
     * under it; the lantern's own flame glows through the bloom) and its size (units) */
    { "scene_lamps", offsetof(GfxFxSettings, scene_lamps), 0.0f },
    { "scene_lamp_far", offsetof(GfxFxSettings, scene_lamp_far), 120.0f },
    { "scene_lamp_glow", offsetof(GfxFxSettings, scene_lamp_glow), 0.0f },
    { "scene_lamp_glow_size", offsetof(GfxFxSettings, scene_lamp_glow_size), 0.6f },
    /* the HDR scene (gfx_metal.m hdr_update): the game's world target float, one tonemap at the scene's end; 0 off;
     * tonemap 0 neutral (today's image), 1 a highlight shoulder (today's image below tone_knee, past it rolled off toward
     * tone_white), 2 ACES fit; the exposure; the white point; the knee */
    { "scene_hdr", offsetof(GfxFxSettings, scene_hdr), 0.0f },
    { "tonemap", offsetof(GfxFxSettings, tonemap), 0.0f },
    { "exposure", offsetof(GfxFxSettings, exposure), 1.0f },
    { "tone_white", offsetof(GfxFxSettings, tone_white), 4.0f },
    { "tone_knee", offsetof(GfxFxSettings, tone_knee), 0.8f },
    /* how much of last frame's shafts each frame keeps (0 none) */
    { "shafts_temporal", offsetof(GfxFxSettings, shafts_temporal), 0.9f },
    /* the shafts alone in place of the scene, times this (0 off) */
    { "shafts_debug", offsetof(GfxFxSettings, shafts_debug), 0.0f },
    /* testing: the scene renderer's sun held this many degrees above the horizon ahead of the camera (0 off), turned
     * scene_sun_test_turn degrees round from it (host/scene.c) */
    { "scene_sun_test", offsetof(GfxFxSettings, scene_sun_test), 0.0f },
    { "scene_sun_test_turn", offsetof(GfxFxSettings, scene_sun_test_turn), 0.0f },
    /* the game's water models raised this far (units) over the zone surfaces laid at their level (d3d8.c water_lift) */
    { "water_lift", offsetof(GfxFxSettings, water_lift), 0.15f },
    /* the scene renderer's bounce light (gfx_gi_metal.inc): 0 off, else how much of it the zone and characters take;
     * needs scene 1 or 2 and ray queries */
    { "scene_gi", offsetof(GfxFxSettings, scene_gi), 0.0f },
    /* its quality: 1 low (24x8x24 probes 4 apart, 32 rays, 1 in 16 a frame), 2 high (32x12x32 3 apart, 64 rays, 1 in 12),
     * both looked up per vertex; 3 high looked up per pixel (with scene 2) */
    { "scene_gi_q", offsetof(GfxFxSettings, scene_gi_q), 2.0f },
    /* how far the share of the sky the probes see darkens the game's ambient (0 not at all, 1 fully) */
    { "scene_gi_sky", offsetof(GfxFxSettings, scene_gi_sky), 0.0f },
    /* the earlier bounces' weight in each ray's hit (0: one bounce) */
    { "scene_gi_bounce", offsetof(GfxFxSettings, scene_gi_bounce), 1.0f },
    /* 1: the zone shows the bounce light alone (on its vertex colour), 2 the ambient's factor, 3 the bounce light x 8 */
    { "scene_gi_debug", offsetof(GfxFxSettings, scene_gi_debug), 0.0f },
    /* the scene renderer's ambient occlusion (gfx_ao_metal.inc): 0 off, else the power the occlusion is
     * raised to (1 as made, 2 darker) before it scales the ambient;
     * needs scene 1 or 2 */
    { "scene_ao", offsetof(GfxFxSettings, scene_ao), 0.0f },
    /* its quality: 1 GTAO low (2 slices of 4 steps), 2 GTAO high (3 of 6), both from the depth at half the window's size;
     * 3 rays low (1 a pixel), 4 rays high (2), against the zone's structure (built while scene_gi or these are on) */
    { "scene_ao_q", offsetof(GfxFxSettings, scene_ao_q), 1.0f },
    /* its reach in world units */
    { "scene_ao_radius", offsetof(GfxFxSettings, scene_ao_radius), 1.0f },
    /* how much of it the characters take (times scene_ao) */
    { "scene_ao_chars", offsetof(GfxFxSettings, scene_ao_chars), 1.0f },
    /* 1: the zone and characters show the occlusion alone */
    { "scene_ao_debug", offsetof(GfxFxSettings, scene_ao_debug), 0.0f },
};

/* the settings no menu sets any more: the environment and the defaults give them, the file not (a line
 * the old menus wrote would hold them where nothing can change them back) */
static const char* const FX_FIXED[] = { "fx", "gameshadows", "filter", "lod", "rt" };

static int fx_fixed(const char* key)
{
    for (size_t i = 0; i < sizeof FX_FIXED / sizeof FX_FIXED[0]; ++i)
        if (!strcmp(FX_FIXED[i], key))
            return 1;
    return 0;
}

static float* fx_setting(const char* key)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
        if (!strcmp(FX_SETTINGS[i].key, key))
            return (float*)((char*)&g_fxs + FX_SETTINGS[i].at);
    return NULL;
}

void gfx_fx_set(const char* key, float v)
{
    float* p = fx_setting(key);
    if (p)
        *p = v;
}

float gfx_fx_get(const char* key)
{
    float* p = fx_setting(key);
    return p ? *p : 0.0f;
}

/* the sun's shadows are on: the effects, a sun strength, and characters among the casters */
int gfx_sun_shadows_on(void)
{
    if (g_fxs.scene >= 0.5f && g_fxs.scene_shadows > 0.0f && gfx_world_supported())
        return 1; /* the scene renderer's own (host/scene.c), characters casting */
    return g_fxs.fx != 0.0f && g_fxs.sun > 0.0f && g_fxs.sun_casters != 2.0f;
}

static char g_fx_file[1024];
static struct timespec g_fx_mtime;
#define FX_NSETTINGS (sizeof FX_SETTINGS / sizeof FX_SETTINGS[0])
static float g_fx_start[FX_NSETTINGS];      /* each setting as the environment and defaults gave it */
static uint8_t g_fx_from_file[FX_NSETTINGS]; /* set by the file's last read */

void fx_reload(void)
{
    static uint64_t last;
    uint64_t now = gfx_now_ns();
    if (!g_fx_file[0] || (last && now - last < 500000000ull))
        return;
    last = now;
    struct stat st;
    if (stat(g_fx_file, &st) || (FX_MTIME(st).tv_sec == g_fx_mtime.tv_sec && FX_MTIME(st).tv_nsec == g_fx_mtime.tv_nsec))
        return;
    g_fx_mtime = FX_MTIME(st);
    FILE* f = fopen(g_fx_file, "r");
    if (!f)
        return;
    char line[256], key[64];
    float v;
    uint8_t seen[FX_NSETTINGS] = { 0 };
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_] = %f", key, &v) == 2)
            for (size_t i = 0; i < FX_NSETTINGS; ++i)
                if (!strcmp(FX_SETTINGS[i].key, key) && !fx_fixed(key))
                    *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v, seen[i] = 1;
    fclose(f);
    /* a line taken out of the file: its key back to what it was at start, not the file's last word */
    for (size_t i = 0; i < FX_NSETTINGS; ++i)
    {
        if (g_fx_from_file[i] && !seen[i])
            *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = g_fx_start[i];
        g_fx_from_file[i] = seen[i];
    }
    fprintf(stderr, "[recomp] gfx: scene effects %s from %s\n", g_fxs.fx != 0.0f ? "on" : "off", g_fx_file);
}

void fx_config(void)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
    {
        char name[64], *c;
        snprintf(name, sizeof name, i ? "FFXI_FX_%s" : "FFXI_FX", FX_SETTINGS[i].key);
        for (c = name; *c; ++c)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32;
        const char* v = getenv(name);
        *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v && *v ? (float)atof(v) : FX_SETTINGS[i].def;
    }
    const char* dbg = getenv("FFXI_FX_DEBUG"); /* also by name */
    if (dbg)
        g_fxs.debug = !strcmp(dbg, "ao") ? 1.0f : !strcmp(dbg, "fog") ? 2.0f : !strcmp(dbg, "bloom") ? 3.0f
            : !strcmp(dbg, "rays") ? 4.0f : !strcmp(dbg, "shadow") ? 5.0f
            : !strcmp(dbg, "gi") ? 6.0f : !strcmp(dbg, "gi_split") ? 7.0f : !strcmp(dbg, "clay") ? 8.0f : (float)atof(dbg);
    for (size_t i = 0; i < FX_NSETTINGS; ++i)
        g_fx_start[i] = *(float*)((char*)&g_fxs + FX_SETTINGS[i].at);
    const char* file = getenv("FFXI_FX_FILE");
    if (file && *file)
        snprintf(g_fx_file, sizeof g_fx_file, "%s", file);
    else
    {
        char dir[900];
        if (cache_dir(dir, sizeof dir))
            snprintf(g_fx_file, sizeof g_fx_file, "%s/fx.txt", dir);
    }
}

/* --- GPU time per pass ------------------------------------------------------------------------------------------- */
static const char* const k_pass_name[GFX_NPASSES] = { "sun maps", "linz/zmip", "ao", "ao rays", "blur", "temporal", "gi",
    "bloom", "rays", "comp", "fxaa", "present", "world", "chars", "shadow zone", "shadow chars", "post bloom", "post dof",
    "scene ao", "scene ao trace", "post shafts", "hdr", "lamps" };
static _Atomic uint64_t g_pass_ns[GFX_NPASSES], g_pass_gpu_ns, g_pass_frames;
static int g_pass_asked;                          /* /fx prof turned it on */
static uint64_t g_pass_since;                     /* the window's start */
static double g_pass_last[GFX_NPASSES], g_pass_last_gpu; /* the last window's ms a frame */
static uint64_t g_pass_last_frames;
static double g_pass_last_secs;

int gfx_pass_timing_on(void) { return gfx_profiling || g_pass_asked; }

void gfx_pass_add(int pass, uint64_t ns)
{
    if (pass >= 0 && pass < GFX_NPASSES)
        atomic_fetch_add(&g_pass_ns[pass], ns);
}

void gfx_pass_gpu(uint64_t ns) { atomic_fetch_add(&g_pass_gpu_ns, ns); }

void gfx_pass_frame(void) { atomic_fetch_add(&g_pass_frames, 1); }

void gfx_pass_timing(int on)
{
    if (on && !g_pass_asked)
        g_pass_since = 0; /* a fresh window */
    g_pass_asked = on;
}

void gfx_pass_tick(void)
{
    if (!gfx_pass_timing_on())
        return;
    uint64_t now = gfx_now_ns();
    if (!g_pass_since)
    {
        g_pass_since = now;
        for (int i = 0; i < GFX_NPASSES; ++i)
            atomic_store(&g_pass_ns[i], 0);
        atomic_store(&g_pass_gpu_ns, 0), atomic_store(&g_pass_frames, 0);
        return;
    }
    if (now - g_pass_since < 2000000000ull)
        return;
    uint64_t frames = atomic_exchange(&g_pass_frames, 0);
    double per = frames ? 1e-6 / (double)frames : 0.0;
    for (int i = 0; i < GFX_NPASSES; ++i)
        g_pass_last[i] = (double)atomic_exchange(&g_pass_ns[i], 0) * per;
    g_pass_last_gpu = (double)atomic_exchange(&g_pass_gpu_ns, 0) * per;
    g_pass_last_frames = frames, g_pass_last_secs = (double)(now - g_pass_since) * 1e-9;
    g_pass_since = now;
    if (gfx_profiling && frames)
    {
        char lines[4][96];
        int n = gfx_pass_report(lines, 4);
        for (int i = 0; i < n; ++i)
            fprintf(stderr, "[gfx]   %s\n", lines[i]);
    }
}

int gfx_pass_report(char (*out)[96], int max)
{
    if (!g_pass_last_frames || max < 1)
        return 0;
    double fx = 0.0;
    for (int i = GFX_PASS_LINZ; i <= GFX_PASS_FXAA; ++i)
        fx += g_pass_last[i];
    int n = 0;
    snprintf(out[n++], 96, "GPU ms a frame over %.0f s (%llu frames): frame %.2f, sun maps %.2f, effects %.2f", g_pass_last_secs,
        (unsigned long long)g_pass_last_frames, g_pass_last_gpu, g_pass_last[GFX_PASS_SUN], fx);
    /* the passes that ran, four to a line */
    int at = 0;
    for (int i = GFX_PASS_LINZ; i < GFX_NPASSES && n < max; ++i)
    {
        if (g_pass_last[i] <= 0.0)
            continue;
        if (!at)
            out[n][0] = 0;
        at += snprintf(out[n] + at, at < 96 ? 96 - at : 0, "%s%s %.2f", at ? ", " : "  ", k_pass_name[i], g_pass_last[i]);
        if (at > 60)
            n++, at = 0;
    }
    if (at && n < max)
        n++;
    return n;
}
