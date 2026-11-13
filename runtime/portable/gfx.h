/* The graphics back end under the D3D8 front end (d3d8.c): Metal on macOS (gfx_metal.m), nothing
 * elsewhere yet (gfx_null.c).
 *
 * d3d8.c keeps every D3D8 semantic - objects, state, state blocks, what a lock returns - and hands
 * this layer resolved work: textures to create and fill, the render targets, clears, and draw
 * packets. A draw packet is the fixed-function (or shader) state reduced to a key that selects
 * the generated shaders and pipeline (GfxVsKey, GfxFsKey, GfxPipeKey), the values those shaders
 * read (GfxU), and the vertex and index bytes the draw touches.
 *
 * Formats are D3DFORMAT values; the back end maps them. Matrices are D3D's: row vectors, row-major
 * storage, v * M. */
#pragma once

#include <stdint.h>

typedef struct GfxTex GfxTex;
typedef struct GfxBuf GfxBuf;

enum
{
    GFX_TEX_2D,
    GFX_TEX_CUBE,
};

enum
{
    GFX_USE_SAMPLE,
    GFX_USE_RT,    /* a render target that can also be sampled */
    GFX_USE_DEPTH, /* a depth-stencil surface */
};

/* D3DPRIMITIVETYPE */
enum
{
    GFX_POINTLIST = 1,
    GFX_LINELIST,
    GFX_LINESTRIP,
    GFX_TRIANGLELIST,
    GFX_TRIANGLESTRIP,
    GFX_TRIANGLEFAN,
};

/* vertex element types: D3DVSDT */
enum
{
    GFX_FLOAT1,
    GFX_FLOAT2,
    GFX_FLOAT3,
    GFX_FLOAT4,
    GFX_D3DCOLOR,
    GFX_UBYTE4,
    GFX_SHORT2,
    GFX_SHORT4,
};

/* the fixed-function input registers (D3DVSDE), also the v# of a vertex shader */
enum
{
    GFX_R_POSITION,
    GFX_R_BLENDWEIGHT,
    GFX_R_BLENDINDICES,
    GFX_R_NORMAL,
    GFX_R_PSIZE,
    GFX_R_DIFFUSE,
    GFX_R_SPECULAR,
    GFX_R_TEXCOORD0, /* .. 14 */
    GFX_R_POSITION2 = 15,
    GFX_R_NORMAL2,
    GFX_NREGS,
};

#define GFX_NSTREAMS 4
#define GFX_NLIGHTS 8
#define GFX_NVSC 96
#define GFX_NPSC 8

typedef struct GfxElem
{
    uint8_t used, stream, type, pad;
} GfxElem;

/* What selects a vertex function. Zeroed, then filled: the bytes are hashed and compared. */
typedef struct GfxVsKey
{
    uint32_t prog;     /* 0: fixed function; else a vertex shader (GfxDraw.vs_tokens), this is its hash */
    GfxElem el[GFX_NREGS];
    uint8_t rhw;       /* XYZRHW positions: already transformed */
    uint8_t lighting, normalize, localviewer, specular;
    uint8_t src_diffuse, src_specular, src_ambient, src_emissive; /* D3DMCS: 0 material, 1 color1, 2 color2 */
    uint8_t nlights, light_type[GFX_NLIGHTS];                    /* D3DLIGHTTYPE of each enabled light */
    uint8_t fog_vertex; /* D3DFOGMODE computed per vertex (FOGVERTEXMODE with no table fog), 0 none */
    uint8_t range_fog;
    uint8_t ntex;       /* texture coordinate outputs */
    uint8_t tci[8];     /* D3DTSS_TEXCOORDINDEX: index | generation mode << 4 */
    uint8_t ttf[8];     /* D3DTSS_TEXTURETRANSFORMFLAGS: count | 0x80 projected */
    uint8_t flat;
    uint8_t pixel;      /* the lighting per pixel rather than per vertex (the back end sets it: the
                         * scene effects' light setting); the vertex function passes the normal on */
    uint8_t shadow;     /* drawn again from the sun (the back end's shadow map): the position the
                         * function makes goes through the matrix in buffer 5; 2: captured for ray
                         * tracing, that point streamed out in view space (the back ends' rt_capture) */
    uint8_t water;      /* the back end's water (GfxFsKey.water): the view-space position is passed on */
} GfxVsKey;

