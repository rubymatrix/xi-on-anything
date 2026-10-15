/* Opt-in one-frame capture. JSONL metadata + little-endian, pointer-free binary payloads.
 * Synchronous by design: diagnostics only, bounded to 256 MiB and 10000 draw packets. */
static struct
{
    FILE *json, *bin;
    uint64_t session, bytes;
    uint32_t sequence, draws, frame;
    int failed;
} g_zonecap;

static void zonecap_blob(const void* data, size_t size)
{
    uint64_t offset = g_zonecap.bytes;
    if (size && (!data || size > 256u * 1024u * 1024u - g_zonecap.bytes))
        g_zonecap.failed = 1;
    if (!g_zonecap.failed && size && fwrite(data, 1, size, g_zonecap.bin) != size)
        g_zonecap.failed = 1;
    if (g_zonecap.failed)
        fprintf(g_zonecap.json, "null");
    else
    {
        g_zonecap.bytes += size;
        fprintf(g_zonecap.json, "{\"offset\":%llu,\"size\":%llu}",
            (unsigned long long)offset, (unsigned long long)size);
    }
}

static void zonecap_field(const char* name, const void* data, size_t size)
{
    fprintf(g_zonecap.json, ",\"%s\":", name);
    zonecap_blob(data, size);
}

static void zonecap_object(uint32_t handle)
{
    Obj* o = obj(handle);
    if (!o)
    {
        fputs("null", g_zonecap.json);
        return;
    }
    fprintf(g_zonecap.json, "{\"id\":%llu,\"guest\":%u,\"kind\":%u,\"format\":%u,\"usage\":%u,"
        "\"width\":%u,\"height\":%u,\"levels\":%u,\"face\":%u,\"level\":%u,\"container\":%llu,"
        "\"origin\":{\"load\":%llu,\"resource\":%u,\"fourcc\":%u,\"parents\":[%u,%u,%u,%u]}}",
        (unsigned long long)o->capture_id, o->guest, o->kind, o->format, o->usage,
        o->width, o->height, o->levels, o->face, o->level,
        (unsigned long long)(obj(o->container) ? obj(o->container)->capture_id : 0),
        (unsigned long long)o->origin.load, o->origin.resource, o->origin.fourcc,
        o->origin.parents[0], o->origin.parents[1], o->origin.parents[2], o->origin.parents[3]);
}

static void zonecap_targets(const char* type)
{
    fprintf(g_zonecap.json, "{\"type\":\"%s\",\"sequence\":%u,\"target\":", type, g_zonecap.sequence++);
    zonecap_object(g_dev.rt);
    fputs(",\"depth_target\":", g_zonecap.json);
    zonecap_object(g_dev.ds);
}

int d3d8_zone_capture(const char* path, uint32_t zone)
{
    uint32_t endian = 1;
    if (!path || !*path || strlen(path) > 980 || g_zonecap.json || *(uint8_t*)&endian != 1)
        return 0;
    char binpath[1024];
    snprintf(binpath, sizeof binpath, "%s.bin", path);
    FILE* bin = fopen(binpath, "wb");
    if (!bin)
        return 0;
    FILE* json = fopen(path, "w");
    if (!json)
    {
        fclose(bin);
        return 0;
    }
    g_zonecap.json = json, g_zonecap.bin = bin;
    g_zonecap.bytes = 0, g_zonecap.sequence = 0, g_zonecap.draws = 0, g_zonecap.failed = 0;
    ++g_zonecap.session;
    fprintf(json, "{\"type\":\"header\",\"version\":1,\"endian\":\"little\",\"zone_label\":%u,"
        "\"frame\":%u,\"image_timestamp\":%u,\"image_size\":%u,"
        "\"scope\":\"submitted-frame-not-complete-zone\",\"max_bytes\":268435456}\n",
        zone, g_zonecap.frame, rt_image_timestamp, rt_image_size);
    return 1;
}

