/* The D3D8 front end (d3d8.c) on the graphics back end, without the game (R3.2): the same COM
 * objects and thunks FFXiMain calls, driven from here the way the game drives them - a device on no
 * window (offscreen), then clears, locks of the back buffer, textures filled through LockRect,
 * vertex and index buffers, FVF and declared layouts, the ...UP draws, CopyRects from the back
 * buffer into a render-target texture - with the pixels read back through the back buffer's own
 * LockRect.
 *
 * No translated code is linked: the generated tables are empty stubs below.
 *
 * usage: d3d8_test      (exit status 0 when every check passes) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8.h"
#include "gfx.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "runtime.h"
#include "thunk.h"

/* no translation in this test */
const RtEntry rt_table[1];
const unsigned rt_table_count = 0;
const unsigned char rt_table_patch[1];
const uint32_t rt_image_base = 0x10000000u, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
               rt_image_packed_rva, rt_image_packed_len, rt_image_oep, rt_image_reloc_rva;

#define W 32
#define H 32

static int g_fails;
#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

/* IDirect3DDevice8 vtable slots (the D3D8 ABI) */
enum
{
    D_Release = 2,
    D_Present = 15,
    D_GetBackBuffer = 16,
    D_CreateTexture = 20,
    D_CreateVertexBuffer = 23,
    D_CreateIndexBuffer = 24,
    D_CopyRects = 28,
    D_SetRenderTarget = 31,
    D_GetRenderTarget = 32,
    D_GetDepthStencilSurface = 33,
    D_BeginScene = 34,
    D_EndScene = 35,
    D_Clear = 36,
    D_SetTransform = 37,
    D_GetTransform = 38,
    D_SetRenderState = 50,
    D_ApplyStateBlock = 54,
    D_DeleteStateBlock = 56,
    D_CreateStateBlock = 57,
    D_SetTexture = 61,
    D_SetTextureStageState = 63,
    D_DrawPrimitive = 70,
    D_DrawIndexedPrimitive = 71,
    D_DrawPrimitiveUP = 72,
    D_CreateVertexShader = 75,
    D_SetVertexShader = 76,
    D_SetStreamSource = 83,
    D_SetIndices = 85,
};
/* IDirect3DTexture8 / IDirect3DSurface8 / buffers */
enum
{
    T_GetSurfaceLevel = 15,
    T_LockRect = 16,
    T_UnlockRect = 17,
    S_Release = 2,
    S_LockRect = 9,
    S_UnlockRect = 10,
    B_Lock = 11,
    B_Unlock = 12,
};

static uint32_t call(uint32_t obj, int slot, int n, ...)
{
    uint32_t args[12] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; ++i)
        args[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_call(rd32(rd32(obj) + 4u * (uint32_t)slot), (unsigned)n + 1, args);
}

static uint32_t galloc(uint32_t n)
{
    gt_lock();
    uint32_t p = gheap_alloc(n, 1);
    gt_unlock();
    return p;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t g_dev, g_bb, g_ds, g_out; /* g_ds: the device's own depth surface */

/* the back buffer's pixel through LockRect (A8R8G8B8 / X8R8G8B8 as D3D lays it out) */
static uint32_t pixel(int x, int y)
{
    uint32_t lr = galloc(8);
    call(g_bb, S_LockRect, 3, lr, 0, 0x10 /* READONLY */);
    uint32_t v = rd32(rd32(lr + 4) + (uint32_t)y * rd32(lr) + 4u * (uint32_t)x);
    call(g_bb, S_UnlockRect, 0);
    return v & 0xFFFFFF;
}

static void clear(uint32_t color)
{
    call(g_dev, D_Clear, 6, 0, 0, 3, color, fbits(1.0f), 0);
}

typedef struct
{
    float x, y, z, rhw;
    uint32_t c;
    float u, v;
} UiVert;

static void test_clear_and_lock(void)
{
    clear(0xFF336699u);
    CHECK(pixel(0, 0) == 0x336699 && pixel(31, 31) == 0x336699, "clear via LockRect: %06x", pixel(0, 0));
}

static void test_up_texture(void)
{
    clear(0xFF000000u);
    /* a 2x2 A8R8G8B8 texture filled through LockRect, the way the game loads its own */
    call(g_dev, D_CreateTexture, 7, 2, 2, 1, 0, 21, 1 /* MANAGED */, g_out);
    uint32_t tex = rd32(g_out);
    uint32_t lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
    wr32(bits, 0xFFFF0000u), wr32(bits + 4, 0xFF00FF00u), wr32(bits + pitch, 0xFF0000FFu), wr32(bits + pitch + 4, 0xFFFFFFFFu);
    call(tex, T_UnlockRect, 1, 0);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 1); /* MAGFILTER point */
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 1);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5 /* TRIANGLESTRIP */, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    CHECK(pixel(2, 2) == 0xFF0000 && pixel(12, 2) == 0x00FF00 && pixel(2, 12) == 0x0000FF && pixel(12, 12) == 0xFFFFFF,
        "UP draw with a locked texture: %06x %06x %06x %06x", pixel(2, 2), pixel(12, 2), pixel(2, 12), pixel(12, 12));

    /* relock, rewrite one texel: the next draw sees it */
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    wr32(rd32(lr + 4), 0xFFFFFF00u);
    call(tex, T_UnlockRect, 1, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    CHECK(pixel(2, 2) == 0xFFFF00, "texture relocked: %06x", pixel(2, 2));
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);
}