typedef struct GfxStage
{
    uint8_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, result;
    uint8_t tex;       /* 0 none bound, 1 2D, 2 cube */
    uint8_t projected; /* divide the coordinates by their last component */
    uint8_t ncoord;    /* components the vertex function writes for this stage: 2, 3 or 4 */
} GfxStage;

typedef struct GfxFsKey
{
    uint32_t prog; /* 0: the texture stages; else a pixel shader (GfxDraw.ps_tokens), this is its hash */
    GfxStage st[8];
    uint8_t nstages;
    uint8_t alpha_func; /* D3DCMPFUNC, 0 or 8 when the test is off */
    uint8_t fog;        /* D3DFOGMODE: pixel (table) fog; 4 = the vertex function's fog factor */
    uint8_t specular_add;
    uint8_t flat;
    uint8_t water; /* the back end's water (GfxDraw.water, the scene effects' water setting): 1 drawn
                    * over the scene behind it (an alpha-blended draw), 2 only its edges softened */
    uint8_t pad[2];
} GfxFsKey;

/* The render pipeline beyond the functions: blending and the color write mask. */
typedef struct GfxPipeKey
{
    uint8_t blend, src, dst, op; /* D3DBLEND, D3DBLENDOP */
    uint8_t write_mask;          /* D3DCOLORWRITEENABLE */
    uint8_t pad[3];
} GfxPipeKey;

typedef struct GfxDepthKey
{
    uint8_t zenable, zwrite, zfunc, stencil;
    uint8_t sfail, szfail, spass, sfunc;
    uint8_t sread, swrite, pad[2];
} GfxDepthKey;

typedef struct GfxLight
{
    float diffuse[4], specular[4], ambient[4];
    float pos[4];  /* camera space; w = range */
    float dir[4];  /* camera space, toward the light for directional lights; w = falloff */
    float att[4];  /* attenuation 0, 1, 2 */
    float spot[4]; /* cos(theta / 2), cos(phi / 2) */
} GfxLight;

/* The values the generated functions read: one buffer for both stages. Layout is mirrored in
 * gfx_msl.c's MSL struct; keep them together. */
typedef struct GfxU
{
    float wvp[16], wv[16], wvit[16];
    float texm[8][16];
    float mat_d[4], mat_a[4], mat_s[4], mat_e[4];
    float params[4];  /* material power, alpha ref (0..255), fog start, fog end */
    float ambient[4]; /* D3DRS_AMBIENT */
    float tfactor[4];
    float fogcolor[4];
    float params2[4]; /* fog density */
    float vp[4];      /* viewport x, y, width, height: the XYZRHW mapping */
    int32_t vofs, pad0[3];
    int32_t stride[GFX_NSTREAMS];
    int32_t offset[20]; /* per input register */
    GfxLight light[GFX_NLIGHTS];
    float vsc[GFX_NVSC][4];
    float psc[GFX_NPSC][4];
} GfxU;

typedef struct GfxSampler
{
    uint8_t addr_u, addr_v, addr_w; /* D3DTEXTUREADDRESS */
    uint8_t mag, min, mip;          /* D3DTEXTUREFILTERTYPE */
    uint8_t max_aniso, max_level;
    uint8_t lod_cap, pad[3]; /* 0, or the coarsest mip sampled + 1 (texture packs: glyph sheets whose
                              * small mips blend neighbouring glyphs into each one's edges) */
    uint32_t border;
} GfxSampler;

