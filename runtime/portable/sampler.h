/* A sampling profiler for the calling thread (sampler_win.c on Windows, sampler_nx.c on the Switch): FFXI_SAMPLE=<file>. */
#pragma once

/* Samples the calling thread's stack every millisecond into path, until the process ends. 1, or 0
 * if the file cannot be written. */
int sampler_start(const char* path);

#if defined(__SWITCH__)
/* the game thread's frame (gfx_queue.c counts them at each present): the sampler marks where each starts */
extern volatile uint32_t sampler_frame;
/* FFXI_SAMPLE_THREAD=<role>: the calling thread sampled from now on in the game thread's place, when role is the one
 * asked for (the render thread calls it with "render") */
void sampler_thread_here(const char* role);
/* The same for another thread, by its handle (FFXI_SAMPLE_THREAD=glthread: Mesa's glthread worker, which
 * platforms/switch/switch_main.c sees made by pthread_create) */
void sampler_thread_handle(const char* role, unsigned handle);
#endif
