// Ray tracing (gfx_d3d12.c's rt_*): the scene effects' passes that trace rays through the world the
// frame's casters make. Built with dxc at build time (tools/build.py rt_shaders: shader model 6.6, for
// inline ray queries and the descriptor heap read directly) into generated/gfx_rt_dxil.h; the rest of
// the effects are gfx_hlsl.c's, built at run time. Bound as those are (gfx_d3d12.c fx_pass): FxU at b0,
// the pass's heap indices at b1 - ft[i] the i-th input, any resource in the heap (the world's
// acceleration structure and triangles too), fsm[0].x the linear-clamp sampler's slot.
//
// The world (rt_capture): every caster's triangles, through its own vertex function, in this frame's
// view space (x right, y up, z ahead times hand), in one structure of two geometries - the solid casters'
// triangles, then an alpha test's (leaves, hair, grass: non-opaque, tested where a ray meets them). Each
// corner two float4s in `tri`: its position, and its first texture coordinates (an alpha test's only).
// rtp: the solid triangles' count, the alpha-tested casters' table (an SRV's heap index: each its first
// triangle, its texture's heap index, its sampler's slot, its alpha test - D3DCMPFUNC << 8 | reference),
// `tri`'s heap index, the table's count.

struct FxU
{
    // gfx_d3d12.c's FxU, field for field (gfx_hlsl.c's gfx_hlsl_fx says what each holds)
    float4 proj, zp, vp, size, ao, grade, hand, up, sun, suncol, sunuv, fogc, fogp, bloom, rays, shadow;
    float4x4 lmat;
    float4 smap, smap2;
    float4x4 reproj;
    float4 hist;
    float4x4 lmatn;
    float4 smapn, smapn2, aop;
    float4x4 gimat, giinv;
    float4 gi, gip;
    uint4 rtp;
};
cbuffer CU : register(b0) { FxU u; };
cbuffer FB : register(b1) { uint4 ft[2]; uint4 fsm[2]; };
SamplerState smp[] : register(s0);
#define IN(i) ft[(i) >> 2][(i) & 3]
#define LIN smp[fsm[0].x]

struct FO
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FO rt_vs(uint vid : SV_VertexID)
{
    float2 p = float2((vid << 1) & 2, vid & 2);
    FO o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

float view_z(float d)
{
    d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);
    float z = u.zp.y / (d * u.hand.x - u.zp.x);
    return d >= 0.999999 || !(z * u.hand.x > 0.0) ? 0.0 : z;
}