typedef struct GfxDraw
{
    GfxVsKey vs;
    GfxFsKey fs;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    const uint32_t* vs_tokens; /* the shader behind vs.prog / fs.prog */
    const uint32_t* ps_tokens;
    uint32_t vs_token_count, ps_token_count; /* their lengths in words, END included; 0 unknown (the
                                              * render worker then draws directly) */
    GfxU u;
    GfxTex* tex[8];
    GfxSampler samp[8];
    uint8_t cull;   /* D3DCULL */
    uint8_t caster; /* an opaque draw of the frame's 3D scene: it casts the sun's shadow */
    uint8_t water;  /* a draw of the game's water: its vertices came from a water model (d3d8.c) */
    uint8_t fill;   /* D3DFILLMODE */
    int32_t zbias;  /* D3DRS_ZBIAS */
    uint32_t stencil_ref;
    uint32_t vp[6]; /* D3DVIEWPORT8 (MinZ, MaxZ as float bits) */
    int32_t scissor[4]; /* x, y, width, height in the target's pixels; width 0: none (the addon
                         * overlay's clip rectangles; D3D8 itself has no scissor) */
    /* vertices: stream s holds vertex (first + i) at data[s] + i * stride - copied per draw - or,
     * for a static buffer, at buf_off[s] + i * stride in buf[s] */
    const void* data[GFX_NSTREAMS];
    GfxBuf* buf[GFX_NSTREAMS];
    uint32_t buf_off[GFX_NSTREAMS];
    uint32_t size[GFX_NSTREAMS];
    /* the primitive: indices (16 or 32 bit) or vertices from vertex_start */
    uint32_t prim, count; /* D3D primitive type and primitive count */
    const void* indices;
    uint32_t index_size;
    GfxBuf* ibuf;      /* the indices are also in this static buffer, at ibuf_off (4-byte aligned) */
    uint32_t ibuf_off;
    uint32_t vertex_start;
} GfxDraw;

/* The SDL_WindowFlags a window the back end draws into must be created with (SDL_WINDOW_VULKAN for
 * Vulkan; SDL_WINDOW_HIGH_PIXEL_DENSITY for Metal on iOS, drawn at the screen's pixels; 0 for the
 * others). */
uint64_t gfx_window_flags(void);
/* Brings the device up on an SDL window. 0 on failure (nothing is drawn, nothing fails). */
int gfx_init(void* sdl_window, int vsync);
/* A window of this many pixels: the back buffer's new size after Reset. */
void gfx_resize(uint32_t w, uint32_t h);

/* A vertex or index buffer the game fills once and draws many times: kept on the GPU, uploaded
 * whole when it changes (into a fresh buffer when the GPU may still read the old contents). */
GfxBuf* gfx_buf_create(uint32_t size);
void gfx_buf_destroy(GfxBuf* b);
/* data stays readable, holding what was uploaded or newer, until the next upload or the destroy: a
 * back end may read it again on the CPU (Direct3D 12's sun shadows; d3d8.c passes the buffer's own memory) */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size);

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use);
void gfx_tex_destroy(GfxTex* t);
/* A whole level from D3D's layout (pitch bytes per row, or per block row for DXT). */
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch);
/* A rectangle of a level (4x4 aligned for DXT); src points at its first texel. */
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch);
/* A whole level back into D3D's layout (waits for the GPU). */
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* The same without waiting: the newest copy of the level the GPU has finished (1-3 frames old),
 * and a new copy queued behind this frame's work. Only the first read of a level waits. For
 * read-only locks the game polls every frame (FFXI's occlusion probe). */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
#if defined(FFXI_ANDROID_VULKAN)
/* gfx_tex_read_async with a history per key (one object's lifetime, chosen by the caller): the newest
 * finished copy from an earlier frame at most max_age frames old, or the current pixels when there is
 * none, and a new copy queued. Key 0, max_age 0, a second read of a key in one frame, and a read with
 * no room for its history read the current pixels, as gfx_tex_read. */
void gfx_tex_read_async_keyed(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch, uint64_t key,
                              uint32_t max_age);
/* Counts one occlusion-probe read in the frame statistics: whether its key was known, and whether
 * asynchronous reads were on. */
void gfx_android_probe_read(int known_key, int async_enabled);
#endif
/* A rectangle between textures of one format. */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
    GfxTex* dst, uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy);

