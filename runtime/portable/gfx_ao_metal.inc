/* --- the scene renderer's ambient occlusion (the scene_ao setting) ------------------------------------------------
 * Included by gfx_metal.m after the bounce light (gfx_gi_metal.inc: the zone's acceleration structure, which the rays
 * trace, and gi_ts_mark); AO_MSL, the shading's side, is gfx_ao_msl.h.
 *
 * Made once a frame at half size, when the scene is done (gfx_scene_done, ahead of the post effects), from the depth
 * the zone and characters were drawn with - so it holds both, and a character darkens the ground at its feet:
 *   ao_prep     the depth at half size: the nearest of each 2x2, as distance along the view (0 the sky);
 *   ao_gtao     ground-truth AO (Jimenez et al. 2016, as XeGTAO): per pixel, slices round the view, the horizon each
 *               way found within scene_ao_radius world units, the cosine-weighted visible arc between them; or
 *   ao_rays     short rays into the hemisphere about the normal, against the zone's structure (static geometry at its
 *               coarsest models; characters are not in it), each hit within the reach occluding by how near it is;
 *   ao_resolve  a 3x3 blur that keeps to its own depth, then blended over frames with the last result reprojected to
 *               this frame (each pixel's own count, so a place new to the view converges at once); the result keeps its
 *               distance beside it.
 * The noise turns each frame, so the frames' samples differ and the blend gathers them.
 *
 * The zone and characters are drawn before it is made, so they read the last frame's result, at their place in it
 * (AO_MSL ao_at: their position through that frame's view and projection, a gather that keeps to their distance): it
 * scales only the ambient term (the game's ambient, the lamps' ambient and the bounce light), never the sun, the moon
 * or the lamps' direct light. A place the last frame did not see takes none for that frame. */