/* XYZ | DIFFUSE (0x042) in a vertex buffer, identity transforms, lighting off; then indexed */
static void test_buffers(void)
{
    clear(0xFF000000u);
    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 16, 0x8, 0x42, 1, g_out);
    uint32_t vb = rd32(g_out);
    uint32_t pp = galloc(4);
    call(vb, B_Lock, 4, 0, 0, pp, 0);
    float vs[4][4] = { { -1, 1, 0.5f, 0 }, { 0, 1, 0.5f, 0 }, { -1, 0, 0.5f, 0 }, { 0, 0, 0.5f, 0 } };
    for (int i = 0; i < 4; ++i)
    {
        uint32_t c = 0xFFFF8000u;
        memcpy(&vs[i][3], &c, 4);
    }
    memcpy(GUEST_PTR(rd32(pp)), vs, sizeof vs);
    call(vb, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, 0x42);
    call(g_dev, D_SetStreamSource, 3, 0, vb, 16);
    call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
    CHECK(pixel(4, 4) == 0xFF8000 && pixel(20, 20) == 0, "vertex buffer: %06x %06x", pixel(4, 4), pixel(20, 20));

    /* the same four vertices through an index buffer: indices 1, 2, 3 from startIndex 1 */
    call(g_dev, D_CreateIndexBuffer, 5, 6 * 2, 0x8, 101, 1, g_out);
    uint32_t ib = rd32(g_out);
    call(ib, B_Lock, 4, 0, 0, pp, 0);
    uint16_t idx[6] = { 0, 1, 2, 3, 0, 0 };
    memcpy(GUEST_PTR(rd32(pp)), idx, sizeof idx);
    call(ib, B_Unlock, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetIndices, 2, ib, 0);
    call(g_dev, D_DrawIndexedPrimitive, 5, 4 /* LIST */, 0, 4, 1, 1); /* indices 1, 2, 3 */
    /* triangle (0,1) (-1,0) (0,0): the lower-right half of the top-left quarter */
    CHECK(pixel(4, 4) == 0 && pixel(14, 14) == 0xFF8000, "indexed: %06x %06x", pixel(4, 4), pixel(14, 14));
    call(g_dev, D_SetIndices, 2, 0, 0);
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(ib, D_Release, 0);
    call(vb, D_Release, 0);
}

/* a declared layout on two streams: position in stream 0, color in stream 1 */
static void test_declaration(void)
{
    clear(0xFF000000u);
    uint32_t decl[] = {
        0x20000000u,                        /* STREAM(0) */
        0x40000000u | (2u << 16) | 0,       /* REG(v0, FLOAT3) */
        0x20000001u,                        /* STREAM(1) */
        0x40000000u | (4u << 16) | 5,       /* REG(v5, D3DCOLOR) */
        0xFFFFFFFFu,
    };
    uint32_t gd = galloc(sizeof decl);
    memcpy(GUEST_PTR(gd), decl, sizeof decl);
    call(g_dev, D_CreateVertexShader, 4, gd, 0, g_out, 0);
    uint32_t h = rd32(g_out);
    CHECK(h & 1, "declared shader handle %08x", h);
    uint32_t pp = galloc(4);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 12, 0x8, 0, 1, g_out);
    uint32_t pos = rd32(g_out);
    call(pos, B_Lock, 4, 0, 0, pp, 0);
    float p[4][3] = { { 0, 0, 0.5f }, { 1, 0, 0.5f }, { 0, -1, 0.5f }, { 1, -1, 0.5f } };
    memcpy(GUEST_PTR(rd32(pp)), p, sizeof p);
    call(pos, B_Unlock, 0);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 4, 0x8, 0, 1, g_out);
    uint32_t col = rd32(g_out);
    call(col, B_Lock, 4, 0, 0, pp, 0);
    uint32_t c[4] = { 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu };
    memcpy(GUEST_PTR(rd32(pp)), c, sizeof c);
    call(col, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, h);
    call(g_dev, D_SetStreamSource, 3, 0, pos, 12);
    call(g_dev, D_SetStreamSource, 3, 1, col, 4);
    call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
    CHECK(pixel(20, 20) == 0x00FFFF && pixel(4, 4) == 0, "two streams: %06x %06x", pixel(20, 20), pixel(4, 4));
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(g_dev, D_SetStreamSource, 3, 1, 0, 0);
    call(pos, D_Release, 0);
    call(col, D_Release, 0);
}