/* The render target (a level of a texture, or a cube face) and depth-stencil surface draws go to. */
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth);
/* Clear(Count, pRects, Flags, Color, Z, Stencil) within the viewport (D3DVIEWPORT8). */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t vp[6]);
void gfx_draw(const GfxDraw* d);
/* The camera and light of a frame's 3D scene, as the scene effects need them. */
typedef struct GfxScene
{
    float proj[16];      /* D3DTS_PROJECTION: a perspective one */
    float view[16];      /* D3DTS_VIEW */
    float sun_dir[4];    /* camera space, toward the light; w = 1 when the scene had a directional light */
    float sun_color[4];  /* its diffuse color */
    float ambient[4];    /* D3DRS_AMBIENT */
    float fogcolor[4];
    float fog[4];        /* fog start, end; z = 1 when fog was on */
    uint32_t vp[6];      /* the viewport the 3D draws used (D3DVIEWPORT8) */
    float indoors;       /* 1 while the sun has not been seen to move with the clock in this place (d3d8.c
                          * sun_sky): a zone's fixed light, not the sky's, whose shadows are scaled as a
                          * Mog House's are; easing to 0 once it is seen to */
} GfxScene;

/* The frame's 3D scene is finished in color (a render target's first level, drawn with depth
 * testing), before anything samples it or draws the interface over it. The back end's scene
 * effects - ambient occlusion from the depth it was drawn with, color grading - run on it in place
 * (FFXI_FX=1); back ends without them do nothing. */
void gfx_scene_done(GfxTex* color, const GfxScene* s);
/* One scene-effect setting by its key in the settings file (fx, ao, fog, bloom, rays, ...). */
void gfx_fx_set(const char* key, float v);
/* Its value now (the settings file reloads while the game runs); 0 for a key it does not know. */
float gfx_fx_get(const char* key);
/* The scene effects' last frames (what the shadows were drawn from), one line each, to a file. */
void gfx_trace_dump(const char* path);
/* The local player's place in the world (world units, as the game's entity holds it), for effects
 * that centre on the character: the sun's near shadow map. NULL when there is none (the lobby, a
 * zone change). */
void gfx_set_focus(const float* pos);
/* Whether the player is in a Mog House, where the sun's shadows are scaled by the moghouse setting
 * (0 by default: none - the room's ceiling would shade all of it). */
void gfx_set_moghouse(int in);
/* Whether the sun's shadows were drawn, at least a quarter as dark as by full day, within the last 30
 * frames: the game's own character shadows stand aside only then (d3d8.c game_shadow_hidden). */
int gfx_sun_shadows_shown(void);
int gfx_sun_shadows_on(void); /* the setting: effects, sun > 0, characters cast (gfx_fx.c) */
/* Whether the sun's shadows want the zone round the camera drawn this frame, out of view as well (the
 * map renderer's culling set aside for it: host64's cull_test): the radius in world units, and in
 * center where round, or 0 for the game's own culling. A frame or two after the casters kept from
 * before were let go (a zone-in) and as the camera moves on, so what stands behind it casts. */
float gfx_sun_prime(float* center);

/* The scene renderer (host/scene.c, the scene setting): the zone drawn from the game's own data rather
 * than from its D3D8 draws, into the current targets (the game's world target and its depth), with
 * the game's lighting equation - 2 x texture x vertex colour x (ambient + sun + points), table fog -
 * per vertex (mode 1, as the game's fixed function lights it) or per pixel (mode 2). Positions,
 * normals and directions are in world space; matrices are D3D's (row vectors, row-major). */