static void zonecap_present(void)
{
    ++g_zonecap.frame;
    if (g_zonecap.json)
    {
        int ok = !g_zonecap.failed && !ferror(g_zonecap.json) && !ferror(g_zonecap.bin);
        if (fclose(g_zonecap.bin))
            ok = 0;
        fprintf(g_zonecap.json, "{\"type\":\"end\",\"draws\":%u,\"bytes\":%llu,\"complete\":%s}\n",
            g_zonecap.draws, (unsigned long long)g_zonecap.bytes, ok ? "true" : "false");
        if (fclose(g_zonecap.json) || !ok)
            rt_log("[recomp] zone capture incomplete\n");
        g_zonecap.json = g_zonecap.bin = NULL;
        return;
    }
    const char* path = getenv("FFXI_ZONE_CAPTURE");
    if (!path || !*path || strlen(path) > 980)
        return;
    char trigger[1024];
    snprintf(trigger, sizeof trigger, "%s.go", path);
    FILE* f = fopen(trigger, "r");
    if (!f)
        return;
    uint32_t zone = 0;
    int valid = fscanf(f, "%u", &zone) == 1 && zone > 0 && zone <= 65535;
    fclose(f);
    if (valid && d3d8_zone_capture(path, zone))
        remove(trigger);
    else
        rt_log("[recomp] zone capture request failed: %s\n", trigger);
}

static void zonecap_clear(uint32_t n, const void* rects, uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil)
{
    if (!g_zonecap.json || g_zonecap.failed)
        return;
    zonecap_targets("clear");
    fprintf(g_zonecap.json, ",\"flags\":%u,\"color\":%u,\"z_bits\":%u,\"stencil\":%u", flags, color, z, stencil);
    zonecap_field("rects", rects, (size_t)n * 16);
    zonecap_field("viewport", g_dev.cur.vp, sizeof g_dev.cur.vp);
    fputs("}\n", g_zonecap.json);
}

static void zonecap_copy(uint32_t src, uint32_t dst, uint32_t n, uint32_t rects, uint32_t points)
{
    if (!g_zonecap.json || g_zonecap.failed)
        return;
    fprintf(g_zonecap.json, "{\"type\":\"copy_attempt\",\"sequence\":%u,\"source\":", g_zonecap.sequence++);
    zonecap_object(src);
    fputs(",\"destination\":", g_zonecap.json);
    zonecap_object(dst);
    zonecap_field("rects", rects ? GUEST_PTR(rects) : NULL, (size_t)n * 16);
    zonecap_field("points", points ? GUEST_PTR(points) : NULL, points ? (size_t)(n ? n : 1) * 8 : 0);
    fputs("}\n", g_zonecap.json);
}

static void zonecap_texture(Obj* surface)
{
    /* GPU-owned content must be reconstructed from ordered target events; stale guest memory
     * is never exported as if it were current. Managed texels retain their original format. */
    if (!surface || !surface->mem || gpu_owned(surface))
    {
        fputs("null", g_zonecap.json);
        return;
    }
    const uint8_t* data = GUEST_PTR(surface->mem);
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < surface->size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    if (surface->capture_texture_session == g_zonecap.session && surface->capture_texture_hash == hash)
        fprintf(g_zonecap.json, "{\"offset\":%llu,\"size\":%u}",
            (unsigned long long)surface->capture_texture_offset, surface->size);
    else
    {
        surface->capture_texture_offset = g_zonecap.bytes;
        zonecap_blob(data, surface->size);
        if (!g_zonecap.failed)
            surface->capture_texture_session = g_zonecap.session, surface->capture_texture_hash = hash;
    }
}

