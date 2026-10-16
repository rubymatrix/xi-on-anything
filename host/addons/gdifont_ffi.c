/* ThornyFFXI's gdifonts renderer (gdifonttexture.dll, GDI+ on Windows) for addons' ffi: text and
 * rounded rectangles drawn into IDirect3DTexture8 objects (d3d_ffi.c), which the gdifonts Lua
 * library then draws with an ID3DXSprite. tHotBar, tCrossBar and other addons carry the library;
 * its `ffi.load('...gdifonttexture.dll')` is the ffi.C namespace here (xi.lua), so these are
 * exported from host64 under the DLL's names and with its structures (gdifonts' include.lua).
 *
 * Text: stb_truetype, fonts found as the overlay's text objects find them (xi_gui_font_file);
 * FontHeight is the em size in pixels, as GDI+'s UnitPixel. Bold or italic without a font file
 * for the style is made (a pixel wider, a slant). The outline is a pen OutlineWidth wide around
 * the glyphs' edges (half outside), under the fill; gradients run the fill colour to
 * GradientColor in the eight GDI+ directions.
 *
 * Addons carry three versions of the DLL, one export set here serving all of them:
 * - ThornyFFXI's own (tTimers, tHotBar): GdiFontData_t with the family and text inline.
 * - fancychat's: the same, plus CreateTextureColor, whose text colours runs with inline markers
 *   (\§AARRGGBBç\ until \§--------ç\), and SetFontFolder, the folder of fonts the addon carries
 *   (Consolas, its game icons), found by family name and for glyphs the asked-for font lacks.
 * - onimitch's "regions" fork (balloon): the family and text as pointers, then Regions (character
 *   ranges filled in a colour of their own) and ClipRange (only those characters shown, the
 *   texture sized for all of them: balloon's text typing out). Ranges count UTF-16 units, as GDI+.
 *   CreateTexture tells it from the inline layout by the zero padding before its family pointer,
 *   where the inline layout has the family's first characters. */
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d_ffi.h"
#include "host.h"
#include "plat.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef struct
{
    int32_t BoxHeight, BoxWidth;
    float FontHeight, OutlineWidth;
    uint32_t FontFlags, FontColor, OutlineColor, GradientStyle, GradientColor;
    char FontFamily[256];
    char FontText[4096];
} GdiFontData;

typedef struct
{
    int32_t First, Length;
} GdiCharRange;

typedef struct
{
    uint32_t FontColor, OutlineColor;
    const GdiCharRange* Ranges;
    int32_t RangesLength;
} GdiRegion;

/* the regions fork's GdiFontData_t */
typedef struct
{
    int32_t BoxHeight, BoxWidth;
    float FontHeight, OutlineWidth;
    uint32_t FontFlags, FontColor, OutlineColor, GradientStyle, GradientColor;
    const char* FontFamily;
    const char* FontText;
    const GdiRegion* Regions;
    int32_t RegionsLength;
    const GdiCharRange* ClipRange;
} GdiFontDataP;

typedef struct
{
    int32_t Width, Height, Diameter;
    uint32_t OutlineColor, OutlineWidth, FillColor, GradientStyle, GradientColor;
} GdiRectData;

typedef struct
{
    int32_t Width, Height;
    void* Texture;
} GdiFontReturn;

enum
{
    F_BOLD = 1,
    F_ITALIC = 2,
    F_UNDERLINE = 4,
    F_STRIKEOUT = 8,
};

typedef struct Manager
{
    uint32_t magic;
    char dump[1024];
    unsigned dumped;
} Manager;

#define MANAGER_MAGIC 0x47444946u /* "GDIF" */

/* --- fonts ---------------------------------------------------------------------------------- */

typedef struct Face
{
    char key[300];
    uint8_t* data;
    stbtt_fontinfo info;
    int ok, fake_bold, fake_italic;
    struct Face* next;
} Face;

static Face* g_faces;