enum
{
    GFX_WORLD_BLEND = 1,     /* alpha blended (SRCALPHA, INVSRCALPHA), no depth write */
    GFX_WORLD_TWOSIDED = 2,  /* no culling */
    GFX_WORLD_MIRRORED = 4,  /* placed mirrored: the other faces culled (D3D's CULL_CW) */
    GFX_WORLD_ATEST = 8,     /* alpha test, GREATER the frame's alpha reference */
    GFX_WORLD_CLAMP = 16,    /* texture addressing clamped (else wrapped) */
    GFX_WORLD_NORMALIZE = 32 /* the instance's normals renormalised after the transform (NORMALIZENORMALS) */
};
#define GFX_WORLD_LIGHTS 4
typedef struct GfxWorldLight
{
    float pos[4];     /* world; w = range */
    float diffuse[4];
    float ambient[4];
    float att[4];     /* attenuation 0, 1, 2 */
} GfxWorldLight;
typedef struct GfxWorldInst
{
    float wvp[16];      /* world x view x projection, multiplied as d3d8.c does for the game's draws */
    float world[16];
    float world_inv[16];
    float sun_dir[4];   /* toward the light; w = 1 when there is one */
    float sun_col[4];
    float sun2_dir[4];  /* a second directional, w = 1 when there is one */
    float sun2_col[4];
    float ambient[4];   /* D3DRS_AMBIENT */
    float fog[4];       /* start, end, on */
    float fog_col[4];
    int32_t light[GFX_WORLD_LIGHTS]; /* into GfxWorldFrame.light; -1 none */
} GfxWorldInst;
typedef struct GfxWorldItem
{
    GfxBuf* vb;         /* 36-byte vertices: position, normal, D3DCOLOR, uv (FVF 0x152) */
    GfxTex* tex;
    uint32_t first, count, prim; /* first vertex, primitive count, GFX_TRIANGLELIST or GFX_TRIANGLESTRIP */
    uint32_t inst;      /* into GfxWorldFrame.inst */
    uint32_t flags;     /* GFX_WORLD_* */
    GfxSampler samp;    /* as the game set stage 0 (its addressing from flags) */
} GfxWorldItem;
typedef struct GfxWorldFrame
{
    float view[16], proj[16];
    uint32_t vp[6];        /* D3DVIEWPORT8 */
    float pixel[2];        /* 1 / the viewport's width and height in the game's pixels: D3D's pixel centres (the
                            * generated functions' position fixup, gfx_msl.c) */
    uint32_t mode;         /* 1 per vertex, 2 per pixel */
    float alpha_ref;       /* 0..255 */
    int32_t zbias_opaque, zbias_blend;
    const GfxWorldInst* inst;
    uint32_t ninst;
    const GfxWorldItem* item; /* drawn in order: the caller puts the opaque ones first */
    uint32_t nitem;
    const GfxWorldLight* light;
    uint32_t nlight;
} GfxWorldFrame;
/* Whether this back end has the scene renderer (asked once, before the game's draws are dropped). */
int gfx_world_supported(void);
/* Draws one batch into the current targets; 0 when it drew nothing (no target). */
int gfx_world_draw(const GfxWorldFrame* f);
/* Characters (the scene renderer, host/scene.c): skinned on the GPU from the game's bone palettes and its meshes'
 * bone-space vertices (Os2), lit with the game's fixed function per vertex (its per-actor directional lights,
 * ambient and material), then its two texture stages: 2 x texture x light, then 2 x that x the texture factor, alpha
 * 4 x 2 x texture alpha x factor alpha twice, alpha tested GREATER the reference; no culling, table fog. */
