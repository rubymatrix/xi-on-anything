/* xi-on-anything: uam as a library. One GLSL stage in, its DKSH (as uam's -o writes it) out, in memory.
 * uam's front end keeps global state (glsl_frontend_init/exit in DekoCompiler's constructor and destructor):
 * one compile at a time. */
#include "compiler_iface.h"
#include "uam_api.h"

extern "C" int uam_compile(int stage, const char* glsl, void** dksh, size_t* size)
{
	*dksh = NULL;
	*size = 0;
	if (stage < (int)pipeline_stage_vertex || stage > (int)pipeline_stage_compute || !glsl)
		return 0;
	DekoCompiler c((pipeline_stage)stage, 3);
	if (!c.CompileGlsl(glsl))
		return 0;
	*size = c.OutputDkshMem(dksh);
	return *size != 0;
}