static const char AO_KERNELS_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace metal::raytracing;\n"
    /* vp: the viewport in the target (x, y, w, h); hs: the half size and its inverse; pr: P00, P11, P20, P21 (D3D's
     * projection, row-major); pz: P22, P32, P23 (+-1), the depth's far end; prm: the reach, steps, slices or rays, the
     * reach's largest size in half pixels; tp: the blend's least weight, whether there is a last result, the frame, the
     * frames it counts at most */
    "struct AoU { float4 vp, hs, pr, pz, prm, tp; float4x4 iv, pvp; };\n"
    "static float ao_ign(float2 p) { return fract(52.9829189 * fract(dot(p, float2(0.06711056, 0.00583715)))); }\n"
    "static float ao_acos(float x) {\n" /* XeGTAO's, within some 0.001 */
    "  float r = (-0.156583 * abs(x) + 1.5707963) * sqrt(saturate(1.0 - abs(x)));\n"
    "  return x >= 0.0 ? r : 3.14159265 - r;\n"
    "}\n"
    /* half pixel h (its corner) at distance z, in the view: x right, y up, z the distance */
    "static float3 ao_vpos(constant AoU& U, float2 h, float z) {\n"
    "  float2 ndc = float2((h.x + 0.5) * 2.0 * U.hs.z - 1.0, 1.0 - (h.y + 0.5) * 2.0 * U.hs.w);\n"
    "  float vz = z * U.pz.z;\n"
    "  return float3((ndc.x * z - vz * U.pr.z) / U.pr.x, (ndc.y * z - vz * U.pr.w) / U.pr.y, z);\n"
    "}\n"
    "static float3 ao_world(constant AoU& U, float3 P) { return (U.iv * float4(P.x, P.y, P.z * U.pz.z, 1.0)).xyz; }\n"
    /* the normal from the neighbours, each way the side whose depth is nearer this one's (edges stay sharp); toward
     * the eye */
    "static float3 ao_normal(constant AoU& U, texture2d<float> hz, int2 p, float3 P) {\n"
    "  int2 hi = int2(U.hs.xy) - 1;\n"
    "  float zl = hz.read(uint2(clamp(p - int2(1, 0), int2(0), hi))).x, zr = hz.read(uint2(clamp(p + int2(1, 0), int2(0), hi))).x;\n"
    "  float zu = hz.read(uint2(clamp(p - int2(0, 1), int2(0), hi))).x, zd = hz.read(uint2(clamp(p + int2(0, 1), int2(0), hi))).x;\n"
    "  bool r = zr > 0.0 && (zl <= 0.0 || abs(zr - P.z) < abs(zl - P.z));\n"
    "  bool d = zd > 0.0 && (zu <= 0.0 || abs(zd - P.z) < abs(zu - P.z));\n"
    "  float2 h = float2(p);\n"
    "  float3 dx = r ? ao_vpos(U, h + float2(1, 0), zr) - P : (zl > 0.0 ? P - ao_vpos(U, h - float2(1, 0), zl) : float3(1, 0, 0));\n"
    "  float3 dy = d ? ao_vpos(U, h + float2(0, 1), zd) - P : (zu > 0.0 ? P - ao_vpos(U, h - float2(0, 1), zu) : float3(0, 1, 0));\n"
    "  float3 n = normalize(cross(dx, dy));\n"
    "  return dot(n, P) > 0.0 ? -n : n;\n"
    "}\n"
    "kernel void ao_prep(constant AoU& U [[buffer(0)]], depth2d<float> dt [[texture(0)]],\n"
    "                    texture2d<float, access::write> hz [[texture(1)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= uint(U.hs.x) || p.y >= uint(U.hs.y)) return;\n"
    "  uint2 b = uint2(U.vp.xy + floor(float2(p) * U.vp.zw * U.hs.zw)), hi = uint2(dt.get_width(), dt.get_height()) - 1u;\n"
    "  float z = 1e30;\n"
    "  for (uint k = 0; k < 4u; ++k) {\n"
    "    float d = dt.read(min(b + uint2(k & 1u, k >> 1), hi));\n"
    "    if (d >= U.pz.w) continue;\n" /* the sky, or nothing drawn */
    "    float vz = U.pz.y / (d * U.pz.z - U.pz.x);\n"
    "    z = min(z, vz * U.pz.z);\n"
    "  }\n"
    "  hz.write(float4(z < 1e29 && z > 0.0 ? z : 0.0), p);\n"
    "}\n"
    "kernel void ao_gtao(constant AoU& U [[buffer(0)]], texture2d<float> hz [[texture(0)]],\n"
    "                    texture2d<float, access::write> out [[texture(1)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= uint(U.hs.x) || p.y >= uint(U.hs.y)) return;\n"
    "  float z = hz.read(p).x;\n"
    /* the reach in texels each way (the texels need not be square on screen), held to prm.w texels near the eye */
    "  float R = U.prm.x;\n"
    "  float2 rpx = R / max(z, 1e-3) * float2(U.pr.x, U.pr.y) * 0.5 * U.hs.xy;\n"
    "  float big = max(rpx.x, rpx.y);\n"
    "  if (z <= 0.0 || big < 1.0) { out.write(float4(1.0), p); return; }\n"
    "  if (big > U.prm.w) { float k = U.prm.w / big; rpx *= k, R *= k; }\n"
    "  float2 px1 = rpx / max(rpx.x, rpx.y);\n" /* a texel's step, the long way */
    "  float3 P = ao_vpos(U, float2(p), z), N = ao_normal(U, hz, int2(p), P), V = normalize(-P);\n"
    "  int steps = int(U.prm.y), slices = int(U.prm.z);\n"
    "  float fr = U.tp.z, n0 = ao_ign(float2(p) + 5.588238 * fr), n1 = ao_ign(float2(p) + float2(37.0, 17.0) + 5.588238 * fr);\n"
    "  float2 hi = U.hs.xy - 1.0;\n"
    "  float vis = 0.0;\n"
    "  for (int s = 0; s < slices; ++s) {\n"
    "    float phi = (float(s) + n0) * (3.14159265 / float(slices));\n"
    "    float2 om = float2(cos(phi), -sin(phi));\n"
    "    float3 dv = float3(cos(phi), sin(phi), 0.0), od = dv - dot(dv, V) * V, ax = normalize(cross(od, V));\n"
    "    float3 pn = N - ax * dot(N, ax);\n"
    "    float pl = length(pn);\n"
    "    float cn = saturate(dot(pn, V) / max(pl, 1e-4)), n = (dot(od, pn) >= 0.0 ? 1.0 : -1.0) * ao_acos(cn);\n"
    "    float lo0 = cos(n + 1.5707963), lo1 = cos(n - 1.5707963), h0c = lo0, h1c = lo1;\n"
    "    for (int j = 0; j < steps; ++j) {\n"
    "      float t = (float(j) + fract(n1 + float(j) * 0.618034)) / float(steps);\n"
    "      float2 off = om * (t * t * rpx + px1);\n"
    "      for (int side = 0; side < 2; ++side) {\n"
    "        float2 q = float2(p) + (side == 0 ? off : -off);\n"
    "        if (any(q < 0.0) || any(q > hi)) continue;\n"
    "        float zq = hz.read(uint2(q + 0.5)).x;\n"
    "        if (zq <= 0.0) continue;\n"
    "        float3 dd = ao_vpos(U, floor(q + 0.5), zq) - P;\n"
    "        float l = length(dd);\n"
    "        if (l < 1e-4) continue;\n"
    "        float w = saturate((R - l) / (0.4 * R));\n"
    "        float c = dot(dd, V) / l;\n"
    "        if (side == 0) h0c = max(h0c, mix(lo0, c, w)); else h1c = max(h1c, mix(lo1, c, w));\n"
    "      }\n"
    "    }\n"
    "    float h0 = -ao_acos(clamp(h1c, -1.0, 1.0)), h1 = ao_acos(clamp(h0c, -1.0, 1.0));\n"
    "    h0 = n + clamp(h0 - n, -1.5707963, 1.5707963), h1 = n + clamp(h1 - n, -1.5707963, 1.5707963);\n"
    "    float sn = sin(n);\n"
    "    vis += pl * ((cn + 2.0 * h0 * sn - cos(2.0 * h0 - n)) + (cn + 2.0 * h1 * sn - cos(2.0 * h1 - n))) * 0.25;\n"
    "  }\n"
    "  out.write(float4(saturate(vis / float(slices))), p);\n"
    "}\n"
    /* the zone's triangles' colours and normals (gfx_gi_metal.inc gi_flatten): x the albedo and two-sidedness, y the
     * normal (octahedral) */
    "static float3 ao_unoct(float2 e) {\n"
    "  float3 v = float3(e, 1.0 - abs(e.x) - abs(e.y));\n"
    "  if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);\n"
    "  return normalize(v);\n"
    "}\n"
    "kernel void ao_rays(constant AoU& U [[buffer(0)]], texture2d<float> hz [[texture(0)]],\n"
    "                    texture2d<float, access::write> out [[texture(1)]], primitive_acceleration_structure as [[buffer(1)]],\n"
    "                    device const uint2* attr [[buffer(2)]], uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= uint(U.hs.x) || p.y >= uint(U.hs.y)) return;\n"
    "  float z = hz.read(p).x;\n"
    "  if (z <= 0.0) { out.write(float4(1.0), p); return; }\n"
    "  float3 P = ao_vpos(U, float2(p), z), N = ao_normal(U, hz, int2(p), P);\n"
    "  float3 W = ao_world(U, P), Nw = normalize((U.iv * float4(N.x, N.y, N.z * U.pz.z, 0.0)).xyz);\n"
    "  float3 T = normalize(abs(Nw.y) < 0.99 ? cross(Nw, float3(0, 1, 0)) : cross(Nw, float3(1, 0, 0))), B = cross(Nw, T);\n"
    "  float3 o = W + Nw * (0.03 + 0.002 * z);\n"
    "  float R = U.prm.x, fr = U.tp.z;\n"
    "  float n0 = ao_ign(float2(p) + 5.588238 * fr), n1 = ao_ign(float2(p) + float2(37.0, 17.0) + 5.588238 * fr);\n"
    "  intersector<triangle_data> is;\n"
    "  is.assume_geometry_type(geometry_type::triangle);\n"
    "  is.force_opacity(forced_opacity::opaque);\n"
    "  int nr = int(U.prm.z);\n"
    "  float occ = 0.0;\n"
    "  for (int r = 0; r < nr; ++r) {\n"
    "    float u1 = fract(n0 + float(r) / float(nr)), u2 = fract(n1 + float(r) * 0.618034);\n" /* cosine-weighted */
    "    float sr = sqrt(u1), ph = 6.2831853 * u2;\n"
    "    float3 d = normalize(T * (sr * cos(ph)) + B * (sr * sin(ph)) + Nw * sqrt(max(1.0 - u1, 0.0)));\n"
    "    ray ry(o, d, 0.0, R);\n"
    "    auto h = is.intersect(ry, as);\n"
    "    if (h.type == intersection_type::none) continue;\n"
    "    uint2 at = attr[h.primitive_id];\n"
    "    float3 hn = ao_unoct(unpack_unorm2x16_to_float(at.y) * 2.0 - 1.0);\n"
    "    if (dot(hn, d) > 0.0 && (at.x >> 24) == 0u) continue;\n" /* a backface: the coarse model under the drawn one */
    "    occ += 1.0 - h.distance / R;\n"
    "  }\n"
    "  out.write(float4(saturate(1.0 - occ / float(nr))), p);\n"
    "}\n"
    "kernel void ao_resolve(constant AoU& U [[buffer(0)]], texture2d<float> hz [[texture(0)]], texture2d<float> raw [[texture(1)]],\n"
    "                       texture2d<float> hist [[texture(2)]], texture2d<float, access::write> out [[texture(3)]],\n"
    "                       uint2 p [[thread_position_in_grid]]) {\n"
    "  if (p.x >= uint(U.hs.x) || p.y >= uint(U.hs.y)) return;\n"
    "  float z = hz.read(p).x;\n"
    "  if (z <= 0.0) { out.write(float4(1.0, 0.0, 0.0, 0.0), p); return; }\n"
    "  int2 hi = int2(U.hs.xy) - 1;\n"
    "  float s = 0.0, ws = 0.0;\n"
    "  for (int y = -1; y <= 1; ++y)\n"
    "    for (int x = -1; x <= 1; ++x) {\n"
    "      uint2 q = uint2(clamp(int2(p) + int2(x, y), int2(0), hi));\n"
    "      float zq = hz.read(q).x;\n"
    "      float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) * saturate(1.0 - abs(zq - z) / (0.03 * z));\n"
    "      s += raw.read(q).x * w, ws += w;\n"
    "    }\n"
    "  s = ws > 0.0 ? s / ws : raw.read(p).x;\n"
    "  float a = s, cnt = 1.0;\n"
    "  if (U.tp.y > 0.5) {\n"
    "    float4 pc = U.pvp * float4(ao_world(U, ao_vpos(U, float2(p), z)), 1.0);\n"
    "    if (pc.w > 1e-4) {\n"
    "      float2 uv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "      float2 t = uv * U.hs.xy - 0.5, f = fract(t);\n"
    "      int2 b = int2(floor(t));\n"
    "      float4 hv = float4(0.0);\n"
    "      float hw = 0.0;\n"
    "      for (int k = 0; k < 4; ++k) {\n"
    "        int2 o = b + int2(k & 1, k >> 1);\n"
    "        if (any(o < 0) || any(o > hi)) continue;\n"
    "        float4 h = hist.read(uint2(o));\n"
    "        float w = ((k & 1) ? f.x : 1.0 - f.x) * ((k >> 1) ? f.y : 1.0 - f.y) * saturate(1.0 - abs(h.y - pc.w) / (0.05 * pc.w));\n"
    "        hv += h * w, hw += w;\n"
    "      }\n"
    "      if (hw > 0.05) {\n"
    "        hv /= hw;\n"
    "        cnt = min(hv.z + 1.0, U.tp.w);\n"
    "        a = mix(hv.x, s, max(1.0 / cnt, U.tp.x));\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  out.write(float4(a, z, cnt, 0.0), p);\n"
    "}\n";

