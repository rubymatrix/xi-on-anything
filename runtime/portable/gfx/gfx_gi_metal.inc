/* --- the scene renderer's bounce light (gfx_gi_zone, gfx_gi_frame, gfx.h) ------------------------------------------
 * Included by gfx_metal.m after the scene renderer's shadows (whose zone layer it reads), ahead of the zone's draws;
 * GI_MSL, the shading's side, is gfx_gi_msl.h.
 *
 * A grid of light probes round the camera (DDGI, cut down): each probe holds the light arriving at its point from
 * every direction, as two small octahedral maps in two atlases - the irradiance (cosine-weighted, 6x6 texels) with the
 * share of its rays that reached the sky in alpha, and the distance to what its rays hit and its square (14x14), which
 * the shading uses to keep a probe behind a wall from lighting the room (Chebyshev, as variance shadow maps).
 *
 * The probes are traced against the zone: at load every static instance's coarsest model is flattened into one
 * world-space triangle list (gi_flatten: each triangle with its albedo - its texture at its centre, coarse, times its
 * vertex colour, times 2 as MODULATE2X - and its normal, turned to face the way its vertex normals do) and built into
 * one acceleration structure. Each frame a share of the probes (1 in stride) casts rays: a hit is lit by the sun and
 * the moon (a shadow ray each), the lamps of the light pool (no shadow) and, for later bounces, by the probes
 * themselves; a ray that reaches the sky brings no light (the game's ambient stands for the sky) but counts toward the
 * probe's sky share. The rays are blended into the maps with hysteresis; a probe whose rays mostly hit backfaces is
 * inside something and is not used.
 *
 * The grid moves with the camera in whole cells, each probe's storage slot fixed by its world cell (mod the grid's
 * size), so a probe that stays in the grid is never moved or traced afresh; the cells that come in are cleared
 * (gi_scroll) and unused until first traced.
 *
 * The zone and characters add the probes' irradiance where they add the ambient (times the vertex colour), and with
 * scene_gi_sky darken the ambient by the probes' sky share. Looked up per vertex (the bounce light is smooth: per pixel
 * cost the zone's pass about 1 ms at 1080p), or per pixel in scene 2 at quality 3. */

#define GI_IRR 6  /* texels across a probe's irradiance map, inside its border */
#define GI_DIS 14 /* its distance map's */