static void zonecap_draw(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t vertices,
    uint32_t indices, uint32_t index_size, uint32_t n, uint32_t up, uint32_t up_stride, int hidden_shadow)
{
    if (!g_zonecap.json || g_zonecap.failed)
        return;
    if (g_zonecap.draws >= 10000)
    {
        g_zonecap.failed = 1;
        return;
    }
    ++g_zonecap.draws;
    zonecap_targets("draw");
    fprintf(g_zonecap.json, ",\"primitive\":%u,\"count\":%u,\"first\":%u,\"vertices\":%u,\"index_size\":%u,"
        "\"index_rebase\":%d,\"rhw\":%u,\"water\":%u,\"hidden_shadow\":%s,\"vs_handle\":%u,\"ps_handle\":%u",
        prim, count, first, vertices, index_size, d->u.vofs, d->vs.rhw, d->water,
        hidden_shadow ? "true" : "false", g_dev.cur.vs, g_dev.cur.ps);
    fprintf(g_zonecap.json, ",\"translated\":{\"blend\":[%u,%u,%u,%u,%u],\"depth\":[%u,%u,%u,%u],"
        "\"zbias\":%d,\"fog\":[%u,%u],\"shader_hashes\":[%u,%u]}",
        d->pipe.blend, d->pipe.src, d->pipe.dst, d->pipe.op, d->pipe.write_mask,
        d->depth.zenable, d->depth.zwrite, d->depth.zfunc, d->depth.stencil,
        d->zbias, d->fs.fog, d->vs.fog_vertex, d->vs.prog, d->fs.prog);
    zonecap_field("render_states", g_dev.cur.rs, sizeof g_dev.cur.rs);
    zonecap_field("texture_stages", g_dev.cur.tss, sizeof g_dev.cur.tss);
    zonecap_field("transforms", g_dev.cur.xf, sizeof g_dev.cur.xf);
    zonecap_field("viewport", g_dev.cur.vp, sizeof g_dev.cur.vp);
    zonecap_field("material", g_dev.cur.mat, sizeof g_dev.cur.mat);
    zonecap_field("lights", g_dev.cur.light, sizeof g_dev.cur.light);
    zonecap_field("vs_constants", g_dev.cur.vsc, sizeof g_dev.cur.vsc);
    zonecap_field("ps_constants", g_dev.cur.psc, sizeof g_dev.cur.psc);
    zonecap_field("clip_planes", g_dev.cur.clip, sizeof g_dev.cur.clip);
    Shader* vs = vs_of(g_dev.cur.vs);
    uint32_t pi = g_dev.cur.ps - 1;
    Shader* ps = pi < g_dev.nps ? &g_dev.ps[pi] : NULL;
    zonecap_field("vs_tokens", vs ? vs->func : NULL, vs ? vs->nfunc * 4 : 0);
    zonecap_field("vs_decl", vs ? vs->decl : NULL, vs ? vs->ndecl * 4 : 0);
    zonecap_field("ps_tokens", ps ? ps->func : NULL, ps ? ps->nfunc * 4 : 0);
    fputs(",\"index_resource\":", g_zonecap.json);
    zonecap_object(up ? 0 : g_dev.cur.ib);
    zonecap_field("indices", indices ? GUEST_PTR(indices) : NULL, indices ? (size_t)n * index_size : 0);
    fputs(",\"layout\":[", g_zonecap.json);
    for (int i = 0; i < GFX_NREGS; ++i)
        fprintf(g_zonecap.json, "%s[%u,%u,%u,%d]", i ? "," : "", d->vs.el[i].used,
            d->vs.el[i].stream, d->vs.el[i].type, d->u.offset[i]);
    fputs("],\"streams\":[", g_zonecap.json);
    for (int i = 0; i < GFX_NSTREAMS; ++i)
    {
        int used = 0;
        for (int j = 0; j < GFX_NREGS; ++j)
            used |= d->vs.el[j].used && d->vs.el[j].stream == i;
        if (i)
            fputc(',', g_zonecap.json);
        if (!used)
        {
            fputs("null", g_zonecap.json);
            continue;
        }
        Obj* b = up ? NULL : obj(g_dev.cur.stream[i]);
        uint32_t stride = up ? up_stride : g_dev.cur.stride[i];
        uint64_t start = (uint64_t)first * stride;
        uint64_t size = stride ? (uint64_t)vertices * stride : 64;
        fprintf(g_zonecap.json, "{\"stride\":%u,\"resource\":", stride);
        zonecap_object(b ? b->guest : 0);
        fputs(",\"data\":", g_zonecap.json);
        if ((!up && (!b || !b->mem || start >= b->size)) || size > 268435456)
            g_zonecap.failed = 1, fputs("null", g_zonecap.json);
        else
        {
            if (b && size > b->size - start)
                size = b->size - start;
            zonecap_blob(GUEST_PTR((up ? up : b->mem) + (uint32_t)start), (size_t)size);
        }
        fputc('}', g_zonecap.json);
    }
    fputs("],\"textures\":[", g_zonecap.json);
    for (int i = 0; i < 8; ++i)
    {
        if (i)
            fputc(',', g_zonecap.json);
        Obj* t = obj(g_dev.cur.tex[i]);
        if (!t)
        {
            fputs("null", g_zonecap.json);
            continue;
        }
        fputs("{\"resource\":", g_zonecap.json);
        zonecap_object(t->guest);
        fputs(",\"surfaces\":[", g_zonecap.json);
        for (uint32_t j = 0; j < t->nsubs; ++j)
        {
            if (j)
                fputc(',', g_zonecap.json);
            Obj* surface = obj(t->subs[j]);
            fputs("{\"resource\":", g_zonecap.json);
            zonecap_object(t->subs[j]);
            fputs(",\"data\":", g_zonecap.json);
            zonecap_texture(surface);
            fputc('}', g_zonecap.json);
        }
        fputs("]}", g_zonecap.json);
    }
    fputs("]}\n", g_zonecap.json);
}