/* AoU of AO_KERNELS_MSL */
typedef struct AoU
{
    float vp[4], hs[4], pr[4], pz[4], prm[4], tp[4];
    float iv[16], pvp[16];
} AoU;

/* AU of AO_MSL */
typedef struct AoShadeU
{
    float pvp[16];
    float vp[4];
    float k[4];
} AoShadeU;

static struct
{
    int tried;
    id<MTLLibrary> lib;
    id<MTLComputePipelineState> prep, gtao, rays, resolve;
    id<MTLTexture> hz, raw, hist[2];
    int cur;          /* hist[cur] is the last result */
    uint32_t hw, hh;
    uint32_t frame;
    /* the zone's draw this frame: its depth, target, camera and viewport (ao_note) */
    id<MTLTexture> depth, rt;
    uint64_t noted;
    float view[16], proj[16];
    uint32_t vp[4];
    /* the last result's camera (row-major view x projection) and viewport; made: its frame */
    float made_vp[16];
    uint32_t made_view[4];
    uint64_t made;
    int on;           /* this batch's zone and characters take it */
    AoShadeU su;
} g_ao;

static int ao_init(void)
{
    if (g_ao.tried)
        return g_ao.resolve != nil;
    g_ao.tried = 1;
    g_ao.lib = compile(AO_KERNELS_MSL);
    if (!g_ao.lib)
        return 0;
    id<MTLComputePipelineState>* pp[4] = { &g_ao.prep, &g_ao.gtao, &g_ao.rays, &g_ao.resolve };
    NSString* names[4] = { @"ao_prep", @"ao_gtao", @"ao_rays", @"ao_resolve" };
    for (int i = 0; i < 4; ++i)
    {
        NSError* err = nil;
        id<MTLFunction> fn = [g_ao.lib newFunctionWithName:names[i]];
        *pp[i] = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&err] : nil;
        [fn release];
        if (!*pp[i] && i != 2) /* the rays only where there are ray queries */
        {
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "[recomp] gfx: scene occlusion: %s failed: %s\n", [names[i] UTF8String],
                err ? [[err localizedDescription] UTF8String] : "?");
            g_ao.resolve = nil;
            return 0;
        }
    }
    return 1;
}