typedef struct GfxCharVertex
{
    float a[4], b[4];   /* position x, y, z, weight for the first and second bone (the weight pre-applied) */
    float na[4], nb[4]; /* normal, likewise */
    uint32_t bones;     /* first bone, second, first's mirror bone, second's mirror bone: a byte each */
    uint32_t mirror;    /* the mirror's axis for each (0 none, 1 x, 2 y, 3 z): bits 0-1 and 2-3 */
} GfxCharVertex;
typedef struct GfxCharLight
{
    float pos[4];      /* point lights: world, w = range */
    float dir[4];      /* toward the light (directional); w = 1 directional, 2 point, 0 off */
    float diffuse[4];
    float ambient[4];
    float att[4];
} GfxCharLight;
typedef struct GfxCharState /* per mesh and material */
{
    float wvp[16];      /* (the game's WORLD, identity but in its cached-skin frames) x view x projection */
    float world[16];
    GfxCharLight light[4];
    float ambient[4];   /* D3DRS_AMBIENT */
    float mat_diffuse[4], mat_ambient[4], mat_emissive[4];
    float tfactor[4];   /* D3DRS_TEXTUREFACTOR */
    float fog[4];       /* start, end, on */
    float fog_col[4];
} GfxCharState;
typedef struct GfxCharItem
{
    GfxBuf* vb;         /* GfxCharVertex, the order of the game's skinned buffers */
    GfxBuf* uv;         /* float2 per vertex (the game's own buffer) */
    GfxBuf* ib;         /* the game's index buffer */
    uint32_t index_size;
    uint32_t first, count, prim; /* first index, primitive count, GFX_TRIANGLELIST or GFX_TRIANGLESTRIP */
    uint32_t palette;   /* first bone in GfxCharFrame.palette */
    uint32_t state;     /* into GfxCharFrame.state */
    uint32_t mirror;    /* the mirrored copy: mirror bones and axes */
    GfxTex* tex;
    GfxSampler samp;
} GfxCharItem;
typedef struct GfxCharFrame
{
    uint32_t vp[6];
    float pixel[2];
    float alpha_ref;    /* 0..255 */
    const float* palette; /* 16 floats a bone (D3D row-major, world space) */
    uint32_t nbones;
    const GfxCharState* state;
    uint32_t nstate;
    const GfxCharItem* item;
    uint32_t nitem;
} GfxCharFrame;
/* Draws a batch of characters into the current targets; 0 when it drew nothing. */
int gfx_chars_draw(const GfxCharFrame* f);
/* The lamps lit this frame (the light pool's point lights, world space), for the clustered lamps (scene_lamps): set
 * once a frame before its batches; n 0 none. */
void gfx_lamps(const GfxWorldLight* l, uint32_t n);
/* Whether the zone's light is fixed (its weather record says so: no sky, a dungeon or a room), from the scene
 * renderer each batch, for the modern fog's indoor dust. */
void gfx_set_fixed_light(int fixed);
/* The zone's weather as the server last sent it (0x00A, 0x057: 0 none, 3 fog, 6 rain, 10 wind, 11 gales, ...; -1 not
 * known), for the modern fog's ground mist and its drift. */
void gfx_set_weather(int weather);

/* While the GPU's passes are timed: the draws from here on are a timed pass of their own, 1 as the scene renderer's
 * zone (GFX_PASS_WORLD), 2 its characters (GFX_PASS_CHARS); 0 that pass ends here (the game's own draws, timed for
 * comparison). */
void gfx_world_time(int pass);

/* The scene renderer's sun and moon shadows (host/scene.c, the scene_shadows setting): stabilised cascades, each a
 * square of the same size round a centre that moves only in whole texels and only when the camera has gone a margin
 * from it, in a light space that turns only in the light's steps. The zone's layer is kept from frame to frame and
 * drawn again a cascade at a time (redraw); the characters' layer is drawn every frame from the batch's characters,
 * into the first nchar cascades. gfx_world_draw and gfx_chars_draw then shade the directional light that points
 * along dir (the sun's or the moon's term, nothing else) with it, until the next gfx_shadow_begin. */