/* the back buffer copied into a render-target texture, which is then drawn */
static void test_copyrects(void)
{
    clear(0xFF123456u);
    call(g_dev, D_CreateTexture, 7, W, H, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t tex = rd32(g_out);
    call(tex, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t surf = rd32(g_out);
    call(g_dev, D_CopyRects, 5, g_bb, 0, 0, surf, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 4 /* MODULATE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 32, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 32, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 32, 32, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    CHECK(pixel(16, 16) == 0x123456, "CopyRects back buffer -> texture: %06x", pixel(16, 16));

    /* render into the texture itself, then read it through its surface */
    call(g_dev, D_SetRenderTarget, 2, surf, 0);
    clear(0xFF654321u);
    uint32_t lr = galloc(8);
    call(surf, S_LockRect, 3, lr, 0, 0x10);
    uint32_t got = rd32(rd32(lr + 4)) & 0xFFFFFF;
    call(surf, S_UnlockRect, 0);
    CHECK(got == 0x654321, "render-target texture readback: %06x", got);
    call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(surf, S_Release, 0);
    call(tex, D_Release, 0);
}

/* a solid 4x4 DXT1 block */
static void dxt1_block(uint8_t* b, uint16_t c565)
{
    b[0] = b[2] = (uint8_t)c565, b[1] = b[3] = (uint8_t)(c565 >> 8);
    b[4] = b[5] = b[6] = b[7] = 0;
}

static void draw_textured_quad(uint32_t tex)
{
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
}

/* A texture pack entry: a 4x4 DXT1 texture the game fills with red is drawn with an 8x8 green one
 * (and its 4x4 mipmap) from <hash>_4x4.dds; once the game writes blue into it, its own again. */
static void test_texture_pack(void)
{
    uint8_t red[8], blue[8];
    dxt1_block(red, 0xF800);
    dxt1_block(blue, 0x001F);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int i = 0; i < 8; ++i)
        hash = (hash ^ red[i]) * 0x100000001b3ull;
    const char* dir = "build/d3d8test/textures";
    plat_mkdir("build/d3d8test");
    plat_mkdir(dir);
    char path[256];
    snprintf(path, sizeof path, "%s/%016llx_4x4.dds", dir, (unsigned long long)hash);
    uint8_t dds[128 + 32 + 8] = { 'D', 'D', 'S', ' ', 124 };
    dds[12] = dds[16] = 8;                                     /* height, width */
    dds[28] = 2;                                               /* mipmaps */
    dds[76] = 32, dds[80] = 4;                                 /* pixel format: FOURCC */
    memcpy(dds + 84, "DXT1", 4);
    for (int i = 0; i < 5; ++i)
        dxt1_block(dds + 128 + 8 * i, 0x07E0); /* 8x8: four blocks, then 4x4: one */
    FILE* f = fopen(path, "wb");
    CHECK(f && fwrite(dds, 1, sizeof dds, f) == sizeof dds, "cannot write %s", path);
    if (f)
        fclose(f);
    d3d8_texture_pack(dir);

    clear(0xFF000000u);
    call(g_dev, D_CreateTexture, 7, 4, 4, 1, 0, 0x31545844u /* DXT1 */, 1, g_out);
    uint32_t tex = rd32(g_out), lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    memcpy(GUEST_PTR(rd32(lr + 4)), red, 8);
    call(tex, T_UnlockRect, 1, 0);
    draw_textured_quad(tex);
    CHECK(pixel(8, 8) == 0x00FF00, "texture pack: the replacement drawn: %06x", pixel(8, 8));

    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    memcpy(GUEST_PTR(rd32(lr + 4)), blue, 8);
    call(tex, T_UnlockRect, 1, 0);
    draw_textured_quad(tex);
    CHECK(pixel(8, 8) == 0x0000FF, "texture pack: other contents, the game's own: %06x", pixel(8, 8));
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);
}

/* A D3DSBT_ALL block made before a transform is first set holds that transform's default: applied
 * after the game sets it, it puts identity back (state_copy stops at the highest transform ever set;
 * a new block starts as the device is past it, not as calloc left it) */
static void test_state_blocks(void)
{
    const uint32_t world44 = 256 + 44; /* D3DTS_WORLDMATRIX(44): no other test sets it */
    call(g_dev, D_CreateStateBlock, 2, 1 /* D3DSBT_ALL */, g_out);
    uint32_t block = rd32(g_out);
    CHECK(block != 0, "CreateStateBlock");
    uint32_t m = galloc(64);
    for (int i = 0; i < 16; ++i)
        wr32(m + 4u * (uint32_t)i, fbits(i % 5 == 0 ? 2.0f : 0.0f)); /* a scale by 2 */
    call(g_dev, D_SetTransform, 2, world44, m);
    call(g_dev, D_ApplyStateBlock, 1, block);
    call(g_dev, D_GetTransform, 2, world44, m);
    int identity = 1;
    for (int i = 0; i < 16; ++i)
    {
        float f;
        uint32_t u = rd32(m + 4u * (uint32_t)i);
        memcpy(&f, &u, 4);
        identity &= f == (i % 5 == 0 ? 1.0f : 0.0f);
    }
    uint32_t d0 = rd32(m), d5 = rd32(m + 20);
    CHECK(identity, "state block: a transform first set after it was made, applied back: [0] %08x [5] %08x (want identity)", d0, d5);
    call(g_dev, D_DeleteStateBlock, 1, block);
}

/* a 1x1 A8R8G8B8 texture of one colour, for the tests' quads to take their colour and alpha from */
static uint32_t solid_texture(uint32_t argb)
{
    call(g_dev, D_CreateTexture, 7, 1, 1, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */, g_out);
    uint32_t tex = rd32(g_out), lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    wr32(rd32(lr + 4), argb);
    call(tex, T_UnlockRect, 1, 0);
    return tex;
}

/* Port Bastok's sea (d3d8.c water_hold, after portfix): the scrolling ripple layer, added with depth written, then a
 * tint 0.0004 behind it in depth with LESSEQUAL. As the PC draws them the tint fails against the ripples' depth and
 * only the ripples show; fixed, the tint is drawn first and the ripples added over it. */
static void test_water_layers(void)
{
    typedef struct { float x, y, z, nx, ny, nz; uint32_t c; float u, v; } WaterVert; /* FVF 0x152 */
    call(g_dev, D_SetRenderTarget, 2, g_bb, g_ds); /* the depth the layers are tested against */
    call(g_dev, D_SetRenderState, 2, 7, 1);    /* ZENABLE */
    call(g_dev, D_SetRenderState, 2, 23, 4);   /* ZFUNC LESSEQUAL */
    call(g_dev, D_SetRenderState, 2, 137, 0);  /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);   /* CULL none */
    clear(0xFF000000u);
    uint32_t tex = solid_texture(0xFFFFFFFFu), pp = galloc(4);
    call(g_dev, D_CreateVertexBuffer, 5, 8 * (uint32_t)sizeof(WaterVert), 0x8, 0x152, 1, g_out);
    uint32_t vb = rd32(g_out);
    call(vb, B_Lock, 4, 0, 0, pp, 0);
    WaterVert v[8] = {
        { -1, 1, 0.5f, 0, 1, 0, 0xFF400000u, 0, 0 }, { 1, 1, 0.5f, 0, 1, 0, 0xFF400000u, 1, 0 }, /* the ripples: red */
        { -1, -1, 0.5f, 0, 1, 0, 0xFF400000u, 0, 1 }, { 1, -1, 0.5f, 0, 1, 0, 0xFF400000u, 1, 1 },
        { -1, 1, 0.5004f, 0, 1, 0, 0xFF0000FFu, 0, 0 }, { 1, 1, 0.5004f, 0, 1, 0, 0xFF0000FFu, 1, 0 }, /* the tint: blue */
        { -1, -1, 0.5004f, 0, 1, 0, 0xFF0000FFu, 0, 1 }, { 1, -1, 0.5004f, 0, 1, 0, 0xFF0000FFu, 1, 1 },
    };
    memcpy(GUEST_PTR(rd32(pp)), v, sizeof v);
    call(vb, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, 0x152);
    call(g_dev, D_SetStreamSource, 3, 0, vb, (uint32_t)sizeof(WaterVert));
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 4 /* COLOROP MODULATE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 3, 0 /* DIFFUSE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 4, 4 /* ALPHAOP MODULATE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 5, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 6, 0 /* DIFFUSE */);
    call(g_dev, D_BeginScene, 0);
    /* the ripples: added (SRCALPHA, ONE), depth written, texture coordinates scrolled (COUNT2) */
    call(g_dev, D_SetRenderState, 2, 14, 1);
    call(g_dev, D_SetRenderState, 2, 27, 1);
    call(g_dev, D_SetRenderState, 2, 19, 5);
    call(g_dev, D_SetRenderState, 2, 20, 2);
    call(g_dev, D_SetTextureStageState, 3, 0, 24, 2);
    call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
    /* the tint: opaque, depth tested LESSEQUAL a little behind the ripples */
    call(g_dev, D_SetRenderState, 2, 27, 0);
    call(g_dev, D_SetTextureStageState, 3, 0, 24, 0);
    call(g_dev, D_DrawPrimitive, 3, 5, 4, 2);
    call(g_dev, D_EndScene, 0);
    uint32_t p = pixel(16, 16);
    CHECK(p == 0x4000FF, "water layers: the tint under the ripples: %06x (want 4000ff, 400000 as the PC draws them)", p);
    call(g_dev, D_SetTextureStageState, 3, 0, 24, 0);
    call(g_dev, D_SetRenderState, 2, 27, 0);
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(vb, D_Release, 0);
    call(tex, D_Release, 0);
}

