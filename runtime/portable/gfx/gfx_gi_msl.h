/* The scene renderer's bounce light: the shading's side of the probes (gfx_gi_metal.inc says what they are), taken by
 * the zone's and characters' shaders and the probes' own kernels. Included by gfx_metal.m ahead of those shaders. */
#pragma once

/* the shading's side, in the zone's and characters' libraries and the kernels': the probes at P (normal N): rgb the
 * bounce light (not scaled by strength), a what the ambient is multiplied by (1 outside the grid) */
#define GI_MSL                                                                                                        \
    "struct GU { float4 org; int4 base; int4 dims; float4 k; float4 inv; float4 tr; uint4 n; float4 sun_dir, sun_col,\n" \
    "            sun2_dir, sun2_col, amb; float4x4 rot; };\n"                                                         \
    "static float gi_sgn(float v) { return v >= 0.0 ? 1.0 : -1.0; }\n"                                                \
    "static float2 gi_oct(float3 n) {\n"                                                                             \
    "  n /= abs(n.x) + abs(n.y) + abs(n.z);\n"                                                                       \
    "  return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * float2(gi_sgn(n.x), gi_sgn(n.y));\n"                         \
    "}\n"                                                                                                            \
    "static float3 gi_unoct(float2 e) {\n"                                                                           \
    "  float3 v = float3(e, 1.0 - abs(e.x) - abs(e.y));\n"                                                           \
    "  if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * float2(gi_sgn(v.x), gi_sgn(v.y));\n"                               \
    "  return normalize(v);\n"                                                                                       \
    "}\n"                                                                                                            \
    "static int3 gi_slot(constant GU& G, int3 c) { return ((c % G.dims.xyz) + G.dims.xyz) % G.dims.xyz; }\n"         \
    "static uint gi_index(constant GU& G, int3 s) { return uint(s.x + G.dims.x * (s.y + G.dims.y * s.z)); }\n"     \
    /* a probe's map (n texels inside a border of one) at direction d, in the atlas's uv */                          \
    "static float2 gi_uv(constant GU& G, int3 s, float3 d, float n, float2 inv) {\n"                                \
    "  float2 t = float2(float(s.x + G.dims.x * s.y), float(s.z)) * (n + 2.0) + 1.0 + (gi_oct(d) * 0.5 + 0.5) * n;\n" \
    "  return t * inv;\n"                                                                                            \
    "}\n"                                                                                                            \
    "static float4 gi_at(constant GU& G, const device uint* PS, texture2d<float> ir, texture2d<float> ds, float3 P,\n" \
    "                    float3 N) {\n"                                                                              \
    "  constexpr sampler ls(coord::normalized, filter::linear, address::clamp_to_edge);\n"                           \
    "  float sp = G.org.w;\n"                                                                                        \
    "  float3 Pb = P + N * G.k.w;\n" /* off the surface, so its own probes' near hits don't shade it */              \
    "  float3 g = Pb / sp, fl = floor(g), f = g - fl;\n"                                                             \
    "  int3 i0 = int3(fl);\n"                                                                                        \
    "  float3 sum = float3(0.0);\n"                                                                                  \
    "  float sky = 0.0, ws = 0.0;\n"                                                                                 \
    "  for (int c = 0; c < 8; ++c) {\n"                                                                              \
    "    int3 o = int3(c & 1, (c >> 1) & 1, c >> 2), pc = i0 + o, rel = pc - G.base.xyz;\n"                          \
    "    if (any(rel < 0) || any(rel >= G.dims.xyz)) continue;\n"                                                    \
    "    int3 s = gi_slot(G, pc);\n"                                                                                 \
    "    if ((PS[gi_index(G, s)] & 3u) != 1u) continue;\n" /* traced, and not inside something */                  \
    "    float3 tw = mix(1.0 - f, f, float3(o));\n"                                                                  \
    "    float w = tw.x * tw.y * tw.z;\n"                                                                            \
    "    float3 dv = float3(pc) * sp - Pb;\n"                                                                        \
    "    float dl = length(dv);\n"                                                                                   \
    "    float3 dn = dl > 1e-4 ? dv / dl : N;\n"                                                                     \
    "    float wb = (dot(dn, N) + 1.0) * 0.5;\n" /* probes behind the surface count less */                         \
    "    w *= wb * wb + 0.2;\n"                                                                                      \
    "    float2 m = ds.sample(ls, gi_uv(G, s, -dn, 14.0, G.inv.zw), level(0)).rg;\n"                                \
    "    if (dl > m.x) {\n" /* the probe sees something nearer than this point that way: it is likely hidden */    \
    "      float v = abs(m.y - m.x * m.x), t = dl - m.x, ch = v / (v + t * t);\n"                                    \
    "      w *= max(ch * ch * ch, 0.05);\n"                                                                          \
    "    }\n"                                                                                                        \
    "    if (w < 0.2) w *= w * w * 25.0;\n"                                                                          \
    "    float4 e = ir.sample(ls, gi_uv(G, s, N, 6.0, G.inv.xy), level(0));\n"                                     \
    "    sum += e.rgb * w;\n"                                                                                        \
    "    sky += e.a * w;\n"                                                                                          \
    "    ws += w;\n"                                                                                                 \
    "  }\n"                                                                                                          \
    "  if (ws <= 1e-5) return float4(0.0, 0.0, 0.0, 1.0);\n"                                                        \
    "  float3 r = g - float3(G.base.xyz), e3 = min(r, float3(G.dims.xyz - 1) - r);\n"                                 \
    "  float fade = saturate(min(e3.x, min(e3.y, e3.z)) * 0.5);\n" /* the grid's last two cells fade out */          \
    "  return float4(sum / ws * fade, mix(1.0, mix(1.0, sky / ws, G.k.y), fade));\n"                                  \
    "}\n"

