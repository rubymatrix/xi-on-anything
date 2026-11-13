/* Status icons drawn from vectors. See vicons.h.
 *
 * Many of the game's ~600 status icons are a few designs repeated: a sword or an orb with a red
 * arrow and "UP" (about 160 effects), the same in blue with "DN", a dark badge with a red or blue
 * crescent and a word (HP, STR, TP, EXP ...), a crosshair, speed lines, an SP orb, a song's note.
 * Each design is a template here, drawn as an SVG document on a 64-unit square and rasterised with
 * nanosvg at the size the texture is wanted. The art is ours; nothing of the game's is copied.
 *
 * Which icon is which: the game uploads each icon as a 32x32 texture of its own (DXT3), decoded
 * from the status icon DAT (file 87: 0x1800-byte records, the icon at +0x280 as u32 size, u8 type,
 * 16-byte name, a 32-bit DIB, alpha 0 or 0x80; host/addons/res.c reads the rest). An upload is
 * decoded and compared with every icon of that DAT; the nearest is the record it came from (DXT3's
 * error is ~6-8 a channel on average; the next icon is 18 or more away). A template is named by a
 * representative status id and serves every record whose pixels are that icon's exactly, so the
 * effects sharing a design share its template, whatever their ids. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vicons.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../third_party/nanosvg/nanosvg.h"
#include "../third_party/nanosvg/nanosvgrast.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* ---- the templates ----------------------------------------------------------------------------- */

enum
{
    T_UP_SWORD,  /* a sword, a red arrow up, "UP": attack and offence */
    T_UP_ORB,    /* a stone orb, the arrow, "UP": defence, bar- spells, negations */
    T_DN_SWORD,  /* a sword, a blue arrow down, "DN" */
    T_DN_ORB,    /* a stone orb, the arrow down, "DN" */
    T_BADGE_UP,  /* a dark badge with red crescents and a word in tan */
    T_BADGE_DN,  /* the same with blue crescents and the word in mint */
    T_AIM_UP,    /* the red badge with a green crosshair: accuracy */
    T_AIM_DN,
    T_EVA_UP,    /* the red badge with cyan speed lines: evasion */
    T_EVA_DN,
    T_SP,        /* a pale orb with "SP" in violet: the two-hour abilities */
    T_NAVY,      /* a navy orb with a word (SP, CH, MB) in its colour */
    T_MP_ORB,    /* a lavender orb with "MP" in yellow */
    T_SONG_UP,   /* a pink orb, a note, red crescents */
    T_SONG_DN,   /* a lavender orb, a note, blue crescents */
    T_SONG,      /* a lavender orb and a note */
    T_HASTE,     /* a pink orb with a red arrow */
    T_LEVEL,     /* a gold medallion with "LV": level restrictions */
    T_DIE,       /* an ivory die: the corsair's rolls (word "red": red pips; "plus": a cyan plus beside it) */
    T_IMAGERY,   /* a dark glass orb with three gold stars: crafting imagery */
    T_DAZE_SPEED,  /* the blue badge, speed lines and a digit: lethargic daze */
    T_DAZE_SWORD,  /* the blue badge, a sword and a digit: sluggish and bewildered daze */
    T_DAZE_GEM,    /* the blue badge, a violet gem and a digit: weakened daze */
};

typedef struct Template
{
    uint16_t rep;      /* a status id whose icon is this design */
    uint8_t kind;
    const char* word;  /* T_BADGE_*, T_NAVY */
    uint32_t color;    /* T_NAVY: the word's */
} Template;