#define GFX_SHADOW_CASCADES 4
typedef struct GfxShadowCascade
{
    float lmat[16];  /* world -> map: x, y -1..1 across it, z 0 (toward the light) .. 1 (D3D row vectors) */
    float radius;    /* the camera's distance it serves (the next one's beyond) */
    float texel;     /* world units a texel */
    float depth;     /* world units over z 0..1 */
    uint32_t valid;  /* drawn at least once */
} GfxShadowCascade;
typedef struct GfxShadowFrame
{
    uint32_t size;      /* texels across each map */
    uint32_t ncascades, nchar; /* cascades in use; how many of the first take the characters */
    float dir[4];       /* toward the light, world; w = 1 when shadows are on this frame */
    float cam[4];       /* the camera, world (the cascades are chosen by the distance from it) */
    float strength;     /* 0..1: how much of the light the shadow takes */
    float soft;         /* the penumbra's width, times the day's (1; the moon's wider) */
    float soft_tan;     /* contact-hardening soft shadows: tan of the light's angular radius (0: off, the fixed filter) */
    GfxShadowCascade c[GFX_SHADOW_CASCADES];
    int32_t redraw;     /* the zone's layer of this cascade drawn again from the items below; -1 none */
    const float* world; /* 16 floats an instance (the items' inst indexes it) */
    uint32_t nworld;
    const GfxWorldItem* item; /* GFX_WORLD_ATEST: alpha tested with tex; GFX_WORLD_BLEND ones are not given */
    uint32_t nitem;
    const GfxCharFrame* chars; /* this batch's characters, NULL none */
} GfxShadowFrame;
/* Before the batch's gfx_world_draw: the shadows' layers drawn as the frame says; f NULL or dir.w 0 ends them. */
void gfx_shadow_begin(const GfxShadowFrame* f);

/* The scene renderer's bounce light (host/scene.c, the scene_gi setting; gfx_gi_metal.inc): a grid of light probes
 * round the camera, traced against the zone's static geometry. gfx_gi_zone gives the zone once it is loaded (NULL
 * drops it): every static instance's world matrix and its inverse (D3D row-major, 16 floats each) and the submeshes to
 * trace (GfxWorldItem, opaque ones, tex for their colour). gfx_gi_frame, before each batch's gfx_world_draw, traces a
 * share of the probes (once a frame) and has the batch's zone and characters lit by them; NULL or strength 0 ends it. */
typedef struct GfxGiZone
{
    const float* world;
    const float* world_inv;
    uint32_t nworld;
    const GfxWorldItem* item;
    uint32_t nitem;
} GfxGiZone;
typedef struct GfxGiFrame
{
    float cam[4];                       /* world */
    float sun_dir[4], sun_col[4];       /* toward the light, w = 1 when there is one (as GfxWorldInst) */
    float sun2_dir[4], sun2_col[4];
    float ambient[4];
    const GfxWorldLight* light;         /* the lamps lit now (the light pool's) */
    uint32_t nlight;
    float strength;                     /* how much of the bounce light the surfaces take */
    float sky;                          /* how far the share of the sky a probe sees darkens the ambient (0 not) */
    float bounce;                       /* the earlier bounces' weight in a ray's hit (0 one bounce) */
    float ambient_bounce;               /* the ambient's weight at a ray's hit */
    float debug;                        /* 1 the bounce light alone, 2 the ambient's factor */
    uint32_t quality;                   /* 1 low, 2 high (both sampled per vertex), 3 high sampled per pixel in scene 2 */
} GfxGiFrame;
void gfx_gi_zone(const GfxGiZone* z);
void gfx_gi_frame(const GfxGiFrame* f);

/* Whether the scene effects can trace rays here (the rt setting: the GPU's ray queries, and on
 * Direct3D 12 the shaders built with dxc); the first call asks the device. */
int gfx_rt_supported(void);

