/* drawcost: the CPU cost of a draw call on the Switch, on OpenGL (Mesa 26, nouveau) and on deko3d.
 *
 * Both paths draw the same scene: N small indexed draws a frame (default 1,500), each one rebinding
 *   - a uniform block range: a 256-byte slice of one big uniform buffer, a different one per draw;
 *   - the texture on unit 0: one of 16 small textures, a different one per draw;
 *   - the vertex buffer: one of 8, a different one per draw;
 *   - the program (vertex + fragment): one of 4, every prog_every draws (default 4).
 * A mode chooses which of these are rebound (all, none, or one at a time), to split the cost.
 * The mesh is a 2x2 grid of quads (9 vertices, 8 triangles, 24 u16 indices), about 12 pixels on screen.
 *
 * Everything here runs on the main thread, and only the main thread writes the log (another thread's
 * stdio breaks the main thread's on the Switch). */
#pragma once

#include <stdint.h>
#include <stdio.h>

#define DC_MAX_DRAWS 6000
#define DC_NTEX 16
#define DC_NVB 8
#define DC_NPROG 4
#define DC_UBO_STRIDE 256 /* one draw's uniform block range: the GPU's offset alignment */
#define DC_TEX_SIZE 16
#define DC_NVERT 9
#define DC_NIDX 24
#define DC_W 1280
#define DC_H 720

/* the rebinding a mode does */
enum
{
    DC_BIND_UBO = 1,
    DC_BIND_TEX = 2,
    DC_BIND_VB = 4,
    DC_BIND_PROG = 8,
    DC_BIND_ALL = 15,
};

typedef struct
{
    float pos[2];
    float uv[2];
} DcVertex;

/* the uniform block: std140, the first 32 bytes read, the range bound is DC_UBO_STRIDE */
typedef struct
{
    float xform[4]; /* offset xy, scale zw */
    float color[4];
    float pad[56];
} DcUniforms;

typedef struct
{
    int draws;      /* draws a frame */
    int prog_every; /* draws per program */
    int vsync;      /* 0: swap interval 0 */
    int sync;       /* 1: wait for the GPU every frame (glFinish / dkQueueWaitIdle); 0: pipelined */
    int sweep;      /* 1: run the modes in turn at start, a SUMMARY line each */
    int sweep_secs; /* seconds a sweep step runs (the first 2 are not counted) */
    int pthread_core; /* GL: the core Mesa's threads are moved to (-1: left where libnx made them) */
    int mode;       /* index into dc_modes */
} DcConfig;

typedef struct
{
    const char* name;
    int binds;
} DcMode;

extern const DcMode dc_modes[];
extern const int dc_nmodes;
extern DcConfig dc_cfg;

/* the scene's data, the same for both paths */
void dc_mesh(int vb, DcVertex out[DC_NVERT]);
extern const uint16_t dc_indices[DC_NIDX];
void dc_texture(int t, uint32_t out[DC_TEX_SIZE * DC_TEX_SIZE]); /* RGBA8 */
void dc_uniforms(int draw, DcUniforms* out);

/* the program a draw uses: changes every prog_every draws */
static inline int dc_prog_of(int draw) { return (draw / (dc_cfg.prog_every > 0 ? dc_cfg.prog_every : 1)) % DC_NPROG; }

/* start: the config file beside the .nro (<dir>/drawcost.cfg; unknown keys become environment variables,
 * e.g. mesa_glthread=false), the log (<dir>/<log_name>), the clocks */
void dc_init(int argc, char** argv, const char* path_name, const char* log_name);
const char* dc_dir(void); /* the folder the log is in, ending in '/' */
void dc_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void dc_exit(void);

uint64_t dc_now_ns(void);

/* CPU time of the main thread and of the threads pthread_create made (Mesa's), in ns */
uint64_t dc_main_cpu_ns(void);
int dc_nthreads(void);
uint64_t dc_thread_cpu_ns(int i);
void dc_log_threads(void); /* the threads made since the last call: core, priority, moved */

/* per frame */
typedef struct
{
    uint64_t start_ns;   /* frame start (before the first draw) */
    uint64_t record_ns;  /* the draw loop's wall time (GL: the calls; deko3d: recording) */
    uint64_t submit_ns;  /* the above plus submitting (deko3d: finish list, submit, flush; GL: = record) */
    uint64_t done_ns;    /* frame start to the GPU done (sync only; 0 otherwise) */
    uint64_t present_ns; /* the swap / present call */
    int draws;
} DcFrame;

void dc_frame(const DcFrame* f);

/* input: returns 0 to quit; changes dc_cfg (and *changed when something the path must apply changed) */
int dc_input(int* vsync_changed);