static const Template g_designs[] = {
    { 45, T_UP_SWORD, NULL, 0 },  /* boost: also berserk, attack boost, en- spells, ... */
    { 57, T_UP_ORB, NULL, 0 },    /* defender: also defense boost, bar- spells, negate ... */
    { 22, T_DN_SWORD, NULL, 0 },  /* intimidate: also attack down, provoke */
    { 21, T_DN_ORB, NULL, 0 },    /* addle: also defense down, magic def. down */
    { 42, T_BADGE_UP, "HP", 0 },  /* regen, max HP boost, auto regen */
    { 43, T_BADGE_UP, "MP", 0 },  /* refresh, max MP boost, auto refresh */
    { 80, T_BADGE_UP, "STR", 0 },
    { 81, T_BADGE_UP, "DEX", 0 },
    { 82, T_BADGE_UP, "VIT", 0 },
    { 83, T_BADGE_UP, "AGI", 0 },
    { 84, T_BADGE_UP, "INT", 0 },
    { 85, T_BADGE_UP, "MND", 0 },
    { 86, T_BADGE_UP, "CHR", 0 },
    { 170, T_BADGE_UP, "TP", 0 }, /* regain, store TP */
    { 249, T_BADGE_UP, "EXP", 0 },/* dedication, commitment */
    { 23, T_BADGE_DN, "HP", 0 },  /* kaustra, burn, frost, ..., helix */
    { 145, T_BADGE_DN, "HP", 0 }, /* max MP down: the game's own icon says HP too */
    { 136, T_BADGE_DN, "STR", 0 },
    { 137, T_BADGE_DN, "DEX", 0 },
    { 138, T_BADGE_DN, "VIT", 0 },
    { 139, T_BADGE_DN, "AGI", 0 },
    { 140, T_BADGE_DN, "INT", 0 },
    { 141, T_BADGE_DN, "MND", 0 },
    { 142, T_BADGE_DN, "CHR", 0 },
    { 168, T_BADGE_DN, "TP", 0 }, /* inhibit TP, max TP down */
    { 58, T_AIM_UP, NULL, 0 },    /* aggressor, focus, sharpshot, accuracy boost */
    { 146, T_AIM_DN, NULL, 0 },   /* accuracy down, magic acc. down */
    { 60, T_EVA_UP, NULL, 0 },    /* dodge, evasion boost, foil */
    { 148, T_EVA_DN, NULL, 0 },   /* evasion down, lethargic daze, magic evasion down */
    { 26, T_SP, NULL, 0 },        /* mighty strikes, hundred fists, ..., trance */
    { 163, T_NAVY, "SP", 0xF2A0B4 }, /* azure lore */
    { 164, T_NAVY, "CH", 0xF4B050 }, /* chain affinity */
    { 165, T_NAVY, "MB", 0xA8E8B4 }, /* burst affinity */
    { 24, T_MP_ORB, NULL, 0 },    /* manafont */
    { 195, T_SONG_UP, NULL, 0 },  /* paeon, ballad, minne, minuet, madrigal, ... */
    { 192, T_SONG_DN, NULL, 0 },  /* requiem, lullaby, elegy, threnody, nocturne */
    { 25, T_SONG, NULL, 0 },      /* the songs' own */
    { 33, T_HASTE, NULL, 0 },     /* haste, flurry, concentration, fast cast */
    { 143, T_LEVEL, NULL, 0 },    /* level restriction, SJ restriction */
    { 310, T_DIE, NULL, 0 },      /* every job's roll, rune fencer's included */
    { 357, T_DIE, "red", 0 },     /* snake eye */
    { 308, T_DIE, "plus", 0 },    /* double-up chance */
    { 235, T_IMAGERY, NULL, 0 },  /* fishing ... cooking imagery, imagery 1-5 */
    { 387, T_DAZE_SPEED, "2", 0 },/* lethargic daze 2-5 (1 is evasion down's icon) */
    { 388, T_DAZE_SPEED, "3", 0 },
    { 389, T_DAZE_SPEED, "4", 0 },
    { 390, T_DAZE_SPEED, "5", 0 },
    { 392, T_DAZE_SWORD, "2", 0 },/* sluggish daze 2-5, bewildered daze 2-5 */
    { 393, T_DAZE_SWORD, "3", 0 },
    { 394, T_DAZE_SWORD, "4", 0 },
    { 395, T_DAZE_SWORD, "5", 0 },
    { 397, T_DAZE_GEM, "2", 0 },  /* weakened daze 2-5 */
    { 398, T_DAZE_GEM, "3", 0 },
    { 399, T_DAZE_GEM, "4", 0 },
    { 400, T_DAZE_GEM, "5", 0 },
};
#define NDESIGNS (sizeof g_designs / sizeof *g_designs)

/* The one-off designs: an SVG document's contents on the same 64-unit square, one per icon (or per
 * icon its effects share), by a representative status id. Kept by theme in vicons_art_*.inc. */
typedef struct Art
{
    uint16_t rep;
    const char* body;
} Art;

#define ART(id, body) { id, body },
static const Art g_art[] = {
#include "vicons_art_a.inc"
#include "vicons_art_b.inc"
#include "vicons_art_c.inc"
#include "vicons_art_d.inc"
#include "vicons_art_e.inc"
#include "vicons_art_f.inc"
    { 0xFFFF, NULL },
};
#undef ART
#define NART (sizeof g_art / sizeof *g_art - 1)
#define NALL (NDESIGNS + NART)

static uint16_t rep_of(size_t t) { return t < NDESIGNS ? g_designs[t].rep : g_art[t - NDESIGNS].rep; }

/* ---- building an SVG document ------------------------------------------------------------------ */

typedef struct Sb
{
    char* p;
    size_t n, len;
    int full;
} Sb;

static void sb(Sb* s, const char* fmt, ...)
{
    if (s->full)
        return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(s->p + s->len, s->n - s->len, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= s->n - s->len)
        s->full = 1;
    else
        s->len += (size_t)k;
}

/* A radial gradient over its shape's box (nanosvg takes a box's fractions as percentages): inner
 * colour at (cx, cy), mid at half way, outer at the edge. */
static void radial(Sb* s, const char* id, float cx, float cy, uint32_t c0, uint32_t c1, uint32_t c2)
{
    sb(s, "<radialGradient id='%s' cx='%.0f%%' cy='%.0f%%' r='75%%' fx='%.0f%%' fy='%.0f%%'>"
          "<stop offset='0' stop-color='#%06X'/><stop offset='0.5' stop-color='#%06X'/>"
          "<stop offset='1' stop-color='#%06X'/></radialGradient>",
        id, cx * 100, cy * 100, cx * 100, cy * 100, c0, c1, c2);
}

static void linear(Sb* s, const char* id, float x1, float y1, float x2, float y2, uint32_t c0, uint32_t c1, uint32_t c2)
{
    sb(s, "<linearGradient id='%s' x1='%.0f%%' y1='%.0f%%' x2='%.0f%%' y2='%.0f%%'>"
          "<stop offset='0' stop-color='#%06X'/><stop offset='0.5' stop-color='#%06X'/>"
          "<stop offset='1' stop-color='#%06X'/></linearGradient>",
        id, x1 * 100, y1 * 100, x2 * 100, y2 * 100, c0, c1, c2);
}