/* A menu button (d3d8.c button_body, after portfix): a black copy of its sprite 2 pixels down and right, alpha
 * blended, then the body added (SRCALPHA, ONE) over a grey background. Along the body's top-left edge, past the
 * shadow, the PC adds it to the grey (a light rim); fixed, the body is alpha blended and that edge is its own colour. */
static void test_button_rims(void)
{
    call(g_dev, D_SetRenderState, 2, 7, 0); /* ZENABLE off */
    clear(0xFF404040u);
    uint32_t tex = solid_texture(0xFF2050A0u);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 4 /* COLOROP MODULATE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 3, 0 /* DIFFUSE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 4, 2 /* ALPHAOP SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 5, 2 /* TEXTURE */);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    call(g_dev, D_SetRenderState, 2, 27, 1);
    call(g_dev, D_SetRenderState, 2, 19, 5); /* SRCALPHA */
    uint32_t v = galloc(4 * sizeof(UiVert));
    call(g_dev, D_BeginScene, 0);
    UiVert shadow[4] = { { 6, 6, 0.5f, 1, 0xFF000000u, 0, 0 }, { 26, 6, 0.5f, 1, 0xFF000000u, 1, 0 },
                         { 6, 16, 0.5f, 1, 0xFF000000u, 0, 1 }, { 26, 16, 0.5f, 1, 0xFF000000u, 1, 1 } };
    memcpy(GUEST_PTR(v), shadow, sizeof shadow);
    call(g_dev, D_SetRenderState, 2, 20, 6); /* INVSRCALPHA */
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    UiVert body[4] = { { 4, 4, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 24, 4, 0.5f, 1, 0xFFFFFFFFu, 1, 0 },
                       { 4, 14, 0.5f, 1, 0xFFFFFFFFu, 0, 1 }, { 24, 14, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    uint32_t v2 = galloc(4 * sizeof(UiVert));
    memcpy(GUEST_PTR(v2), body, sizeof body);
    call(g_dev, D_SetRenderState, 2, 20, 2); /* ONE */
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v2, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    uint32_t edge = pixel(4, 4), inside = pixel(12, 10);
    CHECK(edge == 0x2050A0 && inside == 0x2050A0, "button rims: edge %06x, inside %06x (want 2050a0 both; the PC's rim: 6090e0)",
        edge, inside);
    call(g_dev, D_SetRenderState, 2, 27, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);
}

/* A summon's halo (d3d8.c halo_quad, after portfix): the model drawn into a silhouette target the screen's size, copied
 * down to 256x256 with one quad, then the copy drawn over the scene as quads nudged by a few PS2 pixels. Fixed, a quad
 * reading the copy samples the silhouette instead, its nudge times the target's width / 640 (here 32 / 640): the
 * square stays where the silhouette has it (within a pixel). As the PC draws it, the copy lands the whole nudge, 8
 * pixels, right. */
static void test_summon_halos(void)
{
    call(g_dev, D_SetRenderState, 2, 7, 0);  /* ZENABLE off */
    call(g_dev, D_SetRenderState, 2, 27, 0); /* no blending */
    call(g_dev, D_SetRenderState, 2, 15, 0); /* no alpha test while drawing the silhouette */
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t sil, low, sil_s, low_s;
    call(g_dev, D_CreateTexture, 7, W, H, 1, 1 /* RENDERTARGET */, 21, 0, g_out);
    sil = rd32(g_out);
    call(sil, T_GetSurfaceLevel, 2, 0, g_out);
    sil_s = rd32(g_out);
    call(g_dev, D_CreateTexture, 7, 256, 256, 1, 1, 21, 0, g_out);
    low = rd32(g_out);
    call(low, T_GetSurfaceLevel, 2, 0, g_out);
    low_s = rd32(g_out);
    uint32_t v = galloc(4 * sizeof(UiVert));
    call(g_dev, D_BeginScene, 0);
    /* the silhouette: an opaque white square 12..20 on a target cleared to alpha 0 */
    call(g_dev, D_SetRenderTarget, 2, sil_s, 0);
    clear(0x00000000u);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* COLOROP SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 4, 2 /* ALPHAOP SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 5, 0 /* DIFFUSE */);
    UiVert sq[4] = { { 12, 12, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 20, 12, 0.5f, 1, 0xFFFFFFFFu, 1, 0 },
                     { 12, 20, 0.5f, 1, 0xFFFFFFFFu, 0, 1 }, { 20, 20, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), sq, sizeof sq);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    /* the copy-down into 256x256, colour and alpha from the silhouette */
    call(g_dev, D_SetRenderTarget, 2, low_s, 0);
    call(g_dev, D_SetTexture, 2, 0, sil);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 5, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 1); /* point */
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 1);
    UiVert cp[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 256, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 },
                     { 0, 256, 0.5f, 1, 0xFFFFFFFFu, 0, 1 }, { 256, 256, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), cp, sizeof cp);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    /* the copy back over a black scene, alpha tested, nudged 8 right */
    call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetTexture, 2, 0, low);
    call(g_dev, D_SetRenderState, 2, 15, 1); /* ALPHATESTENABLE */
    call(g_dev, D_SetRenderState, 2, 24, 0); /* ALPHAREF 0 */
    call(g_dev, D_SetRenderState, 2, 25, 5); /* ALPHAFUNC GREATER */
    UiVert q[4] = { { 8, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 8 + W, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 },
                    { 8, H, 0.5f, 1, 0xFFFFFFFFu, 0, 1 }, { 8 + W, H, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    uint32_t left = pixel(13, 16), right = pixel(25, 16);
    CHECK(left == 0xFFFFFF && right == 0, "summon halos: (13,16) %06x, (25,16) %06x (want ffffff 000000; 000000 ffffff as "
        "the PC draws it)", left, right);
    call(g_dev, D_SetRenderState, 2, 15, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(sil_s, S_Release, 0);
    call(low_s, S_Release, 0);
    call(sil, D_Release, 0);
    call(low, D_Release, 0);
}

/* a quad facing the camera (normal 0,0,-1), XYZ|NORMAL|DIFFUSE through identity transforms, drawn indexed, its diffuse
 * black with alpha 0x80: the shine draws' first stage gives that colour and alpha, stage 1 adds the shine */
static void shine_quad(void)
{
    typedef struct { float x, y, z, nx, ny, nz; uint32_t c; } NV; /* FVF 0x52 */
    NV q[4] = { { -1, 1, 0.5f, 0, 0, -1, 0x80000000u }, { 1, 1, 0.5f, 0, 0, -1, 0x80000000u },
                { -1, -1, 0.5f, 0, 0, -1, 0x80000000u }, { 1, -1, 0.5f, 0, 0, -1, 0x80000000u } };
    uint32_t pp = galloc(4);
    call(g_dev, D_CreateVertexBuffer, 5, (uint32_t)sizeof q, 0x8, 0x52, 1, g_out);
    uint32_t vb = rd32(g_out);
    call(vb, B_Lock, 4, 0, 0, pp, 0);
    memcpy(GUEST_PTR(rd32(pp)), q, sizeof q);
    call(vb, B_Unlock, 0);
    call(g_dev, D_CreateIndexBuffer, 5, 6 * 2, 0x8, 101, 1, g_out);
    uint32_t ib = rd32(g_out);
    call(ib, B_Lock, 4, 0, 0, pp, 0);
    uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    memcpy(GUEST_PTR(rd32(pp)), idx, sizeof idx);
    call(ib, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, 0x52);
    call(g_dev, D_SetStreamSource, 3, 0, vb, (uint32_t)sizeof q[0]);
    call(g_dev, D_SetIndices, 2, ib, 0);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawIndexedPrimitive, 5, 4 /* LIST */, 0, 4, 0, 2);
    call(g_dev, D_EndScene, 0);
    call(g_dev, D_SetIndices, 2, 0, 0);
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(ib, D_Release, 0);
    call(vb, D_Release, 0);
}

/* the shine draws' stages: 0 the diffuse (black, alpha 0x80), 1 MODULATEALPHA_ADDCOLOR (CURRENT, TEXTURE) with
 * generated texture coordinates (tci) through D3DTS_TEXTURE1 (m), 2 off */
static void shine_stages(uint32_t tci, const float m[16], uint32_t tex)
{
    call(g_dev, D_SetRenderState, 2, 7, 0);   /* ZENABLE off */
    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetRenderState, 2, 27, 0);  /* no blending */
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 4, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 5, 0 /* DIFFUSE */);
    call(g_dev, D_SetTexture, 2, 1, tex);
    call(g_dev, D_SetTextureStageState, 3, 1, 1, 18 /* MODULATEALPHA_ADDCOLOR */);
    call(g_dev, D_SetTextureStageState, 3, 1, 2, 1 /* CURRENT */);
    call(g_dev, D_SetTextureStageState, 3, 1, 3, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 1, 4, 2 /* ALPHAOP SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 1, 5, 1 /* CURRENT */);
    call(g_dev, D_SetTextureStageState, 3, 1, 11, tci);
    call(g_dev, D_SetTextureStageState, 3, 1, 24, tci >> 16 == 3 ? 3 : 2); /* COUNT3 for the cube, COUNT2 */
    call(g_dev, D_SetTextureStageState, 3, 1, 16, 1); /* point */
    call(g_dev, D_SetTextureStageState, 3, 1, 17, 1);
    call(g_dev, D_SetTextureStageState, 3, 2, 1, 1 /* stage 2 off */);
    call(g_dev, D_SetTextureStageState, 3, 2, 4, 1);
    uint32_t mm = galloc(64);
    memcpy(GUEST_PTR(mm), m, 64);
    call(g_dev, D_SetTransform, 2, 17 /* D3DTS_TEXTURE1 */, mm);
}

