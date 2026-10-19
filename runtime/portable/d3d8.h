/* Direct3D 8 for 64-bit hosts (d3d8.c): the D3D8 front end the Metal renderer goes under. */
#pragma once
#include <stdint.h>

/* Registers the shims (before the images are mapped). */
void d3d8_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void d3d8_setup(void);
/* Called at every Present, on the game's thread with the guest lock held, before the frame goes
 * out: where the host adjusts per-frame game state (host64: the frame-rate divisor). */
void d3d8_set_present_hook(void (*fn)(void));
/* Called at every Present just after the frame went out, on the game's thread: between frames, where
 * the host changes the display (modern.c). */
void d3d8_set_after_present(void (*fn)(void));
/* The back buffer and its depth at a new size, the game's objects kept (the game's own copies of the
 * size are the caller's); 0 before the device exists. */
int d3d8_resize(uint32_t w, uint32_t h);
/* The device's window (0 before the device exists) */
uint32_t d3d8_window(void);
/* The size the frame is shown at: the device window's client area, else the back buffer's; 0x0
 * before the device exists. */
void d3d8_screen_size(uint32_t* w, uint32_t* h);
/* The back buffer's size (0x0 before the device exists). */
void d3d8_backbuffer_size(uint32_t* w, uint32_t* h);
/* The device's current transform (D3DTS_*) and render state as the game last set them (read only;
 * the addon host's Direct3D for addons): 0 before the device exists. */
int d3d8_get_transform(uint32_t ts, float m[16]);
int d3d8_get_render_state(uint32_t rs, uint32_t* v);
/* Called at every Present after the present hook, with the back buffer the frame is in: where the
 * addon host draws its overlay (gfx_draw into it; d3d8 binds its own targets again at its next
 * draw). */
typedef struct GfxTex GfxTex;
void d3d8_set_overlay(void (*fn)(GfxTex* backbuffer, uint32_t w, uint32_t h));
/* The next frame as shown (overlay included), written to path at its Present: "XIF1", then the
 * width, height and D3DFORMAT as uint32s, then the rows, top first, in that format. A serial that
 * goes up once the file is written; d3d8_capture_result says what came of the last request. */
void d3d8_capture(const char* path);
uint32_t d3d8_capture_result(uint32_t* w, uint32_t* h, uint32_t* format, int* ok);
/* Adds a texture pack: <dir>/<hash>_<w>x<h>.dds replacements for the game's textures (see d3d8.c,
 * tools/make_texpack.py). Before the device is created. */
void d3d8_texture_pack(const char* dir);
/* The game is loading one of its water models (host64's hook on the model loader): 1 before, 0 after.
 * The vertex buffers made meanwhile are the model's, and draws from them are drawn as water. */
void d3d8_water_loading(int on);
/* How the game's occlusion probe is answered (no menu sets it now; FFXI_PROBE=gpu reads exactly): 0 fully visible,
 * 1 the newest copy the GPU has finished (Android only; elsewhere it reads as 0), 2 exactly, waiting
 * for the GPU. 0 until set. */
void d3d8_set_occlusion(int mode);
int d3d8_occlusion(void);

/* The scene renderer (host/scene.c, gfx_world_draw): fn runs before the next piece of work that would come
 * after its batch (a draw, a clear, a change of target, a copy or lock of one, Present); a game texture
 * (IDirect3DTexture8*) and static vertex buffer as the back end holds them, uploaded if they were written
 * since (NULL for others); stage 0's sampler as the game has it set, for that texture; and the game's
 * current target bound for drawing, with its viewport in the target's pixels and in the game's (0 when there
 * is none). */
typedef struct GfxBuf GfxBuf;
void d3d8_set_world_flush(void (*fn)(void));
/* For comparison: the game's draws that return to site (its fixed-function zone draw), timed as a pass of their own;
 * 0 none. */
void d3d8_world_time(uint32_t site);
GfxTex* d3d8_world_texture(uint32_t iface);
typedef struct GfxSampler GfxSampler;
int d3d8_world_sampler(uint32_t iface, GfxSampler* out);
GfxBuf* d3d8_world_vbuf(uint32_t iface, uint32_t* size);
int d3d8_world_begin(uint32_t vp[6], uint32_t game_vp[6]);