/* A ring's sector, radii r0..r1 about (32, 32), from angle a0 to a1 in degrees (0 along +x, turning
 * clockwise on screen). */
static void sector(Sb* s, float r0, float r1, float a0, float a1, const char* fill)
{
    const float d = 3.14159265f / 180.0f;
    float c0 = cosf(a0 * d), s0 = sinf(a0 * d), c1 = cosf(a1 * d), s1 = sinf(a1 * d);
    int large = a1 - a0 > 180.0f;
    sb(s, "<path fill='%s' d='M%.2f %.2f A%.2f %.2f 0 %d 1 %.2f %.2f L%.2f %.2f A%.2f %.2f 0 %d 0 %.2f %.2f Z'/>", fill,
        32 + r1 * c0, 32 + r1 * s0, r1, r1, large, 32 + r1 * c1, 32 + r1 * s1, 32 + r0 * c1, 32 + r0 * s1, r0, r0, large,
        32 + r0 * c0, 32 + r0 * s0);
}

/* An arrow from (x0, y0) to its point at (x1, y1): a shaft w wide and a head hl long, hw wide. */
static void arrow(Sb* s, float x0, float y0, float x1, float y1, float w, float hl, float hw, uint32_t fill, uint32_t edge)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    dx /= l, dy /= l;
    float nx = -dy, ny = dx, bx = x1 - dx * hl, by = y1 - dy * hl;
    sb(s, "<path fill='#%06X' stroke='#%06X' stroke-width='1.6' stroke-linejoin='round' d='M%.2f %.2f L%.2f %.2f "
          "L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f Z'/>",
        fill, edge, x0 + nx * w / 2, y0 + ny * w / 2, bx + nx * w / 2, by + ny * w / 2, bx + nx * hw / 2, by + ny * hw / 2, x1, y1,
        bx - nx * hw / 2, by - ny * hw / 2, bx - nx * w / 2, by - ny * w / 2, x0 - nx * w / 2, y0 - ny * w / 2);
}

/* The letters, as strokes through a 10 x 14 cell (y down). */
static const char* glyph(char c)
{
    switch (c)
    {
    case 'A': return "M0 14 L5 0 L10 14 M2 9 H8";
    case 'B': return "M0 7 V0 H5 Q9.5 0 9.5 3.5 Q9.5 7 5 7 H0 V14 H5.5 Q10 14 10 10.5 Q10 7 5.5 7";
    case 'C': return "M10 2.5 Q9 0 5 0 Q0 0 0 7 Q0 14 5 14 Q9 14 10 11.5";
    case 'D': return "M0 0 V14 H4 Q10 14 10 7 Q10 0 4 0 Z";
    case 'E': return "M10 0 H0 V14 H10 M0 7 H7.5";
    case 'G': return "M10 2.5 Q9 0 5 0 Q0 0 0 7 Q0 14 5 14 Q10 14 10 9 V7.5 H6";
    case 'H': return "M0 0 V14 M10 0 V14 M0 7 H10";
    case 'I': return "M5 0 V14 M1.5 0 H8.5 M1.5 14 H8.5";
    case 'L': return "M0 0 V14 H9";
    case 'M': return "M0 14 V0 L5 8 L10 0 V14";
    case 'N': return "M0 14 V0 L10 14 V0";
    case 'P': return "M0 14 V0 H5.5 Q10 0 10 4 Q10 8 5.5 8 H0";
    case 'R': return "M0 14 V0 H5.5 Q10 0 10 4 Q10 8 5.5 8 H0 M5 8 L10 14";
    case 'S': return "M10 2.5 Q9 0 5 0 Q0 0 0 3.6 Q0 7 5 7 Q10 7 10 10.4 Q10 14 5 14 Q1 14 0 11.5";
    case 'T': return "M0 0 H10 M5 0 V14";
    case 'U': return "M0 0 V9 Q0 14 5 14 Q10 14 10 9 V0";
    case 'V': return "M0 0 L5 14 L10 0";
    case 'X': return "M0 0 L10 14 M10 0 L0 14";
    case '1': return "M2.5 3 L6 0 V14 M2.5 14 H9.5";
    case '2': return "M0.5 3 Q1.5 0 5 0 Q10 0 10 4 Q10 7 0 14 H10";
    case '3': return "M0.5 1.5 Q2 0 5 0 Q9.5 0 9.5 3.5 Q9.5 7 5 7 Q10 7 10 10.5 Q10 14 5 14 Q1.5 14 0 12.5";
    case '4': return "M7.5 14 V0 L0 10 H10";
    case '5': return "M9.5 0 H1 L0.5 6.5 Q2.5 5.5 5 5.5 Q10 5.5 10 9.75 Q10 14 5 14 Q1.5 14 0 12.5";
    }
    return "";
}