static void shine_reset(void)
{
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t mm = galloc(64);
    memcpy(GUEST_PTR(mm), id, 64);
    call(g_dev, D_SetTransform, 2, 17, mm);
    call(g_dev, D_SetTexture, 2, 1, 0);
    call(g_dev, D_SetTextureStageState, 3, 1, 1, 1 /* DISABLE */);
    call(g_dev, D_SetTextureStageState, 3, 1, 4, 1);
    call(g_dev, D_SetTextureStageState, 3, 1, 11, 1);
    call(g_dev, D_SetTextureStageState, 3, 1, 24, 0);
}

/* Sphere-map shine (d3d8.c shine_draw, after portfix): a face turned to the camera. The PC's identity matrix reads the
 * map at (nx, ny) = (0, 0), its top-left texel (red), at half strength: 0x80 red. Fixed, the PS2's mapping reads its
 * centre, the bottom-right texel at point sampling (green), at the PS2's strength: full green. */
static void test_shine_sphere(void)
{
    clear(0xFF000000u);
    call(g_dev, D_CreateTexture, 7, 2, 2, 1, 0, 21, 1, g_out);
    uint32_t tex = rd32(g_out), lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
    wr32(bits, 0xFFFF0000u), wr32(bits + 4, 0xFF000000u), wr32(bits + pitch, 0xFF000000u), wr32(bits + pitch + 4, 0xFF00FF00u);
    call(tex, T_UnlockRect, 1, 0);
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    shine_stages(0x10000u | 1 /* CAMERASPACENORMAL */, id, tex);
    shine_quad();
    uint32_t p = pixel(16, 16);
    CHECK(p == 0x00FF00, "sphere-map shine: %06x (want 00ff00; about 800000 as the PC draws it)", p);
    shine_reset();
    call(tex, D_Release, 0);
}

