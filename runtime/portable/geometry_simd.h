/* The game's rigid and weighted vertex transforms (position and normal) as ARM64 NEON or x86 SSE2
 * kernels, in the original SSE operation order. Plain C over byte spans: no guest addresses, build
 * metadata or graphics API. geometry_guest.c adapts them to the translated game. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the host FP environment gives the kernels SSE semantics: round to nearest, gradual
 * underflow, NaN payloads kept and exceptions masked. Reads the control register without changing
 * it; arithmetic may still set status bits. The caller checks once per batch, while it owns the
 * input and output memory; when this is 0 it runs its original geometry path instead. */
int geometry_simd_supported(void);

/* Each matrix is 16 column-major binary32 values (64 bytes). Outputs are XYZ binary32 values
 * (12 bytes each). Addresses need not be aligned. Every input is read before either output is
 * written, the NaN path included, so any of them may alias. Position is written first, then normal.
 *
 * The original SSE operation order, rounded to binary32 at each step, with SSE's NaN selection.
 * Not the translated x87 path's intermediate precision: this is a different result from that path,
 * and must not replace it unannounced.
 *
 * source: position XYZ, then normal XYZ (24 bytes).
 * position = ((x*M0 + y*M1) + z*M2) + M3
 * normal   =  (nx*M0 + ny*M1) + nz*M2
 */
void geometry_simd_rigid(const void* matrix, const void* source, void* position, void* normal);

/* source: xA,xB,yA,yB,zA,zB,wA,wB,nxA,nxB,nyA,nyB,nzA,nzB (56 bytes), already weighted by the
 * caller. Sums A's four position and three normal terms, then adds B's terms one at a time, in
 * order; no FMA, no reassociation. Matrices and outputs as for geometry_simd_rigid. */
void geometry_simd_weighted(const void* matrix_a, const void* matrix_b, const void* source, void* position,
                            void* normal);

#ifdef __cplusplus
}
#endif