/* The frame is done: the back buffer goes to the window. */
void gfx_present(GfxTex* backbuffer);
/* Metal and Android's C++ Vulkan (the screen's safe area, host/display_fit.c): the rectangle of the
 * drawable, in its pixels, the back buffer is presented into, the rest black; w 0 for all of it
 * (Android: the letterbox present_rect.h makes). */
void gfx_set_present_rect(int x, int y, int w, int h);
/* iOS's Metal (host/ios_main.c) and Android's overlay (host/android_overlay.c): a picture (RGBA8,
 * w x h, copied) drawn behind the present rectangle, filling the drawable; NULL for none (black). */
void gfx_set_wallpaper(const uint8_t* rgba, int w, int h);
/* iOS's Metal and Android's overlay (host/touch.c): the touch controls drawn over every frame, in the
 * drawable's pixels; copied, so the caller's array may change after. n 0 for none. */
enum
{
    GFX_TOUCH_BACK,   /* round, a chevron pointing left */
    GFX_TOUCH_BUTTON, /* round, a symbol (sym) or a label */
    GFX_TOUCH_PILL,   /* rounded, hw by hh, a label */
    GFX_TOUCH_RING,   /* a stick's base */
    GFX_TOUCH_KNOB,   /* a stick's knob */
};
enum
{
    GFX_SYM_NONE,
    GFX_SYM_CROSS, GFX_SYM_CIRCLE, GFX_SYM_SQUARE, GFX_SYM_TRIANGLE, /* the face buttons */
    GFX_SYM_UP, GFX_SYM_DOWN, GFX_SYM_LEFT, GFX_SYM_RIGHT,           /* the D-pad's */
    GFX_SYM_PAD,                                                     /* a gamepad: the controls' toggle */
    GFX_SYM_LOCK, GFX_SYM_UNLOCK,                                    /* the layout locked, or being moved */
};
typedef struct GfxTouchShape
{
    float x, y, hw, hh; /* the centre, and half the width and height (round: hw) */
    int kind, sym, pressed;
    float alpha;        /* 0..1: the whole shape's */
    char label[8];      /* PILL and BUTTON without a symbol */
} GfxTouchShape;
#define GFX_TOUCH_MAX 24
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n);
/* Waits for the GPU (tests). */
void gfx_finish(void);
/* Tests: build each pipeline in place, before its first draw, and keep no pipeline cache file. */
void gfx_set_sync_pipelines(int on);
/* Shaders or pipelines that failed to build so far (tests). */
uint32_t gfx_failures(void);

/* Frame profile (FFXI_PROFILE=1): the front end adds the time it spends turning a D3D call into
 * back-end work; the back end logs the breakdown every two seconds from Present. */
extern int gfx_profiling;
/* GPU time per scene-effects pass (sun maps, linz/zmip, ao, blur, temporal, gi, bloom, rays, comp,
 * fxaa, present), from timestamps at each pass's start and end: on with FFXI_PROFILE=1 (the log, every
 * two seconds) or gfx_pass_timing(1) (/fx prof). Back ends without timestamps time nothing.
 * gfx_pass_report writes the last two seconds' averages, ms a frame, a line each into out (max lines
 * at most): how many, 0 while nothing was timed. */
void gfx_pass_timing(int on);
int gfx_pass_report(char (*out)[96], int max);
uint64_t gfx_now_ns(void);
void gfx_prof_front(uint64_t ns);
/* A draw dropped, and why (the profile line counts each reason). */
enum
{
    GFX_SKIP_NO_SHADER,
    GFX_SKIP_STREAM,
    GFX_SKIP_NO_POSITION,
    GFX_SKIP_NO_BUFFER,
    GFX_SKIP_RANGE,
    GFX_SKIP_NO_INDICES,
    GFX_SKIP_PIPELINE,
    GFX_SKIP_NO_TARGET,
    GFX_NSKIPS,
};
void gfx_prof_skip(int reason);
/* Time in a shim call (thunk_timer): counted for the thread that presents, the game's. */
void gfx_prof_shim(uint64_t ns);