/* A word centred on (cx, cy), h units tall at most and maxw wide, its strokes wt wide, outlined. */
static void word(Sb* s, const char* w, float cx, float cy, float h, float maxw, float wt, uint32_t fill, uint32_t edge, float edge_wt)
{
    int n = (int)strlen(w);
    const float gap = 3.4f;
    float k = h / 14.0f, total = (n * 10 + (n - 1) * gap) * k;
    if (total > maxw)
        k *= maxw / total, total = maxw;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i)
            sb(s, "<path transform='translate(%.2f %.2f) scale(%.4f)' fill='none' stroke='#%06X' stroke-width='%.2f' "
                  "stroke-linecap='round' stroke-linejoin='round' d='%s'/>",
                cx - total / 2 + i * (10 + gap) * k, cy - 7 * k, k, pass ? fill : edge, (pass ? wt : wt + 2 * edge_wt) / k,
                glyph(w[i]));
}

/* A sword from its point at the top left to its pommel at the bottom right. */
static void sword(Sb* s)
{
    linear(s, "blade", 0, 1, 1, 0, 0xFCFCFF, 0xB8BAC4, 0x5C5E68);
    sb(s, "<path fill='url(#blade)' stroke='#101014' stroke-width='1.5' stroke-linejoin='round' "
          "d='M4 4 L46.2 41.8 L41.8 46.2 Z'/>"
          "<path fill='none' stroke='#7C7E8A' stroke-width='1' d='M9 9 L42 42'/>"
          "<path fill='none' stroke='#1A1008' stroke-width='6.5' stroke-linecap='round' d='M47 47 L56 56'/>"
          "<path fill='none' stroke='#7A4420' stroke-width='3.6' stroke-linecap='round' d='M47 47 L56 56'/>"
          "<path fill='none' stroke='#1A1008' stroke-width='7' stroke-linecap='round' d='M36.5 52.5 L52.5 36.5'/>"
          "<path fill='none' stroke='#E8C060' stroke-width='4' stroke-linecap='round' d='M36.5 52.5 L52.5 36.5'/>"
          "<circle cx='57.5' cy='57.5' r='3.6' fill='#E8C060' stroke='#1A1008' stroke-width='1.4'/>");
}

/* A stone orb behind an arrow. */
static void stone(Sb* s)
{
    radial(s, "stone", 0.38f, 0.32f, 0xB8ACA4, 0x5E524C, 0x201A18);
    sb(s, "<circle cx='34' cy='29' r='25.5' fill='url(#stone)' stroke='#0C0A0A' stroke-width='2.4'/>"
          "<ellipse cx='28' cy='17' rx='12' ry='5.5' fill='#FFFFFF' fill-opacity='0.22'/>");
}

/* The dark badge: a steel rim, two crescents of colour on it, a dark face. */
static void badge(Sb* s, const char* crescent)
{
    linear(s, "rim", 0, 0, 1, 1, 0xC8CAD2, 0x6A6C76, 0x2A2B30);
    radial(s, "face", 0.4f, 0.32f, 0x5A5C66, 0x24252B, 0x0A0A0D);
    sb(s, "<circle cx='32' cy='32' r='30.5' fill='#08080A'/><circle cx='32' cy='32' r='28.8' fill='url(#rim)'/>");
    sector(s, 21.5f, 29.2f, -98, 2, crescent);
    sector(s, 21.5f, 29.2f, 112, 200, crescent);
    sb(s, "<circle cx='32' cy='32' r='21.6' fill='url(#face)' stroke='#050506' stroke-width='1.4'/>"
          "<ellipse cx='27' cy='19' rx='11' ry='4.5' fill='#FFFFFF' fill-opacity='0.16'/>");
}

/* A glossy orb filling the icon. */
static void orb(Sb* s, const char* id, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t edge)
{
    radial(s, id, 0.38f, 0.3f, c0, c1, c2);
    sb(s, "<circle cx='32' cy='32' r='29' fill='url(#%s)' stroke='#%06X' stroke-width='2.6'/>"
          "<ellipse cx='26' cy='15.5' rx='13' ry='6' fill='#FFFFFF' fill-opacity='0.3'/>", id, edge);
}

/* An eighth note. */
static void note(Sb* s, uint32_t fill, uint32_t edge)
{
    const char* d = "M24 50 Q17 50 17 45 Q17 39 25 38.5 Q28.5 38.5 30.5 40 V12 Q37 17 44 20 Q48 24 45 33 Q43 26 34 22 V44 Q34 50 24 50 Z";
    sb(s, "<path fill='#%06X' stroke='#%06X' stroke-width='2.6' stroke-linejoin='round' d='%s'/>", fill, edge, d);
}

