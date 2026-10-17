/* The scene effects' passes in WGSL (gfx_webgpu.c's scene_fx): FX_GLSL (gfx_vulkan.c) as WGSL, a
 * module per pass. Each pass names what it reads (FxSpec: t0..t5 as depth, filterable float or
 * unfilterable float textures, and whether it samples), and fx_wgsl builds its module from that: the
 * shared uniforms and helpers, its bindings, its fragment function. The shared vertex function draws one
 * triangle over the viewport with uv 0..1 top to bottom (as FX_GLSL's Vulkan clip space gives it).
 *
 * Not yet: the sun's maps in the occlusion pass (the contact shadows are here; sun_map is 1). */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FxSpec
{
    const char* name;
    const char* tex;  /* t0..t5: 'd' depth, 'f' float (filterable), 'u' float (unfilterable), '-' none */
    int sampler;      /* a filtering sampler at binding 7 */
    const char* body; /* the fragment function and its helpers */
} FxSpec;

static const char FX_WGSL_COMMON[] =
    "struct U {\n"
    "  proj: vec4f, zp: vec4f, vp: vec4f, size: vec4f, ao: vec4f, grade: vec4f, hand: vec4f, up: vec4f, sun: vec4f,\n"
    "  suncol: vec4f, sunuv: vec4f, fogc: vec4f, fogp: vec4f, bloom: vec4f, rays: vec4f, shadow: vec4f, lmat: mat4x4f,\n"
    "  smap: vec4f, smap2: vec4f, reproj: mat4x4f, hist: vec4f, lmatn: mat4x4f, smapn: vec4f, smapn2: vec4f, aop: vec4f,\n"
    "  dir: vec4i,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f, };\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> V {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  var o: V;\n"
    "  o.pos = vec4f(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = p;\n"
    "  return o;\n"
    "}\n"
    "const LUMA = vec3f(0.2126, 0.7152, 0.0722);\n"
    "fn view_z(d0: f32) -> f32 {\n"
    "  let d = (d0 - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  let z = u.zp.y / (d * u.hand.x - u.zp.x);\n"
    "  return select(z, 0.0, d >= 0.999999 || !(z * u.hand.x > 0.0));\n"
    "}\n"
    "fn view_pos(px0: vec2f, z: f32) -> vec3f {\n"
    "  let px = px0 - 0.5;\n"
    "  let ndc = vec2f((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  let w = z * u.hand.x;\n"
    "  return vec3f((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);\n"
    "}\n"
    "fn outside(q: vec2f) -> bool { return any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw); }\n";

/* helpers on the scene's depth when t0 is it */
#define FX_DEPTH0 \
    "fn depth_at(px: vec2f) -> f32 { return textureLoad(t0, vec2i(px), 0); }\n" \
    "fn pos_at(px0: vec2f) -> vec3f {\n" \
    "  var px = clamp(px0, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n" \
    "  px = floor(px) + 0.5;\n" \
    "  return view_pos(px, view_z(depth_at(px)));\n" \
    "}\n"

