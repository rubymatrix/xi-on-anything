/* A sampling profiler for the calling thread (sampler_win.c on Windows, sampler_nx.c on the Switch): FFXI_SAMPLE=<file>. */
#pragma once

/* Samples the calling thread's stack every millisecond into path, until the process ends. 1, or 0
 * if the file cannot be written. */
int sampler_start(const char* path);

#if defined(__SWITCH__)
/* the game thread's frame (gfx_queue.c counts them at each present): the sampler marks where each starts */
extern volatile uint32_t sampler_frame;
#endif