/* A die seen from a corner: an ivory top, two shaded sides, pips on each (red ones for snake eye). */
static void die(Sb* s, uint32_t pip)
{
    sb(s, "<path fill='#FAF6E8' stroke='#14120E' stroke-width='2.4' stroke-linejoin='round' d='M32 5 L56 19 L32 33 L8 19 Z'/>"
          "<path fill='#D2CBBA' stroke='#14120E' stroke-width='2.4' stroke-linejoin='round' d='M8 19 L32 33 L32 61 L8 47 Z'/>"
          "<path fill='#A69E8E' stroke='#14120E' stroke-width='2.4' stroke-linejoin='round' d='M32 33 L56 19 L56 47 L32 61 Z'/>"
          "<path fill='none' stroke='#FFFFFF' stroke-opacity='0.55' stroke-width='1.2' d='M12 19.4 L32 8 L52 19.4'/>");
    /* one on top, two on the left, three on the right, each squashed to its face */
    sb(s, "<ellipse cx='32' cy='19' rx='5' ry='2.9' fill='#%06X'/>", pip);
    const float left[2][2] = { { 15.2f, 31.6f }, { 24.8f, 48.4f } }, right[3][2] = { { 38, 37.5f }, { 44, 42 }, { 50, 46.5f } };
    for (int i = 0; i < 2; ++i)
        sb(s, "<ellipse transform='translate(%.1f %.1f) rotate(30)' rx='2.6' ry='3.4' fill='#%06X'/>", left[i][0], left[i][1], pip);
    for (int i = 0; i < 3; ++i)
        sb(s, "<ellipse transform='translate(%.1f %.1f) rotate(-30)' rx='2.6' ry='3.4' fill='#%06X'/>", right[i][0], right[i][1], pip);
}

/* A five-pointed star centred on (cx, cy), r to its points. */
static void star(Sb* s, float cx, float cy, float r, uint32_t fill, uint32_t edge)
{
    sb(s, "<path fill='#%06X' stroke='#%06X' stroke-width='1.6' stroke-linejoin='round' d='", fill, edge);
    for (int i = 0; i < 10; ++i)
    {
        float a = (-90.0f + 36.0f * i) * 3.14159265f / 180.0f, rr = i & 1 ? r * 0.45f : r;
        sb(s, "%c%.2f %.2f ", i ? 'L' : 'M', cx + rr * cosf(a), cy + rr * sinf(a));
    }
    sb(s, "Z'/>");
}

/* A daze's digit, green, in the badge's lower right. */
static void daze_digit(Sb* s, const char* d)
{
    word(s, d, 50, 50, 15, 14, 4.2f, 0xB4E27A, 0x10200A, 1.4f);
}

