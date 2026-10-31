/* The game's size for the screen it is on (iOS and Android: docs/ios.md): the back buffer at the safe
 * area's pixels within a pixel budget, the interface at about ui_points points a pixel within 640x480
 * to 1200 lines (the web port's rule, tools/web/hud.js), presented into the safe area. */
#pragma once

typedef struct DisplayIn
{
    int win_w, win_h;           /* the window, in points */
    float density;              /* pixels a point */
    int safe_x, safe_y, safe_w, safe_h; /* the safe area, in points */
    float ui_points;            /* points an interface pixel the player wants */
    double max_pixels;          /* the back buffer's budget */
} DisplayIn;

typedef struct DisplayOut
{
    int w, h;                   /* the back buffer */
    int menu_w, menu_h;         /* the interface */
    int rect_x, rect_y, rect_w, rect_h; /* where it is presented, in the drawable's pixels */
} DisplayOut;

/* The sizes for a screen (no SDL: tests/display_fit_test.c). */
void display_policy(const DisplayIn* in, DisplayOut* out);

/* The sign-in's window, before the registry is read: the game's first size, put in the registry
 * (0001/0002 the back buffer, 0037/0038 the interface), so it starts at it. */
void display_fit_start(void* sdl_window);
/* Each frame (host64's present hook): when the window's size or safe area changed, the game follows
 * (modern_window_size), presented into the safe area, the pointer mapped to match. */
void display_fit_frame(void);
