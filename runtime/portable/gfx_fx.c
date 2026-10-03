/* The scene effects' settings (gfx_fx.h). */
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
    { "fx", offsetof(GfxFxSettings, fx), 0.0f },
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
    { "light", offsetof(GfxFxSettings, light), 0.0f },
    { "shadow", offsetof(GfxFxSettings, shadow), 0.3f },
    { "shadow_length", offsetof(GfxFxSettings, shadow_length), 0.6f },
    { "sun", offsetof(GfxFxSettings, sun), 0.45f },
    { "sun_distance", offsetof(GfxFxSettings, sun_distance), 100.0f },
    { "sun_soft", offsetof(GfxFxSettings, sun_soft), 0.0f },
    { "sun_face", offsetof(GfxFxSettings, sun_face), 0.0f },
    /* 0: no near-surface casters ignored - with positions rebuilt where the fixup drew them
     * (view_pos), surfaces no longer shade themselves without it */
    { "sun_min", offsetof(GfxFxSettings, sun_min), 0.0f },
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
    /* how much of the sun's shadows stay in a Mog House (gfx_set_moghouse), 0 none to 1 all */
    { "moghouse", offsetof(GfxFxSettings, moghouse), 0.0f },
    /* the near map's reach past the player */
    { "sun_near", offsetof(GfxFxSettings, sun_near), 10.0f },
    /* the near map's texels across, 512 to 8192 (64 MB at 4096, 256 MB at 8192); 0 and 1 are the
     * old switch, 4096 and 8192 (sun_near_size) */
    { "sun_detail", offsetof(GfxFxSettings, sun_detail), 4096.0f },
    /* the occlusion's taps a frame: 0 low (6), 1 medium (10), 2 high (16); the temporal pass gathers
     * them over frames */
    { "ao_quality", offsetof(GfxFxSettings, ao_quality), 0.0f },
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
};

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
                if (!strcmp(FX_SETTINGS[i].key, key))
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
            : !strcmp(dbg, "rays") ? 4.0f : !strcmp(dbg, "shadow") ? 5.0f : (float)atof(dbg);
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