static int build_template(const Template* t, char* buf, size_t n)
{
    Sb s = { buf, n, 0, 0 };
    sb(&s, "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' viewBox='0 0 64 64'>");
    switch (t->kind)
    {
    case T_UP_SWORD:
        sword(&s);
        arrow(&s, 14, 40, 56, 5, 5.5f, 15, 16, 0xFF2A1C, 0x3A0404);
        word(&s, "UP", 18, 50, 17, 32, 4.6f, 0xF27C9C, 0x2A0612, 1.4f);
        break;
    case T_UP_ORB:
        stone(&s);
        arrow(&s, 14, 40, 54, 6, 5.5f, 15, 16, 0xFF2A1C, 0x3A0404);
        word(&s, "UP", 20, 50, 17, 34, 4.6f, 0xF27C9C, 0x2A0612, 1.4f);
        break;
    case T_DN_SWORD:
        sword(&s);
        arrow(&s, 57, 6, 20, 38, 5.5f, 15, 16, 0x2050FF, 0x020A30);
        word(&s, "DN", 20, 50, 17, 34, 4.6f, 0x2C86F0, 0x020A30, 1.4f);
        break;
    case T_DN_ORB:
        stone(&s);
        arrow(&s, 56, 6, 20, 38, 5.5f, 15, 16, 0x2050FF, 0x020A30);
        word(&s, "DN", 20, 50, 17, 34, 4.6f, 0x2C86F0, 0x020A30, 1.4f);
        break;
    case T_BADGE_UP:
    case T_BADGE_DN:
    {
        int up = t->kind == T_BADGE_UP;
        badge(&s, up ? "#D0141C" : "#1E5CD8");
        word(&s, t->word, 32, 33, 17, 44, 4.4f, up ? 0xF6C488 : 0xA2EEDA, up ? 0x2A1404 : 0x04201A, 1.3f);
        break;
    }
    case T_AIM_UP:
    case T_AIM_DN:
        badge(&s, t->kind == T_AIM_UP ? "#D0141C" : "#1E5CD8");
        for (int pass = 0; pass < 2; ++pass)
        {
            uint32_t c = pass ? 0x3CFF6A : 0x02200A;
            float w = pass ? 2.8f : 5.2f;
            sb(&s, "<circle cx='32' cy='32' r='9.5' fill='none' stroke='#%06X' stroke-width='%.1f'/>"
                   "<path fill='none' stroke='#%06X' stroke-width='%.1f' stroke-linecap='round' "
                   "d='M32 16 V25 M32 39 V48 M16 32 H25 M39 32 H48'/><circle cx='32' cy='32' r='%.1f' fill='#%06X'/>",
                c, w, c, w, pass ? 2.2f : 3.4f, c);
        }
        break;
    case T_EVA_UP:
    case T_EVA_DN:
        badge(&s, t->kind == T_EVA_UP ? "#D0141C" : "#1E5CD8");
        for (int pass = 0; pass < 2; ++pass)
            sb(&s, "<path fill='none' stroke='#%06X' stroke-width='%.1f' stroke-linecap='round' "
                   "d='M15 23 H38 M21 30 H49 M13 37 H37 M23 44 H45'/>",
                pass ? 0x46E8E8 : 0x022024, pass ? 3.6f : 6.2f);
        break;
    case T_SP:
        orb(&s, "pale", 0xFFFFFF, 0xD6E2DA, 0x8EA49A, 0x16301C);
        word(&s, "SP", 32, 34, 20, 44, 5.0f, 0x9A6CEA, 0x2A1460, 1.1f);
        break;
    case T_NAVY:
        orb(&s, "navy", 0x92A8F0, 0x2A3C92, 0x081030, 0x03061A);
        word(&s, t->word, 32, 33, 18, 44, 4.8f, t->color, 0x0A0A1C, 1.4f);
        break;
    case T_MP_ORB:
        orb(&s, "lav", 0xEAE4FA, 0x9284C2, 0x3A2E62, 0x140E24);
        word(&s, "MP", 32, 34, 19, 44, 5.0f, 0xFFF040, 0x3A2C00, 1.4f);
        break;
    case T_SONG_UP:
    case T_SONG_DN:
    case T_SONG:
    {
        int up = t->kind == T_SONG_UP;
        if (up)
            orb(&s, "song", 0xFFF4F4, 0xE4A6AE, 0x8C3A4A, 0x1C0608);
        else
            orb(&s, "song", 0xFFFFFF, 0xB6B6EC, 0x585AA2, 0x0C0C24);
        if (t->kind != T_SONG)
        {
            const char* c = up ? "#D0141C" : "#1E5CD8";
            sector(&s, 23.0f, 30.4f, -96, 6, c);
            sector(&s, 23.0f, 30.4f, 108, 196, c);
        }
        note(&s, 0xFFFFFF, up ? 0x5A1420 : 0x24245A);
        break;
    }
    case T_HASTE:
        orb(&s, "pink", 0xFFF4EA, 0xF28C94, 0xA0283A, 0x1C0408);
        arrow(&s, 15, 48, 50, 13, 6.0f, 16, 18, 0xFF2A1C, 0x3A0404);
        break;
    case T_LEVEL:
        orb(&s, "gold", 0xFFFAB0, 0xF0C420, 0x8A6000, 0x1E1200);
        sb(&s, "<rect x='13' y='15' width='38' height='7' rx='1.5' fill='#D41818' stroke='#2A0404' stroke-width='1.4'/>");
        word(&s, "LV", 32, 37, 17, 30, 5.0f, 0x6A1010, 0xFFF4D0, 1.2f);
        break;
    case T_DIE:
        die(&s, t->word && !strcmp(t->word, "red") ? 0xE01818 : 0x2A2622);
        if (t->word && !strcmp(t->word, "plus"))
            for (int pass = 0; pass < 2; ++pass)
                sb(&s, "<path fill='none' stroke='#%06X' stroke-width='%.1f' stroke-linecap='round' d='M50 4 V22 M41 13 H59'/>",
                    pass ? 0x3CE8F0 : 0x022428, pass ? 4.6f : 8.0f);
        break;
    case T_IMAGERY:
        radial(&s, "glass", 0.38f, 0.3f, 0x6A7480, 0x1E2230, 0x05060C);
        linear(&s, "iris", 0, 0, 1, 1, 0xF080C8, 0x80E0F0, 0xF0E070);
        sb(&s, "<circle cx='32' cy='32' r='28' fill='url(#glass)' stroke='url(#iris)' stroke-width='3.2'/>"
               "<circle cx='32' cy='32' r='30.2' fill='none' stroke='#08080C' stroke-width='1.4'/>");
        star(&s, 19, 37, 10, 0xF0D830, 0x2A2000);
        star(&s, 45, 37, 10, 0xF0D830, 0x2A2000);
        star(&s, 32, 29, 11.5f, 0xFFE840, 0x2A2000);
        sb(&s, "<ellipse cx='26' cy='14' rx='13' ry='5.5' fill='#FFFFFF' fill-opacity='0.28'/>");
        break;
    case T_DAZE_SPEED:
        badge(&s, "#1E5CD8");
        for (int pass = 0; pass < 2; ++pass)
            sb(&s, "<path fill='none' stroke='#%06X' stroke-width='%.1f' stroke-linecap='round' "
                   "d='M13 20 H34 M19 27 H45 M11 34 H33 M19 41 H38'/>",
                pass ? 0x46E8E8 : 0x022024, pass ? 3.6f : 6.2f);
        daze_digit(&s, t->word);
        break;
    case T_DAZE_SWORD:
        badge(&s, "#1E5CD8");
        linear(&s, "dblade", 0, 1, 1, 0, 0xFCFCFF, 0xB8BAC4, 0x5C5E68);
        sb(&s, "<path fill='url(#dblade)' stroke='#101014' stroke-width='1.4' stroke-linejoin='round' d='M7 7 L41.4 37.6 L37.6 41.4 Z'/>"
               "<path fill='none' stroke='#1A1008' stroke-width='6' stroke-linecap='round' d='M32 47 L47 32'/>"
               "<path fill='none' stroke='#E8C060' stroke-width='3.4' stroke-linecap='round' d='M32 47 L47 32'/>"
               "<path fill='none' stroke='#1A1008' stroke-width='5.6' stroke-linecap='round' d='M42 42 L48 48'/>"
               "<path fill='none' stroke='#7A4420' stroke-width='3' stroke-linecap='round' d='M42 42 L48 48'/>");
        daze_digit(&s, t->word);
        break;
    case T_DAZE_GEM:
        badge(&s, "#1E5CD8");
        radial(&s, "gem", 0.4f, 0.35f, 0xE0A0FF, 0x8A30E0, 0x3A0880);
        sb(&s, "<path fill='url(#gem)' stroke='#14022A' stroke-width='1.6' stroke-linejoin='round' "
               "d='M29 13 L39 18 L42 29 L37 39 L27 42 L18 37 L15 27 L20 17 Z'/>"
               "<path fill='none' stroke='#F0D0FF' stroke-opacity='0.7' stroke-width='1.2' d='M29 20 L35 23 L37 29 L34 35 L28 37 L22 34 L21 28 L24 22 Z'/>"
               "<circle cx='28.5' cy='28' r='2.6' fill='#2A0858'/>");
        daze_digit(&s, t->word);
        break;
    }
    sb(&s, "</svg>");
    return !s.full;
}