/* The light's highlight (d3d8.c shine_draw, spot_cube): a cube white on -Z, black elsewhere, through the game's
 * diag(1, -1, -1); the face reflects the eye straight back (0, 0, -1). The PC's flip sends that to +Z, black. Fixed,
 * to -Z, the spot, times light 0's colour (white) x 2 x the stage-0 alpha (0x80): grey 0x80. */
static void test_shine_spot(void)
{
    clear(0xFF000000u);
    call(g_dev, 22 /* CreateCubeTexture */, 6, 16, 1, 0, 21, 1, g_out);
    uint32_t cube = rd32(g_out), lr = galloc(8);
    for (uint32_t f = 0; f < 6; ++f)
    {
        call(cube, 16 /* LockRect */, 5, f, 0, lr, 0, 0);
        uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                wr32(bits + y * pitch + 4 * x, f == 5 ? 0xFFFFFFFFu : 0xFF000000u);
        call(cube, 17 /* UnlockRect */, 2, f, 0);
    }
    uint32_t light = galloc(104);
    memset(GUEST_PTR(light), 0, 104);
    wr32(light, 3 /* DIRECTIONAL */);
    wr32(light + 4, fbits(1.0f)), wr32(light + 8, fbits(1.0f)), wr32(light + 12, fbits(1.0f)), wr32(light + 16, fbits(1.0f));
    wr32(light + 72, fbits(1.0f)); /* Direction z (D3DLIGHT8 +64 Direction) */
    call(g_dev, 44 /* SetLight */, 2, 0, light);
    call(g_dev, 46 /* LightEnable */, 2, 0, 1);
    call(g_dev, D_SetRenderState, 2, 60, 0x40000000u); /* TEXTUREFACTOR: the strength in its alpha */
    static const float flip[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1 };
    shine_stages(0x30000u | 1 /* CAMERASPACEREFLECTIONVECTOR */, flip, cube);
    shine_quad();
    uint32_t p = pixel(16, 16);
    CHECK(p == 0x808080 || p == 0x7F7F7F || p == 0x818181, "light's highlight: %06x (want 808080; 000000 as the PC draws it)", p);
    shine_reset();
    call(g_dev, 46, 2, 0, 0);
    call(cube, D_Release, 0);
}