static const FxSpec FX_PASSES[] = {
    { "LINZ", "d-----", 0,
        FX_DEPTH0
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let px = floor(u.vp.xy + v.uv * u.vp.zw) + 0.5;\n"
        "  return vec4f(view_z(depth_at(px)), 0.0, 0.0, 0.0);\n"
        "}\n" },
    { "ZMIP", "u-----", 0,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let p = vec2i(v.pos.xy);\n"
        "  let hi = vec2i(textureDimensions(t0, 0)) - 1;\n"
        "  return textureLoad(t0, min(p * 2 + vec2i(p.y & 1, p.x & 1), hi), 0);\n"
        "}\n" },
    { "AO", "d--u--", 0,
        FX_DEPTH0
        "fn pos_lz(q: vec2f, r: f32) -> vec3f {\n"
        "  let t = (q - u.vp.xy) * u.size.zw / u.vp.zw;\n"
        "  let lv = clamp(i32(floor(log2(max(r * u.size.z / u.vp.z, 1.0)))) - 3, 0, 3);\n"
        "  let p = min(vec2i(max(t, vec2f(0.0))) >> vec2u(u32(lv)), vec2i(textureDimensions(t3, lv)) - 1);\n"
        "  return view_pos(q, textureLoad(t3, p, lv).r);\n"
        "}\n"
        "fn sun_shadow(P: vec3f, N: vec3f, dist: f32, k: f32) -> f32 {\n"
        "  let nl = dot(N, u.sun.xyz);\n"
        "  let fade = smoothstep(0.0, 0.15, nl) * (1.0 - smoothstep(0.6 * u.shadow.w, u.shadow.w, dist));\n"
        "  if (fade <= 0.0) { return 1.0; }\n"
        "  let O = P + N * (0.01 * dist);\n"
        "  for (var i = 0; i < 16; i++) {\n"
        "    let a = (f32(i) + k) / 16.0;\n"
        "    let R = O + u.sun.xyz * (u.shadow.y * a * a);\n"
        "    let rd = R.z * u.hand.x;\n"
        "    if (rd <= 0.05) { break; }\n"
        "    let ndc = vec2f(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / rd;\n"
        "    let q = u.vp.xy + vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
        "    if (outside(q) || outside(q + 0.5)) { break; }\n"
        "    let sd = view_z(depth_at(q + 0.5)) * u.hand.x;\n"
        "    let in_front = rd - sd;\n"
        "    if (sd > 0.0 && in_front > 0.005 * rd + 0.02 && in_front < u.shadow.z + 0.01 * rd) { return 1.0 - fade * (1.0 - a * a); }\n"
        "  }\n"
        "  return 1.0;\n"
        "}\n"
        "var<private> BAYER: array<i32, 16> = array<i32, 16>(0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let px = floor(u.vp.xy + v.uv * u.vp.zw) + 0.5;\n"
        "  let P = pos_at(px);\n"
        "  let dist = P.z * u.hand.x;\n"
        "  if (dist <= 0.0) { return vec4f(1.0, 0.0, 1.0, 1.0); }\n"
        "  let r = pos_at(px + vec2f(1.0, 0.0)) - P; let l = P - pos_at(px - vec2f(1.0, 0.0));\n"
        "  let d = pos_at(px + vec2f(0.0, 1.0)) - P; let t = P - pos_at(px - vec2f(0.0, 1.0));\n"
        "  let dx = select(l, r, (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0);\n"
        "  let dy = select(t, d, (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0);\n"
        "  let nc = cross(dx, dy);\n"
        "  var N = select(-normalize(P), normalize(nc), dot(nc, nc) > 1e-24);\n"
        "  if (dot(N, P) > 0.0) { N = -N; }\n"
        "  let cell = vec2i(v.pos.xy) & vec2i(3);\n"
        "  let k = fract((f32(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y);\n"
        "  var sh = 1.0;\n"
        "  if (u.shadow.x > 0.0 && u.sun.w > 0.0) { sh = sun_shadow(P, N, dist, k); }\n"
        "  let mp = 1.0;\n"
        "  let rad = u.ao.x;\n"
        "  let rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, u.ao.w);\n"
        "  if (u.ao.y <= 0.0 || rpx < 2.0) { return vec4f(1.0, dist, mp, sh); }\n"
        "  let NS = max(i32(u.aop.x), 1);\n"
        "  var sum = 0.0;\n"
        "  for (var i = 0; i < NS; i++) {\n"
        "    let a = (f32(i) + k) / f32(NS);\n"
        "    let ang = f32(i) * 2.3999632 + k * 6.2831853;\n"
        "    let q = px + vec2f(cos(ang), sin(ang)) * (a * rpx);\n"
        "    if (outside(q)) { continue; }\n"
        "    let Q = pos_lz(q, a * rpx);\n"
        "    if (Q.z == 0.0 || dist - Q.z * u.hand.x > 0.5 * rad) { continue; }\n"
        "    let w = Q - P;\n"
        "    let vv = dot(w, w); let vn = dot(w, N);\n"
        "    let q2 = vv / (rad * rad); let fall = clamp(1.0 - q2 * q2, 0.0, 1.0);\n"
        "    sum += fall * max(vn * inverseSqrt(vv + 1e-6) - u.ao.z, 0.0);\n"
        "  }\n"
        "  let facing = smoothstep(0.1, 0.4, dot(N, -P) / dist);\n"
        "  return vec4f(clamp(1.0 - 3.0 * facing * sum / f32(NS), 0.0, 1.0), dist, mp, sh);\n"
        "}\n" },
    { "BLUR", "f-----", 0,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let p = vec2i(v.pos.xy); let hi = vec2i(u.size.zw) - 1;\n"
        "  let c = textureLoad(t0, p, 0);\n"
        "  if (c.y <= 0.0) { return c; }\n"
        "  var s = c.x; var w = 1.0; var sw = 1.0; var ss = c.zw;\n"
        "  for (var i = -2; i <= 2; i++) {\n"
        "    if (i == 0) { continue; }\n"
        "    let t = textureLoad(t0, clamp(p + u.dir.xy * i, vec2i(0), hi), 0);\n"
        "    let k = select(1.0, 0.5, abs(i) == 2) * clamp(1.0 - abs(t.y - c.y) / (0.03 * c.y), 0.0, 1.0);\n"
        "    s += t.x * k; w += k;\n"
        "    if (abs(i) == 1 && u.smapn2.z == 0.0) { ss += t.zw * (0.5 * k); sw += 0.5 * k; }\n"
        "  }\n"
        "  return vec4f(s / w, c.y, ss / sw);\n"
        "}\n" },
    { "TEMPORAL", "ff----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let c = textureLoad(t0, vec2i(v.pos.xy), 0);\n"
        "  if (c.y <= 0.0 || u.hist.x == 0.0) { return c; }\n"
        "  let px = u.vp.xy + v.uv * u.vp.zw;\n"
        "  let P = view_pos(px, c.y * u.hand.x);\n"
        "  let pcl = u.reproj * vec4f(P, 1.0);\n"
        "  if (pcl.w <= 1e-4) { return c; }\n"
        "  let puv = vec2f(pcl.x / pcl.w * 0.5 + 0.5, 0.5 - pcl.y / pcl.w * 0.5);\n"
        "  if (any(puv < vec2f(0.0)) || any(puv > vec2f(1.0))) { return c; }\n"
        "  let h = textureSampleLevel(t1, smp, puv, 0.0);\n"
        "  if (!(h.y > 0.0) || abs(h.y - pcl.w) > 0.04 * pcl.w) { return c; }\n"
        "  let p = vec2i(v.pos.xy); let hi = vec2i(u.size.zw) - 1;\n"
        "  var lo = c.xzw; var up = c.xzw;\n"
        "  for (var dy = -1; dy <= 1; dy++) {\n"
        "    for (var dx = -1; dx <= 1; dx++) {\n"
        "      let t = textureLoad(t0, clamp(p + vec2i(dx, dy), vec2i(0), hi), 0);\n"
        "      if (t.y > 0.0 && abs(t.y - c.y) < 0.05 * c.y) { lo = min(lo, t.xzw); up = max(up, t.xzw); }\n"
        "    }\n"
        "  }\n"
        "  let give = vec3f(0.06, 0.02, 0.02);\n"
        "  let m = mix(c.xzw, clamp(h.xzw, lo - give, up + give), u.hist.z);\n"
        "  return vec4f(m.x, c.y, m.y, m.z);\n"
        "}\n" },
    { "BRIGHT", "ff----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let uv = (u.vp.xy + v.uv * u.vp.zw) / u.size.xy; let t = 1.0 / u.size.xy;\n"
        "  var c = 0.25 * (textureSampleLevel(t0, smp, uv + t * vec2f(-1.0, -1.0), 0.0).rgb + textureSampleLevel(t0, smp, uv + t * vec2f(1.0, -1.0), 0.0).rgb +\n"
        "                  textureSampleLevel(t0, smp, uv + t * vec2f(-1.0, 1.0), 0.0).rgb + textureSampleLevel(t0, smp, uv + t * vec2f(1.0, 1.0), 0.0).rgb);\n"
        "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
        "    let os = textureSampleLevel(t1, smp, v.uv, 0.0);\n"
        "    c *= mix(1.0, os.x, u.ao.y) * mix(1.0, os.z, u.smap.x) * mix(1.0, os.w, u.shadow.x);\n"
        "  }\n"
        "  let l = max(c.r, max(c.g, c.b)); let k = u.bloom.z;\n"
        "  var soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
        "  soft = soft * soft / (4.0 * k + 1e-5);\n"
        "  return vec4f(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
        "}\n" },
    { "FXAA", "f-----", 1,
        "var<private> FXAA_Q: array<f32, 10> = array<f32, 10>(1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
        "fn luma(p: vec2f) -> f32 { return dot(textureSampleLevel(t0, smp, p, 0.0).rgb, LUMA); }\n"
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let rcp = 1.0 / u.size.xy; let uv = v.pos.xy * rcp;\n"
        "  let c = textureSampleLevel(t0, smp, uv, 0.0).rgb;\n"
        "  let lm = dot(c, LUMA);\n"
        "  let ln = luma(uv + vec2f(0.0, -rcp.y)); let ls = luma(uv + vec2f(0.0, rcp.y));\n"
        "  let lw = luma(uv + vec2f(-rcp.x, 0.0)); let le = luma(uv + vec2f(rcp.x, 0.0));\n"
        "  let mx = max(lm, max(max(ln, ls), max(lw, le))); let mn = min(lm, min(min(ln, ls), min(lw, le))); let range = mx - mn;\n"
        "  if (range < max(0.0312, mx * 0.125)) { return vec4f(c, 1.0); }\n"
        "  let lnw = luma(uv + vec2f(-rcp.x, -rcp.y)); let lne = luma(uv + vec2f(rcp.x, -rcp.y));\n"
        "  let lsw = luma(uv + vec2f(-rcp.x, rcp.y)); let lse = luma(uv + vec2f(rcp.x, rcp.y));\n"
        "  let eh = abs(lnw + lne - 2.0 * ln) + 2.0 * abs(lw + le - 2.0 * lm) + abs(lsw + lse - 2.0 * ls);\n"
        "  let ev = abs(lnw + lsw - 2.0 * lw) + 2.0 * abs(ln + ls - 2.0 * lm) + abs(lne + lse - 2.0 * le);\n"
        "  let horz = eh >= ev;\n"
        "  let l1 = select(lw, ln, horz); let l2 = select(le, ls, horz); let g1 = abs(l1 - lm); let g2 = abs(l2 - lm);\n"
        "  var stp = select(rcp.x, rcp.y, horz); var lavg: f32; var grad: f32;\n"
        "  if (g1 >= g2) { stp = -stp; lavg = 0.5 * (l1 + lm); grad = g1; } else { lavg = 0.5 * (l2 + lm); grad = g2; }\n"
        "  var e = uv; let along = select(vec2f(0.0, rcp.y), vec2f(rcp.x, 0.0), horz);\n"
        "  if (horz) { e.y += stp * 0.5; } else { e.x += stp * 0.5; }\n"
        "  var p1 = e - along; var p2 = e + along;\n"
        "  var d1 = luma(p1) - lavg; var d2 = luma(p2) - lavg;\n"
        "  var r1 = abs(d1) >= grad * 0.25; var r2 = abs(d2) >= grad * 0.25;\n"
        "  for (var i = 0; i < 10 && !(r1 && r2); i++) {\n"
        "    if (!r1) { p1 -= along * FXAA_Q[i]; d1 = luma(p1) - lavg; r1 = abs(d1) >= grad * 0.25; }\n"
        "    if (!r2) { p2 += along * FXAA_Q[i]; d2 = luma(p2) - lavg; r2 = abs(d2) >= grad * 0.25; }\n"
        "  }\n"
        "  let dist1 = select(uv.y - p1.y, uv.x - p1.x, horz); let dist2 = select(p2.y - uv.y, p2.x - uv.x, horz);\n"
        "  let near1 = dist1 < dist2;\n"
        "  let dmin = min(dist1, dist2); let len = dist1 + dist2;\n"
        "  let mid_lower = lm < lavg; let good = (select(d2, d1, near1) < 0.0) != mid_lower;\n"
        "  var off = select(0.0, -dmin / len + 0.5, good);\n"
        "  let avg = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;\n"
        "  var sub = clamp(abs(avg - lm) / range, 0.0, 1.0);\n"
        "  sub = (-2.0 * sub + 3.0) * sub * sub;\n"
        "  off = max(off, sub * sub * 0.75);\n"
        "  var f = uv;\n"
        "  if (horz) { f.y += off * stp; } else { f.x += off * stp; }\n"
        "  return vec4f(textureSampleLevel(t0, smp, f, 0.0).rgb, 1.0);\n"
        "}\n" },
    { "DOWN", "f-----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let tx = 1.0 / vec2f(textureDimensions(t0, 0));\n"
        "  return 0.25 * (textureSampleLevel(t0, smp, v.uv + tx * vec2f(-1.0, -1.0), 0.0) + textureSampleLevel(t0, smp, v.uv + tx * vec2f(1.0, -1.0), 0.0) +\n"
        "                 textureSampleLevel(t0, smp, v.uv + tx * vec2f(-1.0, 1.0), 0.0) + textureSampleLevel(t0, smp, v.uv + tx * vec2f(1.0, 1.0), 0.0));\n"
        "}\n" },
    { "GAUSS", "f-----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let tx = vec2f(u.dir.xy) / vec2f(textureDimensions(t0, 0));\n"
        "  var c = textureSampleLevel(t0, smp, v.uv, 0.0) * 0.2270270;\n"
        "  c += (textureSampleLevel(t0, smp, v.uv + tx * 1.3846154, 0.0) + textureSampleLevel(t0, smp, v.uv - tx * 1.3846154, 0.0)) * 0.3162162;\n"
        "  c += (textureSampleLevel(t0, smp, v.uv + tx * 3.2307692, 0.0) + textureSampleLevel(t0, smp, v.uv - tx * 3.2307692, 0.0)) * 0.0702703;\n"
        "  return c;\n"
        "}\n" },
    { "RAYMASK", "fd----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let px = floor(u.vp.xy + v.uv * u.vp.zw) + 0.5;\n"
        "  if (view_z(textureLoad(t1, vec2i(px), 0)) != 0.0) { return vec4f(0.0); }\n"
        "  let c = textureSampleLevel(t0, smp, px / u.size.xy, 0.0).rgb;\n"
        "  let d = (v.uv - u.sunuv.xy) * vec2f(u.proj.y / u.proj.x, 1.0);\n"
        "  let glow = clamp(1.0 - length(d) / 0.6, 0.0, 1.0);\n"
        "  return vec4f(c * smoothstep(0.35, 0.9, dot(c, LUMA)) * glow * glow, 1.0);\n"
        "}\n" },
    { "RAYS", "f-----", 1,
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  var uv = v.uv; let stp = (v.uv - u.sunuv.xy) * (u.rays.z / 64.0);\n"
        "  var acc = vec3f(0.0); var w = 1.0;\n"
        "  for (var i = 0; i < 64; i++) { acc += textureSampleLevel(t0, smp, uv, 0.0).rgb * w; w *= u.rays.y; uv -= stp; }\n"
        "  return vec4f(acc * (4.0 / 64.0), 1.0);\n"
        "}\n" },
    { "COMP", "ffdfff", 1,
        "fn ao_at(uv: vec2f, dist: f32) -> vec3f {\n"
        "  if (dist <= 0.0) { return vec3f(1.0); }\n"
        "  let g = uv * u.size.zw - 0.5; let f = fract(g);\n"
        "  let i0 = vec2i(floor(g)); let hi = vec2i(u.size.zw) - 1;\n"
        "  var s = vec3f(0.0); var w = 0.0;\n"
        "  for (var k = 0; k < 4; k++) {\n"
        "    let o = vec2i(k & 1, k >> 1u);\n"
        "    let t = textureLoad(t1, clamp(i0 + o, vec2i(0), hi), 0);\n"
        "    let bw = select(1.0 - f.x, f.x, o.x != 0) * select(1.0 - f.y, f.y, o.y != 0);\n"
        "    let dw = select(1e-3, 1.0 / (1e-3 + abs(t.y - dist) / dist), t.y > 0.0);\n"
        "    s += t.xzw * bw * dw; w += bw * dw;\n"
        "  }\n"
        "  return select(vec3f(1.0), s / w, w > 0.0);\n"
        "}\n"
        "fn screen(a: vec3f, b: vec3f) -> vec3f { return 1.0 - (1.0 - clamp(a, vec3f(0.0), vec3f(1.0))) * (1.0 - clamp(b, vec3f(0.0), vec3f(1.0))); }\n"
        "@fragment fn fs(v: V) -> @location(0) vec4f {\n"
        "  let px = v.pos.xy;\n"
        "  var c = textureLoad(t0, vec2i(px), 0);\n"
        "  let dbg = i32(u.grade.w);\n"
        "  var os = vec3f(1.0);\n"
        "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) { os = ao_at(v.uv, view_z(textureLoad(t2, vec2i(px), 0)) * u.hand.x); }\n"
        "  let o = os.x; let sun = mix(1.0, os.y, u.smap.x) * mix(1.0, os.z, u.shadow.x);\n"
        "  if (dbg == 1) { return vec4f(o, o, o, c.a); }\n"
        "  if (dbg == 5) { return vec4f(vec3f(sun), c.a); }\n"
        "  c = vec4f(c.rgb * (mix(1.0, o, u.ao.y) * sun), c.a);\n"
        "  var f = 0.0;\n"
        "  if (u.fogc.a > 0.0) {\n"
        "    let z = view_z(textureLoad(t2, vec2i(px), 0));\n"
        "    if (z != 0.0) {\n"
        "      let P = view_pos(px, z);\n"
        "      let d = length(P); let bd = u.fogp.x * dot(P, u.up.xyz);\n"
        "      let k = select(1.0, (1.0 - exp(-bd)) / bd, abs(bd) > 1e-4);\n"
        "      f = min(1.0 - exp(-u.fogc.a * d * k), u.fogp.y);\n"
        "      let g = u.fogp.w; let cs = dot(P / max(d, 1e-5), u.sun.xyz);\n"
        "      let sunk = u.sun.w * u.fogp.z * pow((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cs, 1e-5), 1.5);\n"
        "      c = vec4f(mix(c.rgb, u.fogc.rgb + u.suncol.rgb * sunk, f), c.a);\n"
        "    }\n"
        "  }\n"
        "  if (dbg == 2) { return vec4f(f, f, f, c.a); }\n"
        "  var add = vec3f(0.0);\n"
        "  if (u.bloom.y > 0.0) {\n"
        "    let bl = textureSampleLevel(t3, smp, v.uv, 0.0).rgb * 0.6 + textureSampleLevel(t4, smp, v.uv, 0.0).rgb * 0.8;\n"
        "    if (dbg == 3) { return vec4f(bl, c.a); }\n"
        "    add += bl * u.bloom.y;\n"
        "  }\n"
        "  if (u.rays.x > 0.0 && u.sunuv.z > 0.0) {\n"
        "    let r = textureSampleLevel(t5, smp, v.uv, 0.0).rgb * u.suncol.rgb * u.sunuv.z;\n"
        "    if (dbg == 4) { return vec4f(r, c.a); }\n"
        "    add += r * u.rays.x;\n"
        "  }\n"
        "  let sc = screen(c.rgb, add);\n"
        "  var x = mix(vec3f(dot(sc, LUMA)), sc, u.grade.y);\n"
        "  x = clamp(x, vec3f(0.0), vec3f(1.0));\n"
        "  x = mix(x, x * x * (3.0 - 2.0 * x), u.grade.z);\n"
        "  return vec4f(mix(sc, x, u.grade.x), c.a);\n"
        "}\n" },
};