static const char GI_KERNELS_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace metal::raytracing;\n"
    GI_MSL
    "struct GL { float4 pos, diffuse, ambient, att; };\n" /* GfxWorldLight */
    /* the shadows' uniforms (ShU, SHADOW_MSL's SH) */
    "struct SH { float4x4 m[4]; float4 cas[4]; float4 cbox[4]; float4 dir; float4 cam; float4 info; float4x4 cm[4];\n"
    "            float4 ctx; };\n"
    /* the sun's or moon's shadow at X (normal n) from the zone's cascades, one texel read: 1 lit, 0 not, -1 outside
     * them (a ray then) */
    "static float gi_cascade(constant SH& S, depth2d_array<float> zm, float3 X, float3 n) {\n"
    "  float d = length(X - S.cam.xyz);\n"
    "  for (uint c = 0; c < uint(S.info.x); ++c) {\n"
    "    if (S.cas[c].w == 0.0 || d > S.cas[c].x) continue;\n"
    "    float t = S.cas[c].y;\n"
    "    float4 h = S.m[c] * float4(X + n * (2.0 * t) + S.dir.xyz * t, 1.0);\n"
    "    if (abs(h.x) > 0.99 || abs(h.y) > 0.99) continue;\n"
    "    float w = float(zm.get_width());\n"
    "    uint2 px = uint2(clamp(float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5) * w, 0.0, w - 1.0));\n"
    "    return h.z <= zm.read(px, c) ? 1.0 : 0.0;\n"
    "  }\n"
    "  return -1.0;\n"
    "}\n"
    "struct FI { float4x4 world, inv; uint first, count, prim, tri; uint flags, level, pad0, pad1; };\n"
    /* the zone flattened: triangle t of an item into world space, with its albedo and its normal (faced as its vertex
     * normals face, its corners turned to match); flags 1 textured, 2 two-sided */
    "kernel void gi_flatten(device const uchar* vb [[buffer(0)]], constant FI& I [[buffer(1)]],\n"
    "                       device packed_float3* pos [[buffer(2)]], device uint2* attr [[buffer(3)]],\n"
    "                       texture2d<float> tx [[texture(0)]], uint t [[thread_position_in_grid]]) {\n"
    "  if (t >= I.count) return;\n"
    "  constexpr sampler rs(coord::normalized, filter::linear, mip_filter::linear, address::repeat);\n"
    "  uint v0 = I.first + (I.prim == 5u ? t : 3u * t), vi[3] = { v0, v0 + 1u, v0 + 2u };\n"
    "  if (I.prim == 5u && (t & 1u)) { vi[0] = v0 + 1u; vi[1] = v0; }\n" /* a strip's odd triangles wind the other way */
    "  float3 p[3], n = float3(0.0);\n"
    "  float4 vc = float4(0.0), tc = float4(0.0);\n"
    "  float2 uvc = float2(0.0);\n"
    "  for (int k = 0; k < 3; ++k) {\n"
    "    device const uchar* v = vb + vi[k] * 36u;\n"
    "    device const float* f = (device const float*)v;\n"
    "    p[k] = (I.world * float4(f[0], f[1], f[2], 1.0)).xyz;\n"
    "    n += (float4(f[3], f[4], f[5], 0.0) * I.inv).xyz;\n"
    "    uchar4 c = *(device const uchar4*)(v + 24);\n"
    "    vc += float4(c.z, c.y, c.x, c.w) / 255.0;\n"
    "    float2 uv = float2(f[7], f[8]);\n"
    "    uvc += uv;\n"
    "    if (I.flags & 1u) tc += tx.sample(rs, uv, level(float(I.level)));\n"
    "  }\n"
    "  uvc /= 3.0;\n"
    "  if (I.flags & 1u) tc = (tc + tx.sample(rs, uvc, level(float(I.level)))) * 0.25;\n"
    "  else tc = float4(0.5);\n"
    "  vc /= 3.0;\n"
    "  float3 fn = cross(p[1] - p[0], p[2] - p[0]);\n"
    "  if (dot(fn, n) < 0.0) { float3 x = p[1]; p[1] = p[2]; p[2] = x; fn = -fn; }\n"
    "  float fl = length(fn);\n"
    "  if (!(fl > 1e-12)) p[1] = p[2] = p[0];\n" /* nothing: a degenerate triangle no ray hits */
    "  fn = fl > 1e-12 ? fn / fl : float3(0.0, -1.0, 0.0);\n"
    "  uint o = I.tri + t;\n"
    "  pos[3u * o] = packed_float3(p[0]);\n"
    "  pos[3u * o + 1u] = packed_float3(p[1]);\n"
    "  pos[3u * o + 2u] = packed_float3(p[2]);\n"
    "  float3 alb = min(2.0 * tc.rgb * vc.rgb, float3(0.9));\n"
    "  attr[o] = uint2(pack_float_to_unorm4x8(float4(alb, float(I.flags & 2u) / 255.0)),\n"
    "                  pack_float_to_unorm2x16(gi_oct(fn) * 0.5 + 0.5));\n"
    "}\n"
    /* probe l's world cell */
    "static int3 gi_cell(constant GU& G, uint l) {\n"
    "  int3 s = int3(int(l) % G.dims.x, (int(l) / G.dims.x) % G.dims.y, int(l) / (G.dims.x * G.dims.y));\n"
    "  return G.base.xyz + gi_slot(G, s - G.base.xyz);\n"
    "}\n"
    /* ray r of n: a spherical Fibonacci direction, turned by this update's rotation */
    "static float3 gi_ray(constant GU& G, uint r) {\n"
    "  float n = float(G.n.x), z = 1.0 - (2.0 * float(r) + 1.0) / n, s = sqrt(saturate(1.0 - z * z));\n"
    "  float ph = 6.2831853 * fract(float(r) * 0.6180340);\n"
    "  return normalize((G.rot * float4(cos(ph) * s, sin(ph) * s, z, 0.0)).xyz);\n"
    "}\n"
    /* the cells new to the grid: cleared, not used till traced */
    "kernel void gi_scroll(constant GU& G [[buffer(0)]], device uint* PS [[buffer(1)]], device int4* PC [[buffer(2)]],\n"
    "                      uint l [[thread_position_in_grid]]) {\n"
    "  if (l >= uint(G.dims.w)) return;\n"
    "  int3 c = gi_cell(G, l);\n"
    "  if (any(PC[l].xyz != c) || PC[l].w == 0) { PC[l] = int4(c, 1); PS[l] = 0u; }\n"
    "}\n"
    /* ray id.x of this update's probe id.y: what it brings back (rgb) and how far it went (a; < 0 a backface, 1e4 the
     * sky) */
    "kernel void gi_trace(constant GU& G [[buffer(0)]], const device uint* PS [[buffer(1)]],\n"
    "                     device const uint2* attr [[buffer(2)]], constant GL* LT [[buffer(3)]], device float4* RB [[buffer(4)]],\n"
    "                     primitive_acceleration_structure as [[buffer(5)]], constant SH& S [[buffer(6)]],\n"
    "                     texture2d<float> ir [[texture(0)]], texture2d<float> ds [[texture(1)]],\n"
    "                     depth2d_array<float> zm [[texture(2)]], uint2 id [[thread_position_in_grid]]) {\n"
    "  uint r = id.x, u = id.y, l = u * G.n.y + G.n.z;\n"
    "  if (r >= G.n.x || l >= uint(G.dims.w)) return;\n"
    "  float3 o = float3(gi_cell(G, l)) * G.org.w, d = gi_ray(G, r);\n"
    "  intersector<triangle_data> is;\n"
    "  is.assume_geometry_type(geometry_type::triangle);\n"
    "  is.force_opacity(forced_opacity::opaque);\n"
    "  ray ry(o, d, 0.0, G.tr.x);\n"
    "  auto h = is.intersect(ry, as);\n"
    "  float4 res = float4(0.0, 0.0, 0.0, 1e4);\n"
    "  if (h.type != intersection_type::none) {\n"
    "    uint2 at = attr[h.primitive_id];\n"
    "    float4 alb = unpack_unorm4x8_to_float(at.x);\n"
    "    float3 n = gi_unoct(unpack_unorm2x16_to_float(at.y) * 2.0 - 1.0);\n"
    "    float t = h.distance;\n"
    "    res = float4(0.0, 0.0, 0.0, -t);\n"
    "    if (dot(n, d) > 0.0 && alb.a > 0.0) n = -n;\n" /* two-sided: lit from the side the ray came */
    "    if (dot(n, d) <= 0.0) {\n"
    "      float3 X = o + d * t + n * 0.05;\n"
    "      float3 E = G.amb.rgb * G.tr.w;\n"
    "      intersector<triangle_data> sh;\n"
    "      sh.assume_geometry_type(geometry_type::triangle);\n"
    "      sh.force_opacity(forced_opacity::opaque);\n"
    "      sh.accept_any_intersection(true);\n"
    "      for (int k = 0; k < 2; ++k) {\n"
    "        float4 L = k ? G.sun2_dir : G.sun_dir;\n"
    "        float nl = dot(n, L.xyz);\n"
    "        if (L.w <= 0.0 || nl <= 0.0) continue;\n"
    "        float v = G.org.y > 0.5 && dot(L.xyz, S.dir.xyz) > 0.98 ? gi_cascade(S, zm, X, n) : -1.0;\n"
    "        if (v < 0.0) { ray sr(X, L.xyz, 0.0, 2000.0); v = sh.intersect(sr, as).type == intersection_type::none ? 1.0 : 0.0; }\n"
    "        E += (k ? G.sun2_col.rgb : G.sun_col.rgb) * (nl * v);\n"
    "      }\n"
    "      for (uint i = 0; i < G.n.w; ++i) {\n"
    "        GL l = LT[i];\n"
    "        float3 v = l.pos.xyz - X;\n"
    "        float dist = length(v);\n"
    "        if (dist >= l.pos.w) continue;\n"
    "        float q = dist / l.pos.w, win = saturate(1.0 - q * q * q * q);\n" /* the range's edge, smoothed */
    "        float a = win * win / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-6);\n"
    "        E += l.ambient.rgb * a + l.diffuse.rgb * a * max(dot(n, v / max(dist, 1e-6)), 0.0);\n"
    "      }\n"
    "      if (G.tr.z > 0.0) E += gi_at(G, PS, ir, ds, X, n).rgb * G.tr.z;\n" /* the bounces before */
    "      res = float4(alb.rgb * E, t);\n"
    "    }\n"
    "  }\n"
    "  RB[u * G.n.x + r] = res;\n"
    "}\n"
    /* a probe's map (TS texels a side, a border of one round N inside) from its rays: texel tid of update slot gid;
     * the border copied from the inside as the octahedron folds (bilinear lookups wrap across its edges). dis: the
     * distance map (mean and square, sharp weights), else the irradiance (cosine-weighted, and the sky's share). The
     * irradiance's pass also marks the probe traced, and inside something when a quarter of its rays hit backfaces. */
    "template <int TS, bool dis>\n"
    "static void gi_blend(constant GU& G, device uint* PS, device const float4* RB,\n"
    "                     texture2d<float, access::read_write> m, uint2 tid, uint2 gid, threadgroup float4* tg) {\n"
    "  const int N = TS - 2;\n"
    "  uint u = gid.x, l = u * G.n.y + G.n.z;\n"
    "  if (l >= uint(G.dims.w)) return;\n"
    "  int3 s = int3(int(l) % G.dims.x, (int(l) / G.dims.x) % G.dims.y, int(l) / (G.dims.x * G.dims.y));\n"
    "  uint2 org = uint2(uint(s.x + G.dims.x * s.y), uint(s.z)) * uint(TS);\n"
    "  bool fresh = (PS[l] & 1u) == 0u;\n"
    "  if (tid.x >= 1u && tid.y >= 1u && tid.x <= uint(N) && tid.y <= uint(N)) {\n"
    "    float3 dir = gi_unoct(((float2(tid) - 0.5) / float(N)) * 2.0 - 1.0);\n"
    "    float4 sum = float4(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (uint r = 0; r < G.n.x; ++r) {\n"
    "      float4 R = RB[u * G.n.x + r];\n"
    "      float c = max(dot(dir, gi_ray(G, r)), 0.0);\n"
    "      if (dis) {\n"
    "        float w = pow(c, 50.0), dd = min(abs(R.w), G.tr.x);\n"
    "        sum += float4(dd, dd * dd, 0.0, 0.0) * w;\n"
    "        ws += w;\n"
    "      } else {\n"
    "        if (R.w < 0.0) continue;\n" /* backfaces bring nothing */
    "        sum += float4(R.rgb, R.w >= 1e3 ? 1.0 : 0.0) * c;\n"
    "        ws += c;\n"
    "      }\n"
    "    }\n"
    "    float4 nv = ws > 1e-6 ? sum / ws : float4(0.0);\n"
    "    float4 old = m.read(org + tid);\n"
    "    tg[tid.y * TS + tid.x] = fresh ? nv : mix(nv, old, G.tr.y);\n"
    "  }\n"
    "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "  uint x = tid.x, y = tid.y, e = uint(TS - 1);\n"
    "  uint sx = x, sy = y;\n"
    "  if ((x == 0u || x == e) && (y == 0u || y == e)) sx = x == 0u ? uint(N) : 1u, sy = y == 0u ? uint(N) : 1u;\n"
    "  else if (y == 0u || y == e) sx = e - x, sy = y == 0u ? 1u : uint(N);\n"
    "  else if (x == 0u || x == e) sx = x == 0u ? 1u : uint(N), sy = e - y;\n"
    "  m.write(tg[sy * TS + sx], org + tid);\n"
    "  if (!dis && x == 0u && y == 0u) {\n"
    "    uint bf = 0u;\n"
    "    for (uint r = 0; r < G.n.x; ++r) bf += RB[u * G.n.x + r].w < 0.0 ? 1u : 0u;\n"
    "    PS[l] = 1u | (bf * 4u > G.n.x ? 2u : 0u);\n"
    "  }\n"
    "}\n"
    "kernel void gi_blend_irr(constant GU& G [[buffer(0)]], device uint* PS [[buffer(1)]], device const float4* RB [[buffer(4)]],\n"
    "                         texture2d<float, access::read_write> m [[texture(0)]], uint2 tid [[thread_position_in_threadgroup]],\n"
    "                         uint2 gid [[threadgroup_position_in_grid]]) {\n"
    "  threadgroup float4 tg[8 * 8];\n"
    "  gi_blend<8, false>(G, PS, RB, m, tid, gid, tg);\n"
    "}\n"
    "kernel void gi_blend_dis(constant GU& G [[buffer(0)]], device uint* PS [[buffer(1)]], device const float4* RB [[buffer(4)]],\n"
    "                         texture2d<float, access::read_write> m [[texture(0)]], uint2 tid [[thread_position_in_threadgroup]],\n"
    "                         uint2 gid [[threadgroup_position_in_grid]]) {\n"
    "  threadgroup float4 tg[16 * 16];\n"
    "  gi_blend<16, true>(G, PS, RB, m, tid, gid, tg);\n"
    "}\n";

