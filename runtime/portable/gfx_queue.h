/* The render thread: gfx.h's calls from the D3D8 front end, recorded on the game thread and run on
 * a thread of their own (gfx_queue.c).
 *
 * The back end's own work per draw - encoding, the driver, uploading vertices - was a third of the
 * game thread's busy time in a crowded town, and more on slower cores. d3d8.c includes this header
 * in place of gfx.h, so its gfx_* calls become gfxq_* calls with the same meaning:
 *
 *  - Textures and buffers are handles made at once; the back end's objects are made in order on
 *    the render thread.
 *  - Everything a call points at in the game's memory (texels, vertices, indices, rectangles) is
 *    copied when it is recorded, so the game may change it as soon as the call returns.
 *  - Calls that return something or write the game's memory (reads, finish, failures) wait for the
 *    render thread to catch up and then run on the calling thread; an async read answers from the
 *    newest copy the render thread made, as the back end's own does from the GPU's.
 *  - Present lets the game get at most one frame ahead of the render thread.
 *
 * FFXI_RENDER_THREAD=0 runs every call in place, as before.
 *
 * Built in with -DGFX_QUEUE (the browser build): every front end - d3d8.c, host64.c, modern.c,
 * uidraw.c - includes this, and their gfx_* calls go through it. Without GFX_QUEUE the macros are
 * off and every call goes straight to the back end, as before.
 *
 * In the browser (__EMSCRIPTEN__) there is no render thread: WebGPU belongs to the page's main thread,
 * which runs the recorded calls from its animation frames (gfxq_pump), at most one Present each.
 * The calls that answer the game (init, reads, failures) go over as calls the game's thread waits on;
 * a read can finish later, when WebGPU has mapped the copy (gfxq_call_done). */
#pragma once

#include "gfx.h"

int gfxq_init(void* sdl_window, int vsync);
void gfxq_resize(uint32_t w, uint32_t h);
GfxBuf* gfxq_buf_create(uint32_t size);
void gfxq_buf_destroy(GfxBuf* b);
void gfxq_buf_upload(GfxBuf* b, const void* data, uint32_t size);
GfxTex* gfxq_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use);
void gfxq_tex_destroy(GfxTex* t);
void gfxq_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch);
void gfxq_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch);
void gfxq_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
void gfxq_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
void gfxq_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
    GfxTex* dst, uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy);
void gfxq_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth);
void gfxq_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t vp[6]);
void gfxq_draw(const GfxDraw* d);
void gfxq_scene_done(GfxTex* color, const GfxScene* s);
void gfxq_fx_set(const char* key, float v);
void gfxq_present(GfxTex* backbuffer);
void gfxq_finish(void);
void gfxq_set_sync_pipelines(int on);
uint32_t gfxq_failures(void);
void gfxq_set_focus(const float* pos);
void gfxq_set_moghouse(int in);
void gfxq_trace_dump(const char* path);

/* The browser: the page's main thread runs what the game recorded, from requestAnimationFrame.
 * Returns 1 when it ran a Present (the canvas has a new frame). */
int gfxq_pump(void);
/* A call the game's thread waits on, run where the back end lives: fn does it and calls
 * gfxq_call_done(call) - at once, or later (from a WebGPU callback) for a read. */
typedef struct GfxqCall GfxqCall;
void gfxq_call_done(GfxqCall* call);
/* What a back end gives the queue for gfx_tex_read: the read, then gfxq_call_done(call) - at once,
 * or (WebGPU) when the copy has been mapped. */
void gfx_tex_read_then(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch, GfxqCall* call);

#if defined(GFX_QUEUE) && !defined(GFX_QUEUE_IMPL)
#define gfx_init gfxq_init
#define gfx_resize gfxq_resize
#define gfx_buf_create gfxq_buf_create
#define gfx_buf_destroy gfxq_buf_destroy
#define gfx_buf_upload gfxq_buf_upload
#define gfx_tex_create gfxq_tex_create
#define gfx_tex_destroy gfxq_tex_destroy
#define gfx_tex_upload gfxq_tex_upload
#define gfx_tex_upload_rect gfxq_tex_upload_rect
#define gfx_tex_read gfxq_tex_read
#define gfx_tex_read_async gfxq_tex_read_async
#define gfx_copy gfxq_copy
#define gfx_set_targets gfxq_set_targets
#define gfx_clear gfxq_clear
#define gfx_draw gfxq_draw
#define gfx_scene_done gfxq_scene_done
#define gfx_fx_set gfxq_fx_set
#define gfx_present gfxq_present
#define gfx_finish gfxq_finish
#define gfx_set_sync_pipelines gfxq_set_sync_pipelines
#define gfx_failures gfxq_failures
#define gfx_set_focus gfxq_set_focus
#define gfx_set_moghouse gfxq_set_moghouse
#define gfx_trace_dump gfxq_trace_dump
/* gfx_fx_get, gfx_sun_shadows_shown and gfx_sun_prime read the back end's CPU-side state, and stay
 * direct calls from the game's thread */
#endif