enum { FX_LINZ, FX_ZMIP, FX_AO, FX_BLUR, FX_TEMPORAL, FX_BRIGHT, FX_FXAA, FX_DOWN, FX_GAUSS, FX_RAYMASK, FX_RAYS, FX_COMP, FX_N };

/* a pass's whole module (malloc'd) */
static char* fx_wgsl(const FxSpec* p)
{
    size_t n = sizeof FX_WGSL_COMMON + strlen(p->body) + 1024;
    char* s = (char*)malloc(n);
    size_t o = (size_t)snprintf(s, n, "%s", FX_WGSL_COMMON);
    for (int i = 0; i < 6; ++i)
    {
        char c = p->tex[i];
        if (c == 'd')
            o += (size_t)snprintf(s + o, n - o, "@group(0) @binding(%d) var t%d: texture_depth_2d;\n", 1 + i, i);
        else if (c == 'f' || c == 'u')
            o += (size_t)snprintf(s + o, n - o, "@group(0) @binding(%d) var t%d: texture_2d<f32>;\n", 1 + i, i);
    }
    if (p->sampler)
        o += (size_t)snprintf(s + o, n - o, "@group(0) @binding(7) var smp: sampler;\n");
    snprintf(s + o, n - o, "%s", p->body);
    return s;
}
