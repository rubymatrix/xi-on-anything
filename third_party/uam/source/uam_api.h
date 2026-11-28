/* xi-on-anything: uam as a library (uam_api.cpp). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stages, as uam's pipeline_stage */
enum
{
	UAM_VERTEX = 0,
	UAM_TESS_CTRL = 1,
	UAM_TESS_EVAL = 2,
	UAM_GEOMETRY = 3,
	UAM_FRAGMENT = 4,
	UAM_COMPUTE = 5
};

/* Compiles one stage's GLSL to a DKSH (*dksh malloc'd, *size bytes; free() it). 1 on success, 0 on failure
 * (uam's messages go to stderr). Not thread-safe: one compile at a time. */
int uam_compile(int stage, const char* glsl, void** dksh, size_t* size);

#ifdef __cplusplus
}
#endif