/* GU of GI_MSL */
typedef struct GiU
{
    float org[4];     /* x 1: looked up per pixel (scene 2), y 1: the shadows' zone layer is there, w: world units between
                       * probes */
    int32_t base[4];  /* the grid's lowest cell (world cells); w 1 when the grid is there */
    int32_t dims[4];  /* cells each way; w all of them */
    float k[4];       /* strength, the sky's share's hold on the ambient, debug, the shading's normal offset */
    float inv[4];     /* 1 / the atlases' sizes: irradiance w, h, distance w, h */
    float tr[4];      /* the rays' reach, hysteresis, the earlier bounces' weight, the ambient's weight at a hit */
    uint32_t n[4];    /* rays a probe, 1 probe in this many traced an update, this update's phase, lamps */
    float sun_dir[4], sun_col[4], sun2_dir[4], sun2_col[4], amb[4];
    float rot[16];    /* this update's rays' rotation (columns) */
} GiU;

static struct
{
    int tried;
    id<MTLLibrary> lib;
    id<MTLComputePipelineState> flatten, scroll, trace, birr, bdis;
    /* the zone */
    id<MTLBuffer> pos, attr;
    id<MTLAccelerationStructure> as;
    uint32_t ntri;
    /* the probes */
    id<MTLTexture> irr, dis;
    id<MTLBuffer> ps, pc, rb;
    uint32_t quality;
    uint64_t updated; /* the frame they were last traced */
    uint32_t phase;
    uint32_t seed;
    int on;           /* this batch's zone and characters take the bounce light */
    GiU u;
} g_gi;