/* the quality's method and size: 1 GTAO low (2 slices of 4 steps each way), 2 GTAO high (3 of 6), 3 rays low (1 a
 * pixel), 4 rays high (2) */
static int ao_rays_q(void) { return g_fxs.scene_ao_q >= 2.5f; }

/* the zone's draw (gfx_world_draw): its depth and camera, the first this frame into the main target */
static void ao_note(const GfxWorldFrame* f, id<MTLTexture> depth)
{
    if (g_fxs.scene_ao <= 0.0f || !depth || g_rt_face || g_rt_level || g_ao.noted == g_serial)
        return;
    if (g_ao.depth != depth)
        [g_ao.depth release], g_ao.depth = [depth retain];
    if (g_ao.rt != g_rt->tex)
        [g_ao.rt release], g_ao.rt = [g_rt->tex retain];
    memcpy(g_ao.view, f->view, 64), memcpy(g_ao.proj, f->proj, 64);
    uint32_t w = (uint32_t)g_rt->tex.width, h = (uint32_t)g_rt->tex.height;
    uint32_t x = f->vp[0] < w ? f->vp[0] : 0, y = f->vp[1] < h ? f->vp[1] : 0;
    uint32_t vw = f->vp[2] && x + f->vp[2] <= w ? f->vp[2] : w - x, vh = f->vp[3] && y + f->vp[3] <= h ? f->vp[3] : h - y;
    g_ao.vp[0] = x, g_ao.vp[1] = y, g_ao.vp[2] = vw, g_ao.vp[3] = vh;
    g_ao.noted = g_serial;
}

