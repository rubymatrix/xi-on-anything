#pragma once
extern "C"
{
    int gfx_backend_init(void*, int);
    void gfx_backend_resize(uint32_t, uint32_t);
    GfxBuf* gfx_backend_buf_create(uint32_t);
    void gfx_backend_buf_destroy(GfxBuf*);
    void gfx_backend_buf_upload(GfxBuf*, const void*, uint32_t);
    GfxTex* gfx_backend_tex_create(int, uint32_t, uint32_t, uint32_t, uint32_t, int);
    void gfx_backend_tex_destroy(GfxTex*);
    void gfx_backend_tex_upload(GfxTex*, uint32_t, uint32_t, const void*, uint32_t);
    void gfx_backend_tex_upload_rect(GfxTex*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const void*,
                                     uint32_t);
    void gfx_backend_tex_read(GfxTex*, uint32_t, uint32_t, void*, uint32_t);
    void gfx_backend_tex_read_async(GfxTex*, uint32_t, uint32_t, void*, uint32_t);
    void gfx_backend_tex_read_async_keyed(GfxTex*, uint32_t, uint32_t, void*, uint32_t, uint64_t, uint32_t);
    void gfx_backend_android_probe_read(int, int);
    void gfx_backend_copy(GfxTex*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, GfxTex*, uint32_t,
                          uint32_t, uint32_t, uint32_t);
    void gfx_backend_set_targets(GfxTex*, uint32_t, uint32_t, GfxTex*);
    void gfx_backend_clear(uint32_t, const int32_t*, uint32_t, uint32_t, float, uint32_t, const uint32_t*);
    void gfx_backend_draw(const GfxDraw*);
    void gfx_backend_scene_done(GfxTex*, const GfxScene*);
    void gfx_backend_fx_set(const char*, float);
    float gfx_backend_fx_get(const char*);
    // True only if every key always returns this immutable value, unaffected by set.
    // Pure capability query, called once at init; false preserves ordered getters.
    int gfx_backend_fx_constant(float* value);
    void gfx_backend_trace_dump(const char*);
    void gfx_backend_set_focus(const float*);
    void gfx_backend_present(GfxTex*);
    void gfx_backend_finish(void);
    void gfx_backend_set_sync_pipelines(int);
    uint32_t gfx_backend_failures(void);
    void gfx_backend_prof_front(uint64_t);
    void gfx_backend_prof_skip(int);
    void gfx_backend_prof_shim(uint64_t);
}