static int gi_init(void)
{
    if (g_gi.tried)
        return g_gi.trace != nil;
    g_gi.tried = 1;
    if (![g_dev supportsFamily:MTLGPUFamilyApple6] || !g_dev.supportsRaytracing ||
        g_dev.readWriteTextureSupport < MTLReadWriteTextureTier2)
    {
        fprintf(stderr, "[recomp] gfx: scene bounce light: this GPU has no ray queries or read-write textures\n");
        return 0;
    }
    g_gi.lib = compile(GI_KERNELS_MSL);
    if (!g_gi.lib)
        return 0;
    id<MTLComputePipelineState>* pp[5] = { &g_gi.flatten, &g_gi.scroll, &g_gi.trace, &g_gi.birr, &g_gi.bdis };
    NSString* names[5] = { @"gi_flatten", @"gi_scroll", @"gi_trace", @"gi_blend_irr", @"gi_blend_dis" };
    for (int i = 0; i < 5; ++i)
    {
        NSError* err = nil;
        id<MTLFunction> fn = [g_gi.lib newFunctionWithName:names[i]];
        *pp[i] = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&err] : nil;
        [fn release];
        if (!*pp[i])
        {
            __atomic_fetch_add(&g_failures, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "[recomp] gfx: scene bounce light: %s failed: %s\n", [names[i] UTF8String],
                err ? [[err localizedDescription] UTF8String] : "?");
            g_gi.trace = nil;
            return 0;
        }
    }
    return 1;
}