/* SetFontFolder's fonts, a face for each file in every addon's folder that asked; g_folders_seen
 * (a folder's fonts came) changes the cache's keys, so a family looked up before is looked up again */
static Face* g_folder_faces[64];
static int g_nfolder_faces;
static unsigned g_folders_seen;

static int load_face(Face* f, const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n)
    {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off))
    {
        xi_log_once(path, "gdifonts: can't read %s", path);
        return 0;
    }
    return f->ok = 1;
}

/* a folder font of this family, in the style asked for if there is one (else made) */
static Face* folder_face(const char* key, const char* family, int bold, int italic)
{
    int want = (bold ? STBTT_MACSTYLE_BOLD : 0) | (italic ? STBTT_MACSTYLE_ITALIC : 0);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < g_nfolder_faces; ++i)
        {
            Face* ff = g_folder_faces[i];
            int flags = pass ? STBTT_MACSTYLE_NONE : want ? want : STBTT_MACSTYLE_NONE;
            if (stbtt_FindMatchingFont(ff->data, family, flags) < 0)
                continue;
            Face* f = (Face*)calloc(1, sizeof *f);
            if (!f)
                return NULL;
            snprintf(f->key, sizeof f->key, "%s", key);
            f->data = ff->data, f->info = ff->info, f->ok = 1;
            f->fake_bold = pass && bold, f->fake_italic = pass && italic;
            f->next = g_faces, g_faces = f;
            return f;
        }
    return NULL;
}

/* a folder font with a glyph for c, for one the font asked for lacks (fancychat's game icons) */
static Face* fallback_face(int c)
{
    for (int i = 0; i < g_nfolder_faces; ++i)
        if (stbtt_FindGlyphIndex(&g_folder_faces[i]->info, c))
            return g_folder_faces[i];
    return NULL;
}

static int contains_ci(const char* s, const char* w)
{
    size_t n = strlen(w);
    for (; *s; ++s)
    {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)w[i]))
            ++i;
        if (i == n)
            return 1;
    }
    return 0;
}

static Face* face_for(const char* family, int bold, int italic)
{
    char key[300];
    snprintf(key, sizeof key, "%s|%d%d|%u", family, bold, italic, g_folders_seen);
    for (Face* f = g_faces; f; f = f->next)
        if (!strcmp(f->key, key))
            return f->ok ? f : NULL;
    Face* f = folder_face(key, family, bold, italic);
    if (f)
        return f;
    f = (Face*)calloc(1, sizeof *f);
    if (!f)
        return NULL;
    snprintf(f->key, sizeof f->key, "%s", key);
    f->next = g_faces, g_faces = f;
    char path[1200];
    if (!xi_gui_font_file(family, bold, italic, path, sizeof path) && !xi_gui_font_file("Arial", bold, italic, path, sizeof path) &&
        !xi_gui_font_file("Helvetica", 0, 0, path, sizeof path))
    {
        xi_log_once(key, "gdifonts: no font for %s", family);
        return NULL;
    }
    if (!load_face(f, path))
        return NULL;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    f->fake_bold = bold && !contains_ci(base, "bold");
    f->fake_italic = italic && !contains_ci(base, "italic") && !contains_ci(base, "oblique");
    return f;
}