float3 view_pos(float2 px, float z)
{
    px -= 0.5;
    float2 ndc = float2((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);
    float w = z * u.hand.x;
    return float3((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);
}

// the ray through pixel px from the camera (view space), as view_pos makes the point it shows
float3 view_dir(float2 px)
{
    return normalize(view_pos(px, u.hand.x));
}

// corner k of triangle prim: its position
float3 corner(StructuredBuffer<float4> tri, uint prim, uint k) { return tri[(prim * 3 + k) * 2].xyz; }

// the triangle a ray hit: its face's normal, turned toward the ray's origin
float3 tri_normal(StructuredBuffer<float4> tri, uint prim, float3 dir)
{
    float3 a = corner(tri, prim, 0), b = corner(tri, prim, 1), c = corner(tri, prim, 2);
    float3 n = cross(b - a, c - a);
    n = dot(n, n) > 1e-20 ? normalize(n) : -dir;
    return dot(n, dir) > 0.0 ? -n : n;
}

// The surface's own smooth normal where a ray hit it: the three vertices' (rt_nresolve's: the faces round
// each corner averaged, but across a crease), across the triangle by where it was hit (bary), on the side
// the ray came from (geo, the face's, already turned to it)
float3 smooth_normal(StructuredBuffer<float4> nrm, uint prim, float2 bary, float3 geo)
{
    float3 n = nrm[prim * 3].xyz * (1.0 - bary.x - bary.y) + nrm[prim * 3 + 1].xyz * bary.x + nrm[prim * 3 + 2].xyz * bary.y;
    if (dot(n, n) < 1e-8)
        return geo;
    n = normalize(n);
    return dot(n, geo) < 0.0 ? -n : n;
}

// Does an alpha-tested triangle (prim, of all of them) let a ray through where it meets it (bary)? Its
// caster found in the table by its first triangle; its texture read there, at its corners' coordinates,
// and tested as the draw tests it.
bool alpha_holds(uint prim, float2 bary)
{
    StructuredBuffer<uint4> tab = ResourceDescriptorHeap[u.rtp.y];
    StructuredBuffer<float4> tri = ResourceDescriptorHeap[u.rtp.z];
    uint lo = 0, hi = u.rtp.w;
    while (hi - lo > 1)
    {
        uint mid = (lo + hi) / 2;
        if (tab[mid].x <= prim)
            lo = mid;
        else
            hi = mid;
    }
    uint4 e = tab[lo];
    float2 uv = tri[(prim * 3) * 2 + 1].xy * (1.0 - bary.x - bary.y) + tri[(prim * 3 + 1) * 2 + 1].xy * bary.x +
        tri[(prim * 3 + 2) * 2 + 1].xy * bary.y;
    Texture2D t = ResourceDescriptorHeap[e.y];
    float a = t.SampleLevel(smp[e.z], uv, 0).a * 255.0, ref = float(e.w & 255u);
    switch (e.w >> 8)
    {
    case 1: return false;
    case 2: return a < ref;
    case 3: return abs(a - ref) < 0.5;
    case 4: return a <= ref;
    case 5: return a > ref;
    case 6: return abs(a - ref) >= 0.5;
    case 7: return a >= ref;
    default: return true;
    }
}

// the nearest hit along a ray, t in [tmin, tmax]; prim the triangle (of all of them; -1 for none), bary
// where on it
float trace(RaytracingAccelerationStructure world, float3 o, float3 d, float tmin, float tmax, out int prim, out float2 bary)
{
    RayDesc r;
    r.Origin = o, r.Direction = d, r.TMin = tmin, r.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(world, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alpha_holds(u.rtp.x + q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    prim = -1, bary = float2(0, 0);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return tmax;
    prim = (int)(q.CommittedPrimitiveIndex() + (q.CommittedInstanceID() ? u.rtp.x : 0u));
    bary = q.CommittedTriangleBarycentrics();
    return q.CommittedRayT();
}

float trace(RaytracingAccelerationStructure world, float3 o, float3 d, float tmin, float tmax, out int prim)
{
    float2 bary;
    return trace(world, o, d, tmin, tmax, prim, bary);
}

// the nearest hit among the solid triangles alone: the bounce light's and the occlusion's rays pass the
// zone's alpha-tested leaves and grass untested (a field of grass blades, each tested, cost a ray tracer
// most of a frame at Lufaise; light through a canopy is the sun's maps' to give)
float trace_solid(RaytracingAccelerationStructure world, float3 o, float3 d, float tmin, float tmax, out int prim)
{
    RayDesc r;
    r.Origin = o, r.Direction = d, r.TMin = tmin, r.TMax = tmax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(world, RAY_FLAG_NONE, 0x01, r); /* (the solid instance alone) */
    q.Proceed();
    prim = -1;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return tmax;
    prim = q.CommittedPrimitiveIndex();
    return q.CommittedRayT();
}

// is anything between o + d * tmin and o + d * tmax?
bool blocked(RaytracingAccelerationStructure world, float3 o, float3 d, float tmin, float tmax)
{
    RayDesc r;
    r.Origin = o, r.Direction = d, r.TMin = tmin, r.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(world, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alpha_holds(u.rtp.x + q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

static const uint BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

// a pixel's two numbers for ray i this frame: a 4x4 ordered pattern turned by the frame (hist.y) and the
// ray, the second from a hash of the pixel along an R2 sequence
float2 pattern(float2 pos, int i)
{
    int2 cell = int2(pos) & 3;
    float k = frac((float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0 + u.hist.y + float(i) * 0.6180340);
    uint h = (uint(pos.x) * 73856093u) ^ (uint(pos.y) * 19349663u);
    float j = frac(float(h & 1023u) / 1024.0 + u.hist.y * 1.3247180 + float(i) * 0.7548777);
    return float2(k, j);
}

// two axes across N
void basis(float3 N, out float3 t, out float3 s)
{
    t = abs(N.y) < 0.99 ? normalize(cross(N, float3(0, 1, 0))) : normalize(cross(N, float3(1, 0, 0)));
    s = cross(N, t);
}

// a direction over the hemisphere round N, cosine-weighted, from two numbers in [0, 1)
float3 hemi(float3 N, float a, float b)
{
    float3 t, s;
    basis(N, t, s);
    float r = sqrt(a), phi = 6.2831853 * b;
    return normalize(t * (r * cos(phi)) + s * (r * sin(phi)) + N * sqrt(max(1.0 - a, 0.0)));
}

// How much of the sun reaches p (n its smooth normal, g its face's): rays toward points across the sun's
// disc - widened to a few degrees, as the shadow maps' penumbra is (sun_soft), so an edge far from its
// caster is soft - from just off the surface. What stands within sun_min (smap2.z) of p is passed through:
// a character's clothes, a few centimetres over its body, shaded it in blocks along its polygons, and the
// low-polygon body itself shadows its own smooth side near the turn from the sun (the terminator). The
// smooth normal's turn from the sun fades it out as the face's would, but without the facets.
float sun_seen(RaytracingAccelerationStructure world, float3 p, float3 n, float3 g, float2 pos, int rays)
{
    float nl = dot(n, u.sun.xyz);
    if (nl <= 0.0)
        return 0.0;
    float3 t, s;
    basis(u.sun.xyz, t, s);
    float3 o = p + g * 0.02 + n * 0.03;
    float spread = 0.035, tmin = max(u.smap2.z, 0.05), lit = 0.0;
    for (int i = 0; i < rays; ++i)
    {
        float2 r = pattern(pos, i + 3);
        float rr = sqrt(r.x) * spread, phi = 6.2831853 * r.y;
        float3 d = normalize(u.sun.xyz + t * (rr * cos(phi)) + s * (rr * sin(phi)));
        lit += blocked(world, o, d, tmin, 500.0) ? 0.0 : 1.0;
    }
    return smoothstep(0.0, 0.2, nl) * lit / float(rays);
}

// debug=clay (8): a ray through each pixel against the world, the hit lit from the sun by its smooth
// normal, its shadow soft (sun_seen, eight rays) - grey where it lands where the drawn depth says, red
// nearer than it, blue farther or missed, yellow on the sky. Inputs: the depth (0), the world (1), its
// triangles (2), their normals (3).
float4 rt_clay(FO fi) : SV_Target
{
    Texture2D dt = ResourceDescriptorHeap[IN(0)];
    RaytracingAccelerationStructure world = ResourceDescriptorHeap[IN(1)];
    StructuredBuffer<float4> tri = ResourceDescriptorHeap[IN(2)];
    StructuredBuffer<float4> nrm = ResourceDescriptorHeap[IN(3)];
    float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;
    float z = view_z(dt.Load(int3(int2(px), 0)).r);
    float drawn = z != 0.0 ? length(view_pos(px, z)) : 0.0;
    float3 d = view_dir(px);
    int prim;
    float2 bary;
    float t = trace(world, float3(0, 0, 0), d, 0.05, 2000.0, prim, bary);
    if (prim < 0)
        return drawn > 0.0 ? float4(0.1, 0.2, 0.9, 1) : float4(0.35, 0.45, 0.6, 1);
    float3 g = tri_normal(tri, (uint)prim, d), n = smooth_normal(nrm, (uint)prim, bary, g), p = d * t;
    float sky = 0.25 + 0.15 * dot(n, u.up.xyz);
    float lit = sky + (u.sun.w > 0.0 ? 0.7 * saturate(dot(n, u.sun.xyz)) * sun_seen(world, p, n, g, fi.pos.xy, 8) : 0.0);
    float3 c = lit.xxx;
    if (drawn <= 0.0)
        c *= float3(1.0, 0.85, 0.2);
    else if (abs(t - drawn) > max(0.05 * drawn, 0.3))
        c *= t < drawn ? float3(1.0, 0.3, 0.3) : float3(0.3, 0.4, 1.0);
    return float4(c, 1);
}

// --- the bounce light traced (rt_gi) -----------------------------------------------------------------------------
// gfx_hlsl.c's fx_gi gathers the light the sunlit surfaces near a point throw onto it from the sun's map,
// with nothing between them. Here rays go out from the point instead, over the hemisphere its normal
// faces (cosine-weighted), and each takes the light of the first surface it meets: as the scene shows
// it where that surface is on screen (the frame's own colour, darkened where the sun's maps shade it), as
// the sun sees it where it is in the bounce light's map and lit, and none where neither sees it. A ray
// that meets nothing within the reach brings nothing (the game's own lighting has the sky). Inputs: the
// depth (0), the world (1), its triangles (2), the scene as drawn (3), the occlusion and sun (4: fx_ao's,
// sun in z), the bounce map's depth (5) and colour (6). rgb the light, a the distance.

float3 pos_at(Texture2D dt, float2 px)
{
    px = clamp(px, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);
    px = floor(px) + 0.5;
    return view_pos(px, view_z(dt.Load(int3(int2(px), 0)).r));
}

// the surface's normal at px (P its point) from the depth: each way the neighbour nearer in depth
float3 normal_at(Texture2D dt, float2 px, float3 P)
{
    float3 r = pos_at(dt, px + float2(1, 0)) - P, l = P - pos_at(dt, px - float2(1, 0));
    float3 d = pos_at(dt, px + float2(0, 1)) - P, t = P - pos_at(dt, px - float2(0, 1));
    float3 dx = (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;
    float3 dy = (abs(d.z) < abs(t.z) && dot(d, d) > 0.0) || dot(t, t) == 0.0 ? d : t;
    float3 nc = cross(dx, dy);
    float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);
    return dot(N, P) > 0.0 ? -N : N;
}

// the light leaving the surface at Q (view space) toward a ray, as the frame or the sun saw it; 0 unseen
float3 radiance(float3 Q, Texture2D dt, Texture2D src, Texture2D occ, Texture2D gd, Texture2D gc)
{
    float qd = Q.z * u.hand.x;
    if (qd > 0.05)
    {
        float2 ndc = float2(Q.x * u.proj.x + Q.z * u.proj.z, Q.y * u.proj.y + Q.z * u.proj.w) / qd;
        float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;
        if (all(q >= u.vp.xy) && all(q < u.vp.xy + u.vp.zw - 1.0))
        {
            float sd = view_z(dt.Load(int3(int2(q + 0.5), 0)).r) * u.hand.x;
            if (sd > 0.0 && abs(sd - qd) < 0.03 * qd + 0.1)
            {
                float3 c = src.Load(int3(int2(q + 0.5), 0)).rgb;
                float2 ouv = (q - u.vp.xy) / u.vp.zw;
                float sun = occ.SampleLevel(LIN, ouv, 0).z;
                return c * lerp(1.0, sun, u.smap.x);
            }
        }
    }
    if (u.gi.x > 0.0)
    {
        float4 lc = mul(u.gimat, float4(Q, 1.0));
        if (all(abs(lc.xy) < 0.98) && lc.z < 1.0)
        {
            float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
            uint w, h;
            gd.GetDimensions(w, h);
            float z = gd.Load(int3(int2(uv * float2(w, h)), 0)).r;
            if (lc.z - z < 0.002)
                return gc.SampleLevel(LIN, uv, 0).rgb;
        }
    }
    return float3(0, 0, 0);
}

float4 rt_gi(FO fi) : SV_Target
{
    Texture2D dt = ResourceDescriptorHeap[IN(0)];
    RaytracingAccelerationStructure world = ResourceDescriptorHeap[IN(1)];
    StructuredBuffer<float4> tri = ResourceDescriptorHeap[IN(2)];
    Texture2D src = ResourceDescriptorHeap[IN(3)];
    Texture2D occ = ResourceDescriptorHeap[IN(4)];
    Texture2D gd = ResourceDescriptorHeap[IN(5)];
    Texture2D gc = ResourceDescriptorHeap[IN(6)];
    float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;
    float3 P = pos_at(dt, px);
    float dist = P.z * u.hand.x;
    if (dist <= 0.0)
        return float4(0, 0, 0, 0);
    float fade = 1.0 - smoothstep(0.7 * u.gip.w, u.gip.w, dist);
    if (fade <= 0.0)
        return float4(0, 0, 0, dist);
    float3 N = normal_at(dt, px, P);
    float3 o = P + N * (0.02 + 0.002 * dist);
    int NS = max(int(u.gip.z), 1);
    float3 sum = float3(0, 0, 0);
    for (int i = 0; i < NS; ++i)
    {
        float2 r = pattern(fi.pos.xy, i);
        float3 d = hemi(N, r.x, r.y);
        int prim;
        float t = trace_solid(world, o, d, 0.0, u.gi.z, prim);
        if (prim < 0)
            continue;
        float3 Q = o + d * t, nq = tri_normal(tri, (uint)prim, d);
        sum += radiance(Q + nq * 0.02, dt, src, occ, gd, gc);
    }
    return float4(sum * (u.gi.x * fade / float(NS)), dist);
}

// The occlusion traced (rt_ao): fx_ao's result (0) with its occlusion (x) traced instead - rays over
// the hemisphere, each blocked within the radius (ao.x) darkening by how near it is blocked. The depth at
// 1, the world at 2.
float4 rt_ao(FO fi) : SV_Target
{
    Texture2D ao = ResourceDescriptorHeap[IN(0)];
    Texture2D dt = ResourceDescriptorHeap[IN(1)];
    RaytracingAccelerationStructure world = ResourceDescriptorHeap[IN(2)];
    float4 c = ao.Load(int3(int2(fi.pos.xy), 0));
    if (c.y <= 0.0)
        return c;
    float2 px = floor(u.vp.xy + fi.uv * u.vp.zw) + 0.5;
    float3 P = pos_at(dt, px);
    float3 N = normal_at(dt, px, P);
    float3 o = P + N * (0.01 + 0.002 * c.y);
    float R = u.ao.x * 1.5, occl = 0.0;
    const int NS = 4;
    for (int i = 0; i < NS; ++i)
    {
        float2 r = pattern(fi.pos.xy, i + 7);
        float3 d = hemi(N, r.x, r.y);
        int prim;
        float t = trace_solid(world, o, d, 0.0, R, prim);
        if (prim >= 0)
        {
            float f = 1.0 - t / R;
            occl += f * f;
        }
    }
    c.x = saturate(1.0 - 1.2 * occl / float(NS));
    return c;
}

// The bounce light smoothed (rt_giblur): a 5x5 at the step DIR (fsm[1].xy), each texel by how near its
// distance is to this one's
float4 rt_giblur(FO fi) : SV_Target
{
    Texture2D g = ResourceDescriptorHeap[IN(0)];
    int2 p = int2(fi.pos.xy), stp = int2(asint(fsm[1].x), asint(fsm[1].y));
    uint w, h;
    g.GetDimensions(w, h);
    int2 hi = int2(w, h) - 1;
    float4 c = g.Load(int3(p, 0));
    if (c.a <= 0.0)
        return c;
    static const float K[3] = { 0.375, 0.25, 0.0625 };
    float3 s = float3(0, 0, 0);
    float sw = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            float4 t = g.Load(int3(clamp(p + int2(x, y) * stp, int2(0, 0), hi), 0));
            float k = K[abs(x)] * K[abs(y)] * (t.a > 0.0 ? saturate(1.0 - abs(t.a - c.a) / (0.04 * c.a)) : 0.0);
            s += t.rgb * k, sw += k;
        }
    return float4(sw > 0.0 ? s / sw : c.rgb, c.a);
}

// --- the world's smooth normals (rt_nclear, rt_nsum, rt_nresolve: compute, after each capture) ----------------
// The capture streams out positions alone: a vertex function's normal is in whatever space its draw lights
// in, if it has one. So the corners of the world's triangles that share a place (to 1/256 of a unit) are
// found through a hash table, each place summing the faces' normals round it (each a unit vector, in
// 1/4096ths: integer atomics), and each corner then takes its place's average - or its own face's where
// that turns more than 60 degrees from it (a crease: a box's edge stays an edge). Root constants: the
// triangles (an SRV's heap index), the table and the normals (UAVs'), the count of triangles, the table's
// mask (its slots less one, a power of two), the corners' count.
cbuffer NC : register(b0) { uint n_tri, n_table, n_nrm, n_tris, n_mask, n_verts, n_pad0, n_pad1; };

uint3 n_key(float3 p) { return uint3(int3(floor(p * 256.0 + 0.5))); }
uint n_hash(uint3 k) { return (k.x * 73856093u) ^ (k.y * 19349663u) ^ (k.z * 83492791u); }
uint n_tag(uint3 k) { return ((k.x * 2654435761u) ^ (k.y * 2246822519u) ^ (k.z * 3266489917u)) | 1u; }

// the slot of p's place (made when it has none; n_mask + 1 when the table is full)
uint n_slot(RWByteAddressBuffer table, float3 p, bool make)
{
    uint3 k = n_key(p);
    uint h = n_hash(k) & n_mask, tag = n_tag(k);
    for (uint i = 0; i < 64; ++i)
    {
        uint slot = (h + i) & n_mask, was;
        if (make)
            table.InterlockedCompareExchange(slot * 16, 0, tag, was);
        else
            was = table.Load(slot * 16);
        if (was == tag || (make && was == 0))
            return slot;
        if (!make && was == 0)
            break;
    }
    return n_mask + 1;
}

[numthreads(256, 1, 1)] void rt_nclear(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[n_table];
    if (id.x <= n_mask)
        table.Store4(id.x * 16, uint4(0, 0, 0, 0));
}

[numthreads(256, 1, 1)] void rt_nsum(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= n_tris)
        return;
    StructuredBuffer<float4> tri = ResourceDescriptorHeap[n_tri];
    RWByteAddressBuffer table = ResourceDescriptorHeap[n_table];
    float3 a = corner(tri, id.x, 0), b = corner(tri, id.x, 1), c = corner(tri, id.x, 2);
    float3 f = cross(b - a, c - a);
    if (!(dot(f, f) > 1e-14))
        return;
    int3 q = int3(normalize(f) * 4096.0);
    float3 v[3] = { a, b, c };
    for (int k = 0; k < 3; ++k)
    {
        uint slot = n_slot(table, v[k], true), was;
        if (slot > n_mask)
            continue;
        table.InterlockedAdd(slot * 16 + 4, (uint)q.x, was);
        table.InterlockedAdd(slot * 16 + 8, (uint)q.y, was);
        table.InterlockedAdd(slot * 16 + 12, (uint)q.z, was);
    }
}

[numthreads(256, 1, 1)] void rt_nresolve(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= n_verts)
        return;
    StructuredBuffer<float4> tri = ResourceDescriptorHeap[n_tri];
    RWByteAddressBuffer table = ResourceDescriptorHeap[n_table];
    RWStructuredBuffer<float4> nrm = ResourceDescriptorHeap[n_nrm];
    uint t0 = id.x - id.x % 3;
    float3 a = tri[t0 * 2].xyz, b = tri[(t0 + 1) * 2].xyz, c = tri[(t0 + 2) * 2].xyz;
    float3 f = cross(b - a, c - a);
    if (!(dot(f, f) > 1e-14))
    {
        nrm[id.x] = float4(0, 0, 0, 0);
        return;
    }
    f = normalize(f);
    float3 n = f;
    uint slot = n_slot(table, tri[id.x * 2].xyz, false);
    if (slot <= n_mask)
    {
        float3 sum = float3(int3(table.Load3(slot * 16 + 4)));
        if (dot(sum, sum) > 1.0)
        {
            sum = normalize(sum);
            n = dot(sum, f) > 0.5 ? sum : f;
        }
    }
    nrm[id.x] = float4(n, 0);
}
