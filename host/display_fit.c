/* The game's size for the screen it is on. See display_fit.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_fit.h"

enum
{
    MIN_UI_W = 640, /* the web port's floor; the game lays its windows out in at least 512x448 */
    MIN_UI_H = 480,
    MAX_UI_H = 1200, /* past this the interface's text is too small to read */
};

void display_policy(const DisplayIn* in, DisplayOut* out)
{
    float d = in->density > 0.0f ? in->density : 1.0f;
    int sx = in->safe_x, sy = in->safe_y, sw = in->safe_w, sh = in->safe_h;
    if (sw <= 0 || sh <= 0) /* no safe area: the window */
        sx = 0, sy = 0, sw = in->win_w, sh = in->win_h;
    if (sw <= 0 || sh <= 0)
        sw = 1280, sh = 720;
    /* the back buffer: the safe area's pixels, scaled down to the budget */
    double px = d;
    if (in->max_pixels > 0 && (double)sw * sh * d * d > in->max_pixels)
        px = sqrt(in->max_pixels / ((double)sw * sh));
    int w = (int)lround(sw * px), h = (int)lround(sh * px);
    out->w = (w < 320 ? 320 : w) & ~1;
    out->h = (h < 240 ? 240 : h) & ~1;
    /* the interface: ui_points points a pixel, kept within MIN_UI to MAX_UI_H lines */
    double u = in->ui_points > 0.0f ? in->ui_points : 1.0;
    if (sh / u > MAX_UI_H)
        u = (double)sh / MAX_UI_H;
    if (sw / u < MIN_UI_W || sh / u < MIN_UI_H)
        u = fmin((double)sw / MIN_UI_W, (double)sh / MIN_UI_H);
    out->menu_w = (int)lround(sw / u) & ~1;
    out->menu_h = (int)lround(sh / u) & ~1;
    out->rect_x = (int)lround(sx * d), out->rect_y = (int)lround(sy * d);
    out->rect_w = (int)lround(sw * d), out->rect_h = (int)lround(sh * d);
}

#ifndef DISPLAY_FIT_POLICY_ONLY
#include <SDL3/SDL.h>

#include "d3d8.h"
#include "gfx.h"
#include "k32.h"
#include "modern.h"
#include "user32.h"

#define SETTINGS_KEY "HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI"

static SDL_Window* g_window;
static DisplayIn g_last;

/* What the window is now. FFXI_MAX_PIXELS and FFXI_UI_POINTS override the defaults: a phone (under
 * 600 points high) about 2.4 MP, two thirds of its native pixels, as it runs hot and on battery; an
 * iPad 4 MP. */
static int read_window(SDL_Window* win, DisplayIn* in)
{
    memset(in, 0, sizeof *in);
    int pw = 0, ph = 0;
    SDL_GetWindowSize(win, &in->win_w, &in->win_h);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (in->win_w <= 0 || in->win_h <= 0)
        return 0;
    in->density = (float)pw / (float)in->win_w;
    SDL_Rect safe;
    if (SDL_GetWindowSafeArea(win, &safe))
        in->safe_x = safe.x, in->safe_y = safe.y, in->safe_w = safe.w, in->safe_h = safe.h;
    const char* e = getenv("FFXI_UI_POINTS");
    in->ui_points = e ? (float)atof(e) : 1.0f;
    e = getenv("FFXI_MAX_PIXELS");
    int short_side = in->win_w < in->win_h ? in->win_w : in->win_h;
    in->max_pixels = e ? atof(e) : short_side < 600 ? 2.4e6 : 4.0e6;
    return 1;
}

void display_fit_start(void* sdl_window)
{
    g_window = (SDL_Window*)sdl_window;
    DisplayIn in;
    DisplayOut o;
    if (!g_window || !read_window(g_window, &in))
        return;
    display_policy(&in, &o);
    reg_set_dword(SETTINGS_KEY, "0001", (uint32_t)o.w);
    reg_set_dword(SETTINGS_KEY, "0002", (uint32_t)o.h);
    reg_set_dword(SETTINGS_KEY, "0037", (uint32_t)o.menu_w);
    reg_set_dword(SETTINGS_KEY, "0038", (uint32_t)o.menu_h);
    fprintf(stderr, "[display] %dx%d points at %.2f, safe %dx%d at %d,%d: the game at %dx%d, the interface %dx%d\n",
        in.win_w, in.win_h, in.density, in.safe_w, in.safe_h, in.safe_x, in.safe_y, o.w, o.h, o.menu_w, o.menu_h);
}

void display_fit_frame(void)
{
    DisplayIn in;
    if (!g_window || !read_window(g_window, &in) || !memcmp(&in, &g_last, sizeof in))
        return;
    g_last = in;
    DisplayOut o;
    display_policy(&in, &o);
    modern_window_size(o.w, o.h, o.menu_w, o.menu_h);
    gfx_set_present_rect(o.rect_x, o.rect_y, o.rect_w, o.rect_h);
    /* the pointer, in points, through the same rectangle */
    user32_set_view((float)o.rect_x / in.density, (float)o.rect_y / in.density, (float)o.rect_w / in.density,
        (float)o.rect_h / in.density);
    fprintf(stderr, "[display] presented into %dx%d at %d,%d of the drawable; the game %dx%d, the interface %dx%d\n",
        o.rect_w, o.rect_h, o.rect_x, o.rect_y, o.w, o.h, o.menu_w, o.menu_h);
}
#endif