int main(void)
{
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    gt_init();
    rt_set_native_handler(thunk_dispatch);
    gfx_set_sync_pipelines(1);
    d3d8_init();
    d3d8_setup();
    g_out = galloc(16);

    uint32_t arg = 220;
    uint32_t d3d = guest_call(thunk_for("d3d8.dll", "Direct3DCreate8"), 1, &arg);
    /* D3DPRESENT_PARAMETERS: 32x32 X8R8G8B8, windowed, auto depth D24S8 */
    uint32_t pp = galloc(52);
    uint32_t v[13] = { W, H, 22, 1, 0, 1, 0, 1, 1, 75, 0, 0, 0 };
    for (int i = 0; i < 13; ++i)
        wr32(pp + 4u * (uint32_t)i, v[i]);
    uint32_t hr = call(d3d, 15 /* CreateDevice */, 6, 0, 1, 0, 0x40, pp, g_out);
    g_dev = rd32(g_out);
    CHECK(hr == 0 && g_dev, "CreateDevice: %08x", hr);
    if (!g_dev)
        return 1;
    call(g_dev, D_GetBackBuffer, 3, 0, 0, g_out);
    g_bb = rd32(g_out);
    call(g_dev, D_GetDepthStencilSurface, 1, g_out);
    g_ds = rd32(g_out);
    CHECK(g_ds != 0, "the device's depth surface");

    test_clear_and_lock();
    test_up_texture();
    test_buffers();
    test_declaration();
    test_copyrects();
    test_texture_pack();
    test_state_blocks();
    test_water_layers();
    test_button_rims();
    test_summon_halos();
    test_shine_sphere();
    test_shine_spot();
    call(g_dev, D_Present, 4, 0, 0, 0, 0);
    printf(g_fails ? "d3d8_test: %d failed\n" : "d3d8_test: ok\n", g_fails);
    return g_fails != 0;
}
