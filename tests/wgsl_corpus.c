/* WGSL for every shader key pair the game has used: reads a back end's pipeline cache (the Metal one,
 * pipelines.v1: records of a PipeKey whose first member is the LibKey {GfxVsKey, GfxFsKey}, then the
 * vs and ps tokens) and writes gfx_wgsl_generate's text for each distinct pair to <out>/NNNN.wgsl,
 * for a WGSL validator (tools/web/wgsl_check.sh runs naga over them).
 *
 *   clang -O1 -I runtime -I runtime/portable tests/wgsl_corpus.c runtime/portable/gfx_msl.c \
 *       runtime/portable/gfx_msl_shaders.c -o build/wgsl_corpus
 *   build/wgsl_corpus ~/Library/Caches/FFXI/pipelines.v1 build/wgsl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gfx.h"
#include "gfx_msl.h"

#define PIPE_MAGIC 0x314B5053u /* "SPK1", gfx_metal.m's record header */

typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: wgsl_corpus <pipelines.v1> <out folder>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    mkdir(argv[2], 0755);
    LibKey* seen = NULL;
    unsigned nseen = 0, written = 0, failed = 0, records = 0;
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2)
    {
        if (hdr[0] != PIPE_MAGIC || hdr[1] < sizeof(LibKey) || hdr[1] > 4096)
            break;
        uint8_t* key = malloc(hdr[1]);
        uint32_t nvs = 0, nps = 0, *vs = NULL, *ps = NULL;
        int ok = fread(key, hdr[1], 1, f) == 1 && fread(&nvs, 4, 1, f) == 1 && nvs <= 65536;
        if (ok && nvs)
            vs = malloc(4u * nvs), ok = fread(vs, 4, nvs, f) == nvs;
        ok = ok && fread(&nps, 4, 1, f) == 1 && nps <= 65536;
        if (ok && nps)
            ps = malloc(4u * nps), ok = fread(ps, 4, nps, f) == nps;
        if (!ok)
            break;
        records++;
        LibKey lk;
        memcpy(&lk, key, sizeof lk);
        int dup = 0;
        for (unsigned i = 0; i < nseen && !dup; ++i)
            dup = !memcmp(&seen[i], &lk, sizeof lk);
        if (!dup)
        {
            seen = realloc(seen, (nseen + 1) * sizeof *seen);
            seen[nseen++] = lk;
            char* src = gfx_wgsl_generate(&lk.vs, &lk.fs, vs, ps);
            if (!src)
                failed++;
            else
            {
                char path[1024];
                snprintf(path, sizeof path, "%s/%04u.wgsl", argv[2], written++);
                FILE* o = fopen(path, "w");
                fputs(src, o);
                fclose(o);
                free(src);
            }
        }
        free(key), free(vs), free(ps);
    }
    fclose(f);
    printf("%u records, %u key pairs: %u written, %u not translated\n", records, nseen, written, failed);
    return 0;
}