/* Design t (an index into g_designs, then g_art) as an SVG document. */
static int build(size_t t, char* buf, size_t n)
{
    if (t < NDESIGNS)
        return build_template(&g_designs[t], buf, n);
    int k = snprintf(buf, n, "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' viewBox='0 0 64 64'>%s</svg>",
        g_art[t - NDESIGNS].body);
    return k > 0 && (size_t)k < n;
}

uint8_t* vicons_rasterize(const char* svg, int size)
{
    size_t n = strlen(svg);
    char* copy = (char*)malloc(n + 1); /* nanosvg parses in place */
    uint8_t* px = (uint8_t*)malloc((size_t)size * size * 4);
    NSVGimage* img = NULL;
    NSVGrasterizer* r = NULL;
    if (copy && px)
    {
        memcpy(copy, svg, n + 1);
        img = nsvgParse(copy, "px", 96.0f);
        r = nsvgCreateRasterizer();
    }
    if (!img || !r)
    {
        free(px);
        px = NULL;
    }
    else
        nsvgRasterize(r, img, 0, 0, (float)size / 64.0f, px, size, size, size * 4);
    if (r)
        nsvgDeleteRasterizer(r);
    if (img)
        nsvgDelete(img);
    free(copy);
    return px;
}

/* ---- recognising the game's icons -------------------------------------------------------------- */

#define ICON_FILE 87
#define ICON_RS 0x1800
#define ICON_AT 0x280
#define ICON_PX (32 * 32)

static ViconsRead g_read;
static int g_loaded;
static uint8_t (*g_icons)[ICON_PX * 4]; /* every record's icon, RGBA top-down, alpha 0 or 255 */
static uint32_t g_nicons;
static int16_t* g_template_of;          /* by record: an index into g_designs (the templates), or -1 */

void vicons_init(ViconsRead read)
{
    g_read = read;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The install's path of a file id: VTABLE.DAT says which ROM folder (1 = ROM), FTABLE.DAT where in it. */
static int file_path(uint32_t id, char* out, size_t n)
{
    for (int r = 1; r <= 9; ++r)
    {
        char vr[32], fr[32];
        if (r == 1)
            snprintf(vr, sizeof vr, "VTABLE.DAT"), snprintf(fr, sizeof fr, "FTABLE.DAT");
        else
            snprintf(vr, sizeof vr, "ROM%d/VTABLE%d.DAT", r, r), snprintf(fr, sizeof fr, "ROM%d/FTABLE%d.DAT", r, r);
        size_t vn = 0, fn = 0;
        uint8_t* v = g_read(vr, &vn);
        uint8_t* f = v && id < vn && v[id] == r ? g_read(fr, &fn) : NULL;
        int ok = f && (size_t)id * 2 + 2 <= fn;
        if (ok)
        {
            uint32_t x = (uint32_t)f[id * 2] | f[id * 2 + 1] << 8;
            if (r == 1)
                snprintf(out, n, "ROM/%u/%u.DAT", x >> 7, x & 0x7F);
            else
                snprintf(out, n, "ROM%d/%u/%u.DAT", r, x >> 7, x & 0x7F);
        }
        free(v);
        free(f);
        if (ok)
            return 1;
    }
    return 0;
}

static void load(void)
{
    g_loaded = 1;
    char rel[64];
    size_t size = 0;
    uint8_t* d = g_read && file_path(ICON_FILE, rel, sizeof rel) ? g_read(rel, &size) : NULL;
    uint32_t n = d ? (uint32_t)(size / ICON_RS) : 0;
    g_icons = n ? calloc(n, sizeof *g_icons) : NULL;
    g_template_of = n ? malloc(n * sizeof *g_template_of) : NULL;
    if (!g_icons || !g_template_of)
    {
        free(d);
        free(g_icons), free(g_template_of);
        g_icons = NULL, g_template_of = NULL;
        fprintf(stderr, "[vicons] the status icons (file %d) cannot be read: none is replaced\n", ICON_FILE);
        return;
    }
    g_nicons = n;
    uint8_t* ok = calloc(n, 1);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* b = d + (size_t)i * ICON_RS + ICON_AT;
        uint32_t len = rd32(b);
        const uint8_t* dib = b + 21;
        if (len < 17 + 40 + ICON_PX * 4 || len > ICON_RS - ICON_AT - 4)
            continue;
        uint32_t hs = rd32(dib);
        int32_t w = (int32_t)rd32(dib + 4), h = (int32_t)rd32(dib + 8);
        uint16_t bpp = (uint16_t)(dib[14] | dib[15] << 8);
        if (w != 32 || (h != 32 && h != -32) || bpp != 32 || hs + ICON_PX * 4 > len - 17)
            continue;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                const uint8_t* p = dib + hs + ((h > 0 ? 31 - y : y) * 32 + x) * 4; /* B G R A, bottom-up when h > 0 */
                uint8_t* q = g_icons[i] + (y * 32 + x) * 4;
                q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3] ? 255 : 0;
            }
        ok[i] = 1;
    }
    free(d);
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        g_template_of[i] = -1;
        for (size_t t = 0; t < NALL && ok[i]; ++t)
        {
            uint32_t rep = rep_of(t);
            if (rep < n && ok[rep] && !memcmp(g_icons[i], g_icons[rep], sizeof *g_icons))
            {
                g_template_of[i] = (int16_t)t;
                mapped++;
                break;
            }
        }
    }
    free(ok);
    fprintf(stderr, "[vicons] %u status icons, %u of them drawn from %u designs\n", n, mapped, (unsigned)NALL);
}

