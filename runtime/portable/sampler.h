/* A sampling profiler for the calling thread (sampler_win.c on Windows, sampler_nx.c on the Switch): FFXI_SAMPLE=<file>. */
#pragma once

/* Samples the calling thread's stack every millisecond into path, until the process ends. 1, or 0
 * if the file cannot be written. */
int sampler_start(const char* path);