static int ao_tex(id<MTLTexture>* t, uint32_t w, uint32_t h, MTLPixelFormat f)
{
    if (*t && (*t).width == w && (*t).height == h)
        return 1;
    [*t release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModePrivate;
    *t = [g_dev newTextureWithDescriptor:d];
    return *t != nil;
}

static void inverse4(const float* m, float* out);

/* the scene done (gfx_scene_done): this frame's occlusion from the zone's depth, ahead of the post effects */
static void ao_make(GfxTex* color)
{
    if (g_fxs.scene_ao <= 0.0f || g_ao.noted != g_serial || !color || color->tex != g_ao.rt || !ao_init())
        return;
    int rays = ao_rays_q() && g_ao.rays && g_gi.as && g_gi.attr;
    @autoreleasepool
    {
        flush_pass();
        /* half the viewport across, and down as many as keep the texels square on screen (the scene's target can be
         * a square stretched to the window: its projection says the window's shape) */
        const float* P = g_ao.proj;
        uint32_t hw = (g_ao.vp[2] + 1) / 2;
        float sq = P[5] != 0.0f ? fabsf(P[0] / P[5]) : 1.0f;
        uint32_t hh = (uint32_t)((float)hw * sq + 0.5f);
        if (hh > g_ao.vp[3])
            hh = g_ao.vp[3];
        if (hw < 8 || hh < 8)
            return;
        int fresh = !g_ao.hist[0] || g_ao.hw != hw || g_ao.hh != hh;
        if (!ao_tex(&g_ao.hz, hw, hh, MTLPixelFormatR32Float) || !ao_tex(&g_ao.raw, hw, hh, MTLPixelFormatR16Float) ||
            !ao_tex(&g_ao.hist[0], hw, hh, MTLPixelFormatRGBA16Float) || !ao_tex(&g_ao.hist[1], hw, hh, MTLPixelFormatRGBA16Float))
            return;
        g_ao.hw = hw, g_ao.hh = hh;
        float vpm[16], iv[16];
        mat_mul(vpm, g_ao.view, g_ao.proj);
        inverse4(g_ao.view, iv);
        AoU u;
        memset(&u, 0, sizeof u);
        u.vp[0] = (float)g_ao.vp[0], u.vp[1] = (float)g_ao.vp[1], u.vp[2] = (float)g_ao.vp[2], u.vp[3] = (float)g_ao.vp[3];
        u.hs[0] = (float)hw, u.hs[1] = (float)hh, u.hs[2] = 1.0f / (float)hw, u.hs[3] = 1.0f / (float)hh;
        u.pr[0] = P[0], u.pr[1] = P[5], u.pr[2] = P[8], u.pr[3] = P[9];
        u.pz[0] = P[10], u.pz[1] = P[14], u.pz[2] = P[11] < 0.0f ? -1.0f : 1.0f, u.pz[3] = 0.99999f;
        int q = g_fxs.scene_ao_q >= 3.5f ? 4 : g_fxs.scene_ao_q >= 2.5f ? 3 : g_fxs.scene_ao_q >= 1.5f ? 2 : 1;
        if (q >= 3 && !rays)
            q = 2;
        u.prm[0] = g_fxs.scene_ao_radius > 0.05f ? g_fxs.scene_ao_radius : 0.05f;
        u.prm[1] = q == 1 ? 4.0f : 6.0f;
        u.prm[2] = q == 1 ? 2.0f : q == 2 ? 3.0f : q == 3 ? 1.0f : 2.0f;
        u.prm[3] = 0.06f * (float)hw;
        int same = !fresh && g_ao.made + 1 >= g_serial && !memcmp(g_ao.made_view, g_ao.vp, sizeof g_ao.vp);
        u.tp[0] = 0.08f, u.tp[1] = same ? 1.0f : 0.0f, u.tp[2] = (float)(g_ao.frame++ & 63), u.tp[3] = 16.0f;
        memcpy(u.iv, iv, 64);
        memcpy(u.pvp, g_ao.made_vp, 64);
        MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
        gi_ts_mark(cp, GFX_PASS_SCENE_AO);
        id<MTLComputeCommandEncoder> e = [cmd() computeCommandEncoderWithDescriptor:cp];
        MTLSize grid = MTLSizeMake(hw, hh, 1), tg = MTLSizeMake(8, 8, 1);
        [e setBytes:&u length:sizeof u atIndex:0];
        [e setComputePipelineState:g_ao.prep];
        [e setTexture:g_ao.depth atIndex:0];
        [e setTexture:g_ao.hz atIndex:1];
        [e dispatchThreads:grid threadsPerThreadgroup:tg];
        /* while timed, each step its own pass (an encoder's start and end are what a pass is timed by) */
        int split = gfx_pass_timing_on();
        if (split)
        {
            [e endEncoding];
            cp = [MTLComputePassDescriptor computePassDescriptor];
            gi_ts_mark(cp, GFX_PASS_SCENE_AO_TRACE);
            e = [cmd() computeCommandEncoderWithDescriptor:cp];
            [e setBytes:&u length:sizeof u atIndex:0];
        }
        else
            [e memoryBarrierWithScope:MTLBarrierScopeTextures];
        [e setTexture:g_ao.hz atIndex:0];
        [e setTexture:g_ao.raw atIndex:1];
        if (q >= 3)
        {
            [e setComputePipelineState:g_ao.rays];
            [e setAccelerationStructure:g_gi.as atBufferIndex:1];
            [e setBuffer:g_gi.attr offset:0 atIndex:2];
        }
        else
            [e setComputePipelineState:g_ao.gtao];
        [e dispatchThreads:grid threadsPerThreadgroup:tg];
        if (split)
        {
            [e endEncoding];
            cp = [MTLComputePassDescriptor computePassDescriptor];
            gi_ts_mark(cp, GFX_PASS_SCENE_AO);
            e = [cmd() computeCommandEncoderWithDescriptor:cp];
            [e setBytes:&u length:sizeof u atIndex:0];
            [e setTexture:g_ao.hz atIndex:0];
        }
        else
            [e memoryBarrierWithScope:MTLBarrierScopeTextures];
        int nx = g_ao.cur ^ 1;
        [e setComputePipelineState:g_ao.resolve];
        [e setTexture:g_ao.raw atIndex:1];
        [e setTexture:g_ao.hist[g_ao.cur] atIndex:2];
        [e setTexture:g_ao.hist[nx] atIndex:3];
        [e dispatchThreads:grid threadsPerThreadgroup:tg];
        [e endEncoding];
        g_ao.cur = nx;
        memcpy(g_ao.made_vp, vpm, 64);
        memcpy(g_ao.made_view, g_ao.vp, sizeof g_ao.vp);
        g_ao.made = g_serial;
        /* what the next frame's shading reads */
        AoShadeU* s = &g_ao.su;
        memcpy(s->pvp, vpm, 64);
        s->vp[0] = (float)g_ao.vp[2], s->vp[1] = (float)g_ao.vp[3];
        s->vp[2] = s->vp[3] = 1.0f; /* the texture is the viewport */
        static uint32_t told;
        if (++told % 3000 == 1)
            fprintf(stderr, "[recomp] gfx: scene occlusion: %s, %ux%u of a %ux%u viewport (projection %.3f %.3f), reach %.2f\n",
                q >= 3 ? (q == 4 ? "rays high" : "rays low") : (q == 2 ? "GTAO high" : "GTAO low"), hw, hh, g_ao.vp[2], g_ao.vp[3],
                (double)P[0], (double)P[5], (double)u.prm[0]);
    }
}

/* the occlusion for a batch's draws: AU at buffer 10, the result at texture 6, both stages; 0 off */
static int ao_bind(id<MTLRenderCommandEncoder> e)
{
    g_ao.on = 0;
    if (g_fxs.scene_ao <= 0.0f || !g_ao.made || g_ao.made + 2 < g_serial || !g_ao.hist[g_ao.cur] || !g_rt ||
        g_rt->tex != g_ao.rt || g_rt_face || g_rt_level)
        return 0;
    AoShadeU* s = &g_ao.su;
    s->k[0] = g_fxs.scene_ao, s->k[1] = g_fxs.scene_ao * g_fxs.scene_ao_chars, s->k[2] = g_fxs.scene_ao_debug, s->k[3] = 0.08f;
    [e setVertexBytes:s length:sizeof *s atIndex:10];
    [e setFragmentBytes:s length:sizeof *s atIndex:10];
    [e setFragmentTexture:g_ao.hist[g_ao.cur] atIndex:6];
    g_ao.on = 1;
    return 1;
}

/* a 4x4 inverse (cofactors); a singular m gives the identity */
static void inverse4(const float* m, float* out)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f)
    {
        memset(out, 0, 64);
        out[0] = out[5] = out[10] = out[15] = 1.0f;
        return;
    }
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
}
