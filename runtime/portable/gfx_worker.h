#pragma once
#ifdef __cplusplus
extern "C"
{
#endif
    // Must run before the Android SDL/native entry point returns. Drains ordered
    // work, joins the thread and restores direct calls. If the opt-in mailbox was
    // enabled, retires its bounded transfers before returning.
    void gfx_worker_shutdown(void);
#ifdef __cplusplus
}
#endif