/* A 32x32 DXT3 or A8R8G8B8 level as RGBA, top-down; 0 for other formats. */
static int decode(uint32_t format, const uint8_t* src, uint32_t pitch, uint8_t* out)
{
    if (format == 21) /* A8R8G8B8 */
    {
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                const uint8_t* p = src + y * pitch + x * 4;
                uint8_t* q = out + (y * 32 + x) * 4;
                q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3];
            }
        return 1;
    }
    if (format != 0x33545844) /* 'DXT3' */
        return 0;
    for (int by = 0; by < 8; ++by)
        for (int bx = 0; bx < 8; ++bx)
        {
            const uint8_t* b = src + by * pitch + bx * 16;
            uint32_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8, idx = rd32(b + 12);
            int pal[4][3];
            for (int k = 0; k < 2; ++k)
            {
                uint32_t c = k ? c1 : c0;
                pal[k][0] = (int)((c >> 11) & 31) * 255 / 31;
                pal[k][1] = (int)((c >> 5) & 63) * 255 / 63;
                pal[k][2] = (int)(c & 31) * 255 / 31;
            }
            for (int ch = 0; ch < 3; ++ch)
                pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3, pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
            for (int i = 0; i < 16; ++i)
            {
                uint8_t* q = out + ((by * 4 + i / 4) * 32 + bx * 4 + i % 4) * 4;
                const int* c = pal[(idx >> (2 * i)) & 3];
                q[0] = (uint8_t)c[0], q[1] = (uint8_t)c[1], q[2] = (uint8_t)c[2];
                q[3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
            }
        }
    return 1;
}

/* The record whose icon the upload is, or -1: the nearest by colour where opaque, an alpha that
 * disagrees counting as a full 255. */
static int nearest(const uint8_t* u)
{
    int best = -1;
    uint32_t best_e = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < g_nicons; ++i)
    {
        const uint8_t* d = g_icons[i];
        uint32_t e = 0, opaque = 0;
        for (int p = 0; p < ICON_PX && e < best_e; ++p)
        {
            const uint8_t *a = u + p * 4, *b = d + p * 4;
            if ((a[3] != 0) != (b[3] != 0))
                e += 3 * 255;
            else if (b[3])
                e += (uint32_t)abs(a[0] - b[0]) + (uint32_t)abs(a[1] - b[1]) + (uint32_t)abs(a[2] - b[2]), opaque++;
        }
        if (e < best_e && opaque)
            best_e = e, best = (int)i;
    }
    /* DXT3 leaves ~6-8 a channel on average where opaque; another icon is 18 or more away */
    return best >= 0 && best_e <= 13u * 3u * ICON_PX ? best : -1;
}


uint8_t* vicons_texture(uint32_t w, uint32_t h, uint32_t format, const uint8_t* level0, uint32_t pitch, uint32_t* size)
{
    if (w != 32 || h != 32 || !g_read)
        return NULL;
    uint8_t u[ICON_PX * 4];
    if (!decode(format, level0, pitch, u))
        return NULL;
    if (!g_loaded)
        load();
    int rec = g_icons ? nearest(u) : -1;
    if (rec < 0 || g_template_of[rec] < 0)
        return NULL;
    static char svg[16384];
    if (!build((size_t)g_template_of[rec], svg, sizeof svg))
        return NULL;
    uint8_t amax = 0; /* the game's opaque alpha (0x80 in the DAT; 136 once in DXT3) */
    for (int p = 0; p < ICON_PX; ++p)
        amax = u[p * 4 + 3] > amax ? u[p * 4 + 3] : amax;
    const int n = 128;
    uint8_t* px = vicons_rasterize(svg, n);
    if (!px)
        return NULL;
    for (int p = 0; p < n * n; ++p) /* RGBA, straight alpha -> B G R A, with the game's alpha scale */
    {
        uint8_t* q = px + p * 4, r = q[0];
        q[0] = q[2], q[2] = r, q[3] = (uint8_t)((q[3] * amax + 127) / 255);
    }
    *size = (uint32_t)n;
    return px;
}

int vicons_svg(uint32_t id, char* buf, size_t n)
{
    for (size_t i = 0; i < NALL; ++i)
        if (rep_of(i) == id)
            return build(i, buf, n);
    return 0;
}