static uint32_t utf8_next(const unsigned char** p)
{
    const unsigned char* s = *p;
    uint32_t c = *s++;
    int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (k)
        c &= 0x3Fu >> k;
    for (; k && (*s & 0xC0) == 0x80; --k)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

/* --- pixels --------------------------------------------------------------------------------- */

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static float gradient_t(uint32_t style, float x, float y, float w, float h)
{
    float u = w > 1 ? x / (w - 1) : 0, v = h > 1 ? y / (h - 1) : 0;
    switch (style)
    {
    case 1: return u;
    case 2: return (u + v) * 0.5f;
    case 3: return v;
    case 4: return ((1 - u) + v) * 0.5f;
    case 5: return 1 - u;
    case 6: return 1 - (u + v) * 0.5f;
    case 7: return 1 - v;
    case 8: return (u + 1 - v) * 0.5f;
    default: return 0;
    }
}

/* ARGB colour at (x, y) of a fill with a gradient */
static void fill_color(uint32_t c0, uint32_t c1, uint32_t style, float x, float y, float w, float h, float out[4])
{
    float t = style ? clampf(gradient_t(style, x, y, w, h), 0, 1) : 0;
    for (int k = 0; k < 4; ++k)
    {
        float a = (float)((c0 >> (8 * k)) & 0xFF), b = (float)((c1 >> (8 * k)) & 0xFF);
        out[k] = a + (b - a) * t; /* b g r a */
    }
}

/* fill (coverage cf, colour f) over outline (coverage co, colour o) into BGRA */
static void compose(uint8_t* d, float cf, const float f[4], float co, const float o[4])
{
    float af = cf * f[3] / 255.0f, ao = co * o[3] / 255.0f;
    float a = af + ao * (1 - af);
    if (a <= 0)
    {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    for (int k = 0; k < 3; ++k)
        d[k] = (uint8_t)clampf((f[k] * af + o[k] * ao * (1 - af)) / a + 0.5f, 0, 255);
    d[3] = (uint8_t)clampf(a * 255.0f + 0.5f, 0, 255);
}

/* Grows a coverage mask by radius r (pixels), soft at the edge: what a pen r wide outside draws. */
static float* dilate(const float* m, int w, int h, float r)
{
    float* o = (float*)calloc((size_t)w * h, sizeof *o);
    if (!o)
        return NULL;
    int R = (int)ceilf(r);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float best = 0;
            for (int dy = -R; dy <= R && best < 1; ++dy)
            {
                int yy = y + dy;
                if (yy < 0 || yy >= h)
                    continue;
                for (int dx = -R; dx <= R; ++dx)
                {
                    int xx = x + dx;
                    if (xx < 0 || xx >= w)
                        continue;
                    float v = m[(size_t)yy * w + xx];
                    if (v <= best)
                        continue;
                    float edge = clampf(r + 0.5f - sqrtf((float)(dx * dx + dy * dy)), 0, 1);
                    if (v * edge > best)
                        best = v * edge;
                }
            }
            o[(size_t)y * w + x] = best;
        }
    return o;
}

static void dump(Manager* m, const char* what, const uint8_t* bgra, int w, int h)
{
    if (!m || !m->dump[0])
        return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s_%04u.bmp", m->dump, what, m->dumped++);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t size = 54 + (uint32_t)w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t v = 54;
    memcpy(hdr + 10, &v, 4);
    v = 40;
    memcpy(hdr + 14, &v, 4);
    int32_t iw = w, ih = -h;
    memcpy(hdr + 18, &iw, 4);
    memcpy(hdr + 22, &ih, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(bgra, 4, (size_t)w * h, f);
    fclose(f);
}

/* --- the exports ---------------------------------------------------------------------------- */

XI_FFI uint32_t* CreateFontManager(void* device)
{
    (void)device;
    Manager* m = (Manager*)calloc(1, sizeof *m);
    if (m)
        m->magic = MANAGER_MAGIC;
    return (uint32_t*)m;
}

static Manager* manager(uint32_t* p) { return p && ((Manager*)p)->magic == MANAGER_MAGIC ? (Manager*)p : NULL; }

XI_FFI void DestroyFontManager(uint32_t* p)
{
    Manager* m = manager(p);
    if (m)
    {
        m->magic = 0;
        free(m);
    }
}

XI_FFI void EnableTextureDump(uint32_t* p, const char* folder)
{
    Manager* m = manager(p);
    if (m && folder)
        xi_host_path(folder, m->dump, sizeof m->dump);
}

XI_FFI void DisableTextureDump(uint32_t* p)
{
    Manager* m = manager(p);
    if (m)
        m->dump[0] = 0;
}

XI_FFI int GetFontAvailable(const char* family)
{
    char path[1200];
    if (!family)
        return 0;
    /* a style named matches the family alone (with none, stb wants "Family Subfamily") */
    static const int STYLES[] = { STBTT_MACSTYLE_NONE, STBTT_MACSTYLE_BOLD, STBTT_MACSTYLE_ITALIC,
                                  STBTT_MACSTYLE_BOLD | STBTT_MACSTYLE_ITALIC };
    for (int i = 0; i < g_nfolder_faces; ++i)
        for (int s = 0; s < 4; ++s)
            if (stbtt_FindMatchingFont(g_folder_faces[i]->data, family, STYLES[s]) >= 0)
                return 1;
    return xi_gui_font_file(family, 0, 0, path, sizeof path);
}

/* SetFontFolder (fancychat's): the fonts in an addon's folder, by family name and for glyphs */
XI_FFI void SetFontFolder(const char* folder)
{
    char dir[1200], prefix[1300];
    if (!folder)
        return;
    xi_host_path(folder, dir, sizeof dir);
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
        dir[--n] = 0;
    snprintf(prefix, sizeof prefix, "file|%s/", dir);
    size_t np = strlen(prefix);
    for (int i = 0; i < g_nfolder_faces; ++i)
        if (!strncmp(g_folder_faces[i]->key, prefix, np) && !strchr(g_folder_faces[i]->key + np, '/'))
            return; /* this folder's fonts are in */
    PlatDir* d = plat_dir_open(dir);
    const char* e;
    int added = 0;
    while (d && (e = plat_dir_next(d)) && g_nfolder_faces < (int)(sizeof g_folder_faces / sizeof *g_folder_faces))
    {
        size_t len = strlen(e);
        if (len < 5 || (strcasecmp(e + len - 4, ".ttf") && strcasecmp(e + len - 4, ".otf")))
            continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e);
        Face* f = (Face*)calloc(1, sizeof *f);
        if (!f)
            break;
        snprintf(f->key, sizeof f->key, "file|%s", path);
        if (!load_face(f, path))
        {
            free(f->data);
            free(f);
            continue;
        }
        g_folder_faces[g_nfolder_faces++] = f, ++added;
    }
    if (d)
        plat_dir_close(d);
    if (added)
        ++g_folders_seen;
}

/* --- text ----------------------------------------------------------------------------------- */

/* the fields every layout of GdiFontData_t starts with */
typedef struct
{
    int32_t BoxHeight, BoxWidth;
    float FontHeight, OutlineWidth;
    uint32_t FontFlags, FontColor, OutlineColor, GradientStyle, GradientColor;
} GdiFontHead;

/* a character to draw: where it is in UTF-16 units (GDI+'s character ranges count those), its
 * font, a colour of its own (a run's or a region's) and whether it shows */
typedef struct Glyph
{
    int c, at;
    uint32_t color;
    uint8_t own, shown;
    Face* face;
} Glyph;

typedef struct Text
{
    Glyph* g;
    int n, cap, u16;
} Text;

static int text_add(Text* t, int c, uint32_t color, int own)
{
    if (t->n == t->cap)
    {
        int cap = t->cap ? t->cap * 2 : 256;
        Glyph* g = (Glyph*)realloc(t->g, (size_t)cap * sizeof *g);
        if (!g)
            return 0;
        t->g = g, t->cap = cap;
    }
    Glyph* g = &t->g[t->n++];
    g->c = c, g->at = t->u16, g->color = color, g->own = (uint8_t)own, g->shown = 1, g->face = NULL;
    t->u16 += c >= 0x10000 ? 2 : 1;
    return 1;
}

/* the text's characters; with marks, fancychat's markers colour runs: \§AARRGGBBç\ (backslash,
 * C2 A7, eight characters, C3 A7, backslash) starts one, \§--------ç\ goes back to the font's */
static int text_utf8(Text* t, const char* s, int marks)
{
    const unsigned char* p = (const unsigned char*)s;
    uint32_t color = 0;
    int own = 0;
    while (*p)
    {
        if (marks && p[0] == '\\' && p[1] == 0xC2 && p[2] == 0xA7 && memchr(p + 3, 0, 8) == NULL && p[11] == 0xC3 &&
            p[12] == 0xA7 && p[13] == '\\')
        {
            char hex[9], *end;
            memcpy(hex, p + 3, 8);
            hex[8] = 0;
            unsigned long v = strtoul(hex, &end, 16);
            own = end == hex + 8;
            color = own ? (uint32_t)v : 0;
            p += 14;
            continue;
        }
        if (!text_add(t, (int)utf8_next(&p), color, own))
            return 0;
    }
    return 1;
}

static float glyph_scale(const Glyph* g, float height) { return stbtt_ScaleForMappingEmToPixels(&g->face->info, height); }

static float glyph_advance(const Glyph* g, float height)
{
    if (g->c == '\r')
        return 0;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g->face->info, g->c, &adv, &lsb);
    return (float)adv * glyph_scale(g, height);
}