/* a compute pass times itself as pass (ts_mark's, for compute) */
static void gi_ts_mark(MTLComputePassDescriptor* cp, int pass)
{
    frame_begin();
    if (g_ts_n >= TS_MAX)
        return;
    id<MTLCounterSampleBuffer> b = ts_buffer();
    if (!b)
        return;
    MTLComputePassSampleBufferAttachmentDescriptor* a = cp.sampleBufferAttachments[0];
    a.sampleBuffer = b;
    a.startOfEncoderSampleIndex = 2 * g_ts_n;
    a.endOfEncoderSampleIndex = 2 * g_ts_n + 1;
    g_ts_pass[g_frame][g_ts_n++] = (uint8_t)pass;
}

void gfx_gi_zone(const GfxGiZone* z)
{
    if (!g_dev)
        return;
    @autoreleasepool
    {
        [g_gi.as release], [g_gi.pos release], [g_gi.attr release];
        g_gi.as = nil, g_gi.pos = g_gi.attr = nil, g_gi.ntri = 0;
        g_gi.updated = 0;
        if (g_gi.ps) /* every probe afresh */
            memset([g_gi.ps contents], 0, [g_gi.ps length]), memset([g_gi.pc contents], 0, [g_gi.pc length]);
        if (!z || !z->nitem || !gi_init())
            return;
        uint64_t t0 = gfx_now_ns();
        uint32_t ntri = 0;
        for (uint32_t i = 0; i < z->nitem; ++i)
            if (z->item[i].vb && z->item[i].inst < z->nworld)
                ntri += z->item[i].count;
        if (!ntri)
            return;
        g_gi.pos = [g_dev newBufferWithLength:(NSUInteger)ntri * 36 options:MTLResourceStorageModePrivate];
        g_gi.attr = [g_dev newBufferWithLength:(NSUInteger)ntri * 8 options:MTLResourceStorageModePrivate];
        if (!g_gi.pos || !g_gi.attr)
            return;
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:g_gi.flatten];
        [e setBuffer:g_gi.pos offset:0 atIndex:2];
        [e setBuffer:g_gi.attr offset:0 atIndex:3];
        struct
        {
            float world[16], inv[16];
            uint32_t first, count, prim, tri, flags, level, pad[2];
        } fi;
        uint32_t at = 0;
        for (uint32_t i = 0; i < z->nitem; ++i)
        {
            const GfxWorldItem* it = &z->item[i];
            if (!it->vb || it->inst >= z->nworld)
                continue;
            memcpy(fi.world, z->world + 16 * (size_t)it->inst, 64);
            memcpy(fi.inv, z->world_inv + 16 * (size_t)it->inst, 64);
            fi.first = it->first, fi.count = it->count, fi.prim = it->prim, fi.tri = at;
            fi.flags = (it->flags & GFX_WORLD_TWOSIDED) ? 2u : 0u, fi.level = 0;
            const GfxTex* t = it->tex;
            if (t && t->view)
            {
                fi.flags |= 1u;
                /* a level some 8 texels across, when the chain is all there */
                uint32_t lv = 0, w = t->w < t->h ? t->w : t->h;
                while (w > 8 && lv + 1 < t->levels)
                    w >>= 1, ++lv;
                fi.level = t->levels > 1 && t->levels < 32 && t->filled == (1u << t->levels) - 1 ? lv : 0;
                [e setTexture:t->view atIndex:0];
            }
            [e setBuffer:it->vb->b offset:0 atIndex:0];
            [e setBytes:&fi length:sizeof fi atIndex:1];
            [e dispatchThreads:MTLSizeMake(it->count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            at += it->count;
        }
        [e endEncoding];
        MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        g.vertexBuffer = g_gi.pos;
        g.vertexStride = 12;
        g.vertexFormat = MTLAttributeFormatFloat3;
        g.triangleCount = ntri;
        g.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor* pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pd.geometryDescriptors = @[ g ];
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:pd];
        g_gi.as = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        id<MTLBuffer> scratch = [g_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate];
        if (!g_gi.as || !scratch)
        {
            [scratch release];
            [cb commit];
            [g_gi.as release], g_gi.as = nil;
            fprintf(stderr, "[recomp] gfx: scene bounce light: no room for the zone's structure (%u triangles)\n", ntri);
            return;
        }
        id<MTLBuffer> csz = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        id<MTLAccelerationStructureCommandEncoder> ae = [cb accelerationStructureCommandEncoder];
        [ae buildAccelerationStructure:g_gi.as descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
        [ae writeCompactedAccelerationStructureSize:g_gi.as toBuffer:csz offset:0];
        [ae endEncoding];
        [cb commit];
        [cb waitUntilCompleted]; /* once a zone, while it loads */
        [scratch release];
        double gpu = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
        /* compacted (the build's own size is a bound), and the triangles' corners let go: the structure holds its own */
        uint32_t csize = *(const uint32_t*)[csz contents];
        [csz release];
        id<MTLAccelerationStructure> small = csize ? [g_dev newAccelerationStructureWithSize:csize] : nil;
        if (small)
        {
            id<MTLCommandBuffer> cc = [g_queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> ce = [cc accelerationStructureCommandEncoder];
            [ce copyAndCompactAccelerationStructure:g_gi.as toAccelerationStructure:small];
            [ce endEncoding];
            [cc commit];
            [cc waitUntilCompleted];
            gpu += (cc.GPUEndTime - cc.GPUStartTime) * 1e3;
            [g_gi.as release];
            g_gi.as = small;
        }
        [g_gi.pos release], g_gi.pos = nil;
        g_gi.ntri = ntri;
        fprintf(stderr, "[recomp] gfx: scene bounce light: the zone's structure: %u items, %u triangles, %.1f MB (%.1f MB "
                        "before compaction) + %.1f MB the triangles' colours and normals, GPU %.1f ms, %.1f ms in all\n",
            z->nitem, ntri, (double)(small ? csize : sz.accelerationStructureSize) / 1048576.0,
            (double)sz.accelerationStructureSize / 1048576.0, (double)ntri * 8.0 / 1048576.0, gpu, (double)(gfx_now_ns() - t0) / 1e6);
    }
}

/* the grid for quality q: cells each way, world units between them, rays a probe, 1 in how many traced a frame,
 * hysteresis */
static void gi_preset(uint32_t q, int32_t d[3], float* sp, uint32_t* rays, uint32_t* stride, float* hyst)
{
    if (q >= 2)
        d[0] = 32, d[1] = 12, d[2] = 32, *sp = 3.0f, *rays = 64, *stride = 12, *hyst = 0.9f;
    else
        d[0] = 24, d[1] = 8, d[2] = 24, *sp = 4.0f, *rays = 32, *stride = 16, *hyst = 0.85f;
}

static int gi_probes(uint32_t q)
{
    if (g_gi.irr && g_gi.quality == q)
        return 1;
    [g_gi.irr release], [g_gi.dis release], [g_gi.ps release], [g_gi.pc release], [g_gi.rb release];
    g_gi.irr = g_gi.dis = nil, g_gi.ps = g_gi.pc = g_gi.rb = nil;
    int32_t d[3];
    float sp, hyst;
    uint32_t rays, stride;
    gi_preset(q, d, &sp, &rays, &stride, &hyst);
    uint32_t np = (uint32_t)(d[0] * d[1] * d[2]);
    for (int k = 0; k < 2; ++k)
    {
        uint32_t ts = k ? GI_DIS + 2 : GI_IRR + 2;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:k ? MTLPixelFormatRG16Float : MTLPixelFormatRGBA16Float
                                                                                     width:(NSUInteger)(d[0] * d[1]) * ts
                                                                                    height:(NSUInteger)d[2] * ts
                                                                                 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        *(k ? &g_gi.dis : &g_gi.irr) = [g_dev newTextureWithDescriptor:td];
    }
    g_gi.ps = [g_dev newBufferWithLength:np * 4 options:MTLResourceStorageModeShared];
    g_gi.pc = [g_dev newBufferWithLength:np * 16 options:MTLResourceStorageModeShared];
    g_gi.rb = [g_dev newBufferWithLength:(NSUInteger)((np + stride - 1) / stride) * rays * 16 options:MTLResourceStorageModePrivate];
    if (!g_gi.irr || !g_gi.dis || !g_gi.ps || !g_gi.pc || !g_gi.rb)
        return 0;
    memset([g_gi.ps contents], 0, np * 4), memset([g_gi.pc contents], 0, np * 16);
    g_gi.quality = q, g_gi.updated = 0, g_gi.phase = 0;
    fprintf(stderr, "[recomp] gfx: scene bounce light: %dx%dx%d probes %.1f apart, %u rays, 1 in %u a frame, %.1f MB\n", d[0],
        d[1], d[2], (double)sp, rays, stride,
        (double)((d[0] * d[1] * 8) * (d[2] * 8) * 8 + (d[0] * d[1] * 16) * (d[2] * 16) * 4 + np * 20 +
                    (np + stride - 1) / stride * rays * 16) / 1048576.0);
    return 1;
}

static uint32_t gi_rand(void)
{
    g_gi.seed = g_gi.seed * 1664525u + 1013904223u;
    return g_gi.seed;
}

/* a random rotation (a random unit quaternion) into u->rot, as columns */
static void gi_rotation(float* m)
{
    float r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = (float)(gi_rand() >> 8) / 16777216.0f;
    float a = sqrtf(1.0f - r[0]), b = sqrtf(r[0]), t1 = 6.2831853f * r[1], t2 = 6.2831853f * r[2];
    float x = a * sinf(t1), y = a * cosf(t1), z = b * sinf(t2), w = b * cosf(t2);
    float R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
        2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
    memset(m, 0, 64);
    for (int c = 0; c < 3; ++c)
        for (int rr = 0; rr < 3; ++rr)
            m[c * 4 + rr] = R[rr * 3 + c];
    m[15] = 1.0f;
}

void gfx_gi_frame(const GfxGiFrame* f)
{
    g_gi.on = 0;
    if (!g_dev || !f || f->strength <= 0.0f || !g_gi.as || !gi_init())
        return;
    @autoreleasepool
    {
        uint32_t q = f->quality >= 2 ? 2 : 1;
        g_gi.u.org[0] = f->quality >= 3 ? 1.0f : 0.0f;
        if (!gi_probes(q))
            return;
        GiU* u = &g_gi.u;
        int32_t d[3];
        float sp, hyst;
        uint32_t rays, stride;
        gi_preset(q, d, &sp, &rays, &stride, &hyst);
        u->org[3] = sp;
        for (int a = 0; a < 3; ++a)
            u->dims[a] = d[a], u->base[a] = (int32_t)floorf(f->cam[a] / sp + 0.5f) - d[a] / 2;
        u->base[3] = 1, u->dims[3] = d[0] * d[1] * d[2];
        u->k[0] = f->strength, u->k[1] = f->sky, u->k[2] = f->debug, u->k[3] = 0.25f * sp;
        u->inv[0] = 1.0f / (float)(d[0] * d[1] * (GI_IRR + 2)), u->inv[1] = 1.0f / (float)(d[2] * (GI_IRR + 2));
        u->inv[2] = 1.0f / (float)(d[0] * d[1] * (GI_DIS + 2)), u->inv[3] = 1.0f / (float)(d[2] * (GI_DIS + 2));
        if (g_gi.updated != g_serial) /* the probes traced once a frame, at its first batch */
        {
            u->tr[0] = 16.0f * sp, u->tr[1] = hyst, u->tr[2] = f->bounce, u->tr[3] = f->ambient_bounce;
            /* the sun's visibility at a hit from the shadows' zone layer when it is there (one read), else a ray */
            u->org[1] = g_shd.on && g_shd.zmap ? 1.0f : 0.0f;
            uint32_t nl = f->nlight < 64 ? f->nlight : 64;
            u->n[0] = rays, u->n[1] = stride, u->n[2] = g_gi.phase, u->n[3] = nl;
            memcpy(u->sun_dir, f->sun_dir, 16), memcpy(u->sun_col, f->sun_col, 16);
            memcpy(u->sun2_dir, f->sun2_dir, 16), memcpy(u->sun2_col, f->sun2_col, 16), memcpy(u->amb, f->ambient, 16);
            gi_rotation(u->rot);
            g_gi.phase = (g_gi.phase + 1) % stride;
            g_gi.updated = g_serial;
            static uint32_t told;
            if (++told % 2000 == 1)
                fprintf(stderr, "[recomp] gfx: scene bounce light: grid at (%d, %d, %d), %u lamps, sun %.2f %.2f %.2f, second %.2f %.2f %.2f, "
                                "ambient %.2f %.2f %.2f, zone layer %s\n",
                    u->base[0], u->base[1], u->base[2], nl, u->sun_col[0] * u->sun_dir[3], u->sun_col[1] * u->sun_dir[3],
                    u->sun_col[2] * u->sun_dir[3], u->sun2_col[0] * u->sun2_dir[3], u->sun2_col[1] * u->sun2_dir[3],
                    u->sun2_col[2] * u->sun2_dir[3], u->amb[0], u->amb[1], u->amb[2], u->org[1] > 0.0f ? "read" : "not there");
            uint32_t np = (uint32_t)u->dims[3], slots = (np + stride - 1) / stride;
            id<MTLBuffer> lb;
            NSUInteger lo;
            GfxWorldLight none;
            memset(&none, 0, sizeof none);
            memcpy(ring((nl ? nl : 1) * sizeof(GfxWorldLight), 256, &lb, &lo), nl ? f->light : &none, (nl ? nl : 1) * sizeof(GfxWorldLight));
            /* a command buffer of its own, committed now, ahead of the frame's draws (as the shadows' zone layer) */
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
            gi_ts_mark(cp, GFX_PASS_GI);
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoderWithDescriptor:cp];
            [e setBytes:u length:sizeof *u atIndex:0];
            [e setBuffer:g_gi.ps offset:0 atIndex:1];
            [e setBuffer:g_gi.pc offset:0 atIndex:2];
            [e setComputePipelineState:g_gi.scroll];
            [e dispatchThreads:MTLSizeMake(np, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [e setComputePipelineState:g_gi.trace];
            [e setBuffer:g_gi.attr offset:0 atIndex:2];
            [e setBuffer:lb offset:lo atIndex:3];
            [e setBuffer:g_gi.rb offset:0 atIndex:4];
            [e setAccelerationStructure:g_gi.as atBufferIndex:5];
            [e setTexture:g_gi.irr atIndex:0];
            [e setTexture:g_gi.dis atIndex:1];
            [e setBytes:&g_shd.u length:sizeof g_shd.u atIndex:6];
            [e setTexture:g_shd.zmap atIndex:2];
            [e dispatchThreads:MTLSizeMake(rays, slots, 1) threadsPerThreadgroup:MTLSizeMake(rays < 64 ? rays : 64, 1, 1)];
            [e memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            [e setComputePipelineState:g_gi.bdis]; /* first: the irradiance's pass marks the probes traced */
            [e setTexture:g_gi.dis atIndex:0];
            [e dispatchThreadgroups:MTLSizeMake(slots, 1, 1) threadsPerThreadgroup:MTLSizeMake(GI_DIS + 2, GI_DIS + 2, 1)];
            [e memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            [e setComputePipelineState:g_gi.birr];
            [e setTexture:g_gi.irr atIndex:0];
            [e dispatchThreadgroups:MTLSizeMake(slots, 1, 1) threadsPerThreadgroup:MTLSizeMake(GI_IRR + 2, GI_IRR + 2, 1)];
            [e endEncoding];
            if (gfx_pass_timing_on())
                [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    uint64_t ns = (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9);
                    atomic_fetch_add(&g_gpu_ns, ns);
                    gfx_pass_gpu(ns);
                }];
            [cb commit];
        }
        g_gi.on = 1;
    }
}

/* the bounce light for a batch's draws: the probes at buffers 8 (GU) and 9, textures 4 and 5, both stages; 0 off */
static int gi_bind(id<MTLRenderCommandEncoder> e)
{
    if (!g_gi.on)
        return 0;
    [e setVertexBytes:&g_gi.u length:sizeof g_gi.u atIndex:8];
    [e setFragmentBytes:&g_gi.u length:sizeof g_gi.u atIndex:8];
    [e setVertexBuffer:g_gi.ps offset:0 atIndex:9];
    [e setFragmentBuffer:g_gi.ps offset:0 atIndex:9];
    [e setVertexTexture:g_gi.irr atIndex:4];
    [e setFragmentTexture:g_gi.irr atIndex:4];
    [e setVertexTexture:g_gi.dis atIndex:5];
    [e setFragmentTexture:g_gi.dis atIndex:5];
    return 1;
}
