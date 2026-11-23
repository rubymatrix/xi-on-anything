/* The scene renderer's ambient occlusion: the shading's side (gfx_ao_metal.inc says how it is made), taken by the
 * zone's and characters' shaders. Included by gfx_metal.m ahead of those shaders. */
#pragma once

/* AU: the occlusion as last made. pvp: the view and projection it was made with (D3D row-major, so pvp * p in MSL is
 * D3D's p * VP); vp: its viewport's size in pixels (x, y) and the share of the texture it fills (z, w); k: the zone's
 * strength, the characters', debug, the depth test's tolerance.
 * ao_at: the occlusion at a point whose place last frame is pc (pvp * its world position): a bilinear gather of the
 * half-size texture (x the occlusion, y the distance it was made at), each tap weighted by how near its distance is to
 * the point's (so a character's occlusion stays off the ground behind it, and a place new to the view gets none); 1
 * where nothing matches. */
#define AO_MSL                                                                                                        \
    "struct AU { float4x4 pvp; float4 vp; float4 k; };\n"                                                             \
    "static float ao_at(constant AU& A, texture2d<float> ao, float4 pc) {\n"                                          \
    "  if (pc.w <= 1e-4) return 1.0;\n"                                                                              \
    "  float2 uv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"                                     \
    "  if (any(uv <= 0.0) || any(uv >= 1.0)) return 1.0;\n"                                                         \
    "  float2 tc = uv * A.vp.zw;\n"                                                                                  \
    "  float2 sz = float2(ao.get_width(), ao.get_height()), t = tc * sz - 0.5, f = fract(t);\n"                      \
    "  constexpr sampler ns(coord::normalized, filter::nearest, address::clamp_to_edge);\n"                          \
    "  float2 g = (floor(t) + 1.0) / sz;\n"                                                                         \
    "  float4 o = ao.gather(ns, g, int2(0), component::x), d = ao.gather(ns, g, int2(0), component::y);\n"          \
    /* gather's order: (0,1) (1,1) (1,0) (0,0) */                                                                    \
    "  float4 w = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));\n"            \
    "  w *= saturate(1.0 - abs(d - pc.w) / (A.k.w * pc.w));\n"                                                      \
    "  float ws = w.x + w.y + w.z + w.w;\n"                                                                          \
    "  return ws > 1e-3 ? dot(o, w) / ws : 1.0;\n"                                                                   \
    "}\n"