/* kerning against the character before, in the same font */
static float glyph_kern(const Glyph* g, const Glyph* prev, float height)
{
    if (!prev || prev->face != g->face || g->c == '\r')
        return 0;
    return (float)stbtt_GetCodepointKernAdvance(&g->face->info, prev->c, g->c) * glyph_scale(g, height);
}

#define MAX_LINES 128

static GdiFontReturn render_text(Manager* mgr, const GdiFontHead* d, const char* family, Text* t)
{
    GdiFontReturn r = { 0, 0, NULL };
    if (!t->n || d->FontHeight <= 0 || d->FontHeight > 512)
        return r;
    int bold = (d->FontFlags & F_BOLD) != 0, italic = (d->FontFlags & F_ITALIC) != 0;
    Face* f = face_for(family && family[0] ? family : "Arial", bold, italic);
    if (!f)
        return r;
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, d->FontHeight);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    float line_h = (float)(asc - desc + gap) * scale, ascent = (float)asc * scale;
    int coloured = 0;
    for (int i = 0; i < t->n; ++i)
    {
        Glyph* g = &t->g[i];
        g->face = f;
        if (g->c >= ' ' && !stbtt_FindGlyphIndex(&f->info, g->c))
        {
            Face* o = fallback_face(g->c);
            if (o)
                g->face = o;
        }
        coloured |= g->own;
    }

    float ow = d->OutlineWidth > 0 ? d->OutlineWidth : 0;
    float slant = f->fake_italic ? 0.2f : 0;
    int pad = (int)ceilf(ow * 0.5f) + 1;

    /* lines (their first and end characters) and their widths, every character's, shown or not; in
     * a box BoxWidth wide, as GDI+ lays text out in one: a line breaks after the last space that
     * fits, or inside a word wider than the box */
    float room = d->BoxWidth > 0 ? (float)(d->BoxWidth - 2 * pad) - slant * line_h : 0;
    int first[MAX_LINES], last[MAX_LINES];
    float widths[MAX_LINES];
    int nl = 0;
    for (int i = 0; nl < MAX_LINES;)
    {
        int start = i, brk = -1;
        float w = 0, wbrk = 0;
        const Glyph* prev = NULL;
        for (; i < t->n && t->g[i].c != '\n'; ++i)
        {
            const Glyph* g = &t->g[i];
            float step = glyph_kern(g, prev, d->FontHeight) + glyph_advance(g, d->FontHeight);
            if (room > 0 && i > start && g->c != ' ' && w + step > room)
            {
                if (brk > start)
                    i = brk, w = wbrk;
                break;
            }
            w += step;
            if (g->c != '\r')
                prev = g;
            if (g->c == ' ' && (i + 1 >= t->n || t->g[i + 1].c != ' '))
                brk = i + 1, wbrk = w;
        }
        first[nl] = start, last[nl] = i, widths[nl++] = w;
        if (i >= t->n)
            break;
        if (t->g[i].c == '\n')
            ++i;
    }
    float maxw = 0;
    for (int i = 0; i < nl; ++i)
        if (widths[i] > maxw)
            maxw = widths[i];
    int W = d->BoxWidth > 0 ? d->BoxWidth : (int)ceilf(maxw + slant * line_h + (f->fake_bold ? 1 : 0)) + 2 * pad;
    int H = d->BoxHeight > 0 ? d->BoxHeight : (int)ceilf(line_h * (float)nl) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192)
        return r;

    /* coverage: mask for the font's colour, cmask (with ccol) for characters with their own */
    float* mask = (float*)calloc((size_t)W * H, sizeof *mask);
    float* cmask = coloured ? (float*)calloc((size_t)W * H, sizeof *cmask) : NULL;
    uint32_t* ccol = coloured ? (uint32_t*)calloc((size_t)W * H, sizeof *ccol) : NULL;
    if (!mask || (coloured && (!cmask || !ccol)))
    {
        free(mask);
        free(cmask);
        free(ccol);
        return r;
    }
    float th = d->FontHeight / 14.0f < 1 ? 1 : d->FontHeight / 14.0f;
    for (int li = 0; li < nl; ++li)
    {
        float x = (float)pad, base = (float)pad + (float)li * line_h + ascent;
        const Glyph* prev = NULL;
        for (int gi = first[li]; gi < last[li]; ++gi)
        {
            const Glyph* gl = &t->g[gi];
            if (gl->c == '\r')
                continue;
            x += glyph_kern(gl, prev, d->FontHeight);
            prev = gl;
            float adv = glyph_advance(gl, d->FontHeight);
            if (!gl->shown)
            {
                x += adv;
                continue;
            }
            float* target = gl->own ? cmask : mask;
            float gs = glyph_scale(gl, d->FontHeight);
            int gx0, gy0, gx1, gy1;
            float sx = x - floorf(x);
            stbtt_GetCodepointBitmapBoxSubpixel(&gl->face->info, gl->c, gs, gs, sx, 0, &gx0, &gy0, &gx1, &gy1);
            int gw = gx1 - gx0, gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw < 1024 && gh < 1024)
            {
                unsigned char* g = (unsigned char*)malloc((size_t)gw * gh);
                if (g)
                {
                    stbtt_MakeCodepointBitmapSubpixel(&gl->face->info, g, gw, gh, gw, gs, gs, sx, 0, gl->c);
                    int ox = (int)floorf(x) + gx0, oy = (int)floorf(base) + gy0;
                    for (int y = 0; y < gh; ++y)
                    {
                        int yy = oy + y;
                        if (yy < 0 || yy >= H)
                            continue;
                        /* a slant made for italic: rows above the baseline move right */
                        float shift = slant * (base - (float)yy);
                        int ish = (int)floorf(shift);
                        float fr = shift - (float)ish;
                        for (int xg = 0; xg < gw; ++xg)
                        {
                            float v = g[(size_t)y * gw + xg] / 255.0f;
                            if (v <= 0)
                                continue;
                            for (int k = 0; k < 2; ++k)
                            {
                                int xx = ox + xg + ish + k;
                                float part = v * (k ? fr : 1 - fr);
                                if (xx >= 0 && xx < W && part > 0)
                                {
                                    size_t i = (size_t)yy * W + xx;
                                    target[i] = clampf(target[i] + part, 0, 1);
                                    if (gl->own)
                                        ccol[i] = gl->color;
                                }
                            }
                        }
                    }
                    free(g);
                }
            }
            /* underline and strikeout, under the characters shown */
            for (int which = 0; which < 2; ++which)
            {
                if (!(d->FontFlags & (which ? F_STRIKEOUT : F_UNDERLINE)))
                    continue;
                float y0 = which ? base - d->FontHeight * 0.3f : base + d->FontHeight * 0.1f;
                for (int yy = (int)floorf(y0); yy < (int)ceilf(y0 + th) && yy < H; ++yy)
                    for (int xx = (int)floorf(x); xx < (int)ceilf(x + adv) && xx < W; ++xx)
                        if (yy >= 0 && xx >= 0)
                        {
                            size_t i = (size_t)yy * W + xx;
                            target[i] = 1;
                            if (gl->own)
                                ccol[i] = gl->color;
                        }
            }
            x += adv;
        }
    }
    if (f->fake_bold) /* a pixel wider */
        for (int y = 0; y < H; ++y)
            for (int x = W - 1; x > 0; --x)
            {
                size_t i = (size_t)y * W + x;
                if (mask[i - 1] > mask[i])
                    mask[i] = mask[i - 1];
                if (cmask && cmask[i - 1] > cmask[i])
                    cmask[i] = cmask[i - 1], ccol[i] = ccol[i - 1];
            }
    /* the outline goes round every character shown, whatever its colour */
    float* all = mask;
    if (cmask)
    {
        all = (float*)malloc((size_t)W * H * sizeof *all);
        for (size_t i = 0; all && i < (size_t)W * H; ++i)
            all[i] = cmask[i] > mask[i] ? cmask[i] : mask[i];
    }
    float* outline = all && ow > 0 && (d->OutlineColor >> 24) ? dilate(all, W, H, ow * 0.5f) : NULL;
    if (all != mask)
        free(all);
    uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
    if (!px)
    {
        free(mask);
        free(cmask);
        free(ccol);
        free(outline);
        return r;
    }
    float oc[4];
    fill_color(d->OutlineColor, d->OutlineColor, 0, 0, 0, 1, 1, oc);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float fc[4];
            size_t i = (size_t)y * W + x;
            if (cmask && cmask[i] > mask[i])
            {
                fill_color(ccol[i], ccol[i], 0, 0, 0, 1, 1, fc);
                compose(px + i * 4, cmask[i], fc, outline ? outline[i] : 0, oc);
                continue;
            }
            fill_color(d->FontColor, d->GradientColor, d->GradientStyle, (float)x, (float)y, (float)W, (float)H, fc);
            compose(px + i * 4, mask[i], fc, outline ? outline[i] : 0, oc);
        }
    free(mask);
    free(cmask);
    free(ccol);
    free(outline);
    dump(mgr, "font", px, W, H);
    r.Texture = xi_d3d_texture_bgra(px, W, H);
    free(px);
    if (r.Texture)
        r.Width = W, r.Height = H;
    return r;
}

