#pragma once
// Include after gfx.h in the Vulkan backend and before its definitions. Renames
// the public entry points to gfx_backend_* so gfx_worker.cpp can wrap them;
// calls inside the backend go straight to the backend, never through the queue.
#if defined(FFXI_ANDROID_VULKAN) && defined(FFXI_RENDER_WORKER_BUILD)
#define gfx_init gfx_backend_init
#define gfx_resize gfx_backend_resize
#define gfx_buf_create gfx_backend_buf_create
#define gfx_buf_destroy gfx_backend_buf_destroy
#define gfx_buf_upload gfx_backend_buf_upload
#define gfx_tex_create gfx_backend_tex_create
#define gfx_tex_destroy gfx_backend_tex_destroy
#define gfx_tex_upload gfx_backend_tex_upload
#define gfx_tex_upload_rect gfx_backend_tex_upload_rect
#define gfx_tex_read gfx_backend_tex_read
#define gfx_tex_read_async gfx_backend_tex_read_async
#define gfx_tex_read_async_keyed gfx_backend_tex_read_async_keyed
#define gfx_android_probe_read gfx_backend_android_probe_read
#define gfx_copy gfx_backend_copy
#define gfx_set_targets gfx_backend_set_targets
#define gfx_clear gfx_backend_clear
#define gfx_draw gfx_backend_draw
#define gfx_scene_done gfx_backend_scene_done
#define gfx_fx_set gfx_backend_fx_set
#define gfx_fx_get gfx_backend_fx_get
#define gfx_trace_dump gfx_backend_trace_dump
#define gfx_set_focus gfx_backend_set_focus
#define gfx_present gfx_backend_present
#define gfx_finish gfx_backend_finish
#define gfx_set_sync_pipelines gfx_backend_set_sync_pipelines
#define gfx_failures gfx_backend_failures
#define gfx_prof_front gfx_backend_prof_front
#define gfx_prof_skip gfx_backend_prof_skip
#define gfx_prof_shim gfx_backend_prof_shim
#endif
