/* uam, the deko3d shader compiler, for the deko3d back end (dk_uam.c). */
#pragma once
#include <stddef.h>

/* One stage's GLSL (vertex 1, else fragment) to its DKSH (*dksh malloc'd, *size bytes); uam's messages, if any,
 * in *log (malloc'd, NULL when none). 1 on success. Not thread-safe: one compile at a time. */
int dk_uam_compile(int vertex, const char* glsl, void** dksh, size_t* size, char** log);