/* the regions fork's layout: zero padding after GradientColor, then a family or text pointer (balloon
 * passes no family when it found no default font), where the inline layout has the family's first
 * characters - all zero for an empty family */
static int pointer_layout(const void* data)
{
    const uint8_t* b = (const uint8_t*)data;
    size_t a = offsetof(GdiFontData, FontFamily), p = offsetof(GdiFontDataP, FontFamily);
    if (p <= a)
        return 0;
    for (size_t i = a; i < p; ++i)
        if (b[i])
            return 0;
    const char *family, *text;
    memcpy(&family, b + p, sizeof family);
    memcpy(&text, b + offsetof(GdiFontDataP, FontText), sizeof text);
    return family != NULL || text != NULL;
}

/* a range of UTF-16 units: an addon's, so its end is summed without overflow */
static int in_range(int at, const GdiCharRange* r) { return at >= r->First && (int64_t)at < (int64_t)r->First + r->Length; }

static GdiFontReturn create_texture(uint32_t* mgr, const void* data, int marks)
{
    GdiFontReturn r = { 0, 0, NULL };
    Text t = { NULL, 0, 0, 0 };
    if (!data)
        return r;
    if (pointer_layout(data))
    {
        const GdiFontDataP* d = (const GdiFontDataP*)data;
        if (d->FontText && text_utf8(&t, d->FontText, marks))
        {
            for (int k = 0; d->Regions && k < d->RegionsLength; ++k)
            {
                const GdiRegion* rg = &d->Regions[k];
                for (int j = 0; rg->Ranges && j < rg->RangesLength; ++j)
                    for (int i = 0; i < t.n; ++i)
                        if (in_range(t.g[i].at, &rg->Ranges[j]))
                            t.g[i].color = rg->FontColor, t.g[i].own = 1;
            }
            /* GDI+'s clip: a range of no characters clips nothing */
            const GdiCharRange* clip = d->ClipRange;
            if (clip && clip->Length > 0)
                for (int i = 0; i < t.n; ++i)
                    t.g[i].shown = in_range(t.g[i].at, clip);
            r = render_text(manager(mgr), (const GdiFontHead*)data, d->FontFamily ? d->FontFamily : "", &t);
        }
    }
    else
    {
        const GdiFontData* d = (const GdiFontData*)data;
        char family[256], text[4097];
        memcpy(family, d->FontFamily, sizeof family);
        family[255] = 0;
        memcpy(text, d->FontText, 4096);
        text[4096] = 0;
        if (text_utf8(&t, text, marks))
            r = render_text(manager(mgr), (const GdiFontHead*)data, family, &t);
    }
    free(t.g);
    return r;
}

XI_FFI GdiFontReturn CreateTexture(uint32_t* mgr, const void* d) { return create_texture(mgr, d, 0); }

/* fancychat's: its text's markers colour runs of it */
XI_FFI GdiFontReturn CreateTextureColor(uint32_t* mgr, const void* d) { return create_texture(mgr, d, 1); }

/* signed distance from p to a rounded rectangle [x0,x1]x[y0,y1] with corner radius rad */
static float rr_dist(float px, float py, float x0, float y0, float x1, float y1, float rad)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hx = (x1 - x0) * 0.5f - rad, hy = (y1 - y0) * 0.5f - rad;
    float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad;
}

XI_FFI GdiFontReturn CreateRectTexture(uint32_t* mgr, const GdiRectData* d)
{
    GdiFontReturn r = { 0, 0, NULL };
    if (!d || d->Width <= 0 || d->Height <= 0 || d->Width > 8192 || d->Height > 8192)
        return r;
    int W = d->Width, H = d->Height;
    float ow = (float)d->OutlineWidth;
    float rad = d->Diameter > 0 ? (float)d->Diameter * 0.5f : 0;
    float lim = (float)(W < H ? W : H) * 0.5f;
    if (rad > lim)
        rad = lim;
    uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
    if (!px)
        return r;
    float oc[4];
    fill_color(d->OutlineColor, d->OutlineColor, 0, 0, 0, 1, 1, oc);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            float dout = rr_dist(cx, cy, 0, 0, (float)W, (float)H, rad);
            float rin = rad - ow > 0 ? rad - ow : 0;
            float din = ow > 0 ? rr_dist(cx, cy, ow, ow, (float)W - ow, (float)H - ow, rin) : dout;
            float cov_out = clampf(0.5f - dout, 0, 1), cov_in = clampf(0.5f - din, 0, 1);
            float fc[4];
            fill_color(d->FillColor, d->GradientColor, d->GradientStyle, (float)x, (float)y, (float)W, (float)H, fc);
            compose(px + ((size_t)y * W + x) * 4, cov_in, fc, ow > 0 ? cov_out - cov_in * cov_out : 0, oc);
        }
    dump(manager(mgr), "rect", px, W, H);
    r.Texture = xi_d3d_texture_bgra(px, W, H);
    free(px);
    if (r.Texture)
        r.Width = W, r.Height = H;
    return r;
}
