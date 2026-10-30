/* USER32 and WINMM on SDL3 (user32.c). */
#pragma once

#include <stdint.h>

void user32_init(void);
/* An SDL_Window already open (host64's sign-in screen, the graphics back end already on it) for
 * the game's first top-level window to take over instead of opening one: one window, start to end. */
void user32_adopt_window(void* sdl_window);
/* A full-screen device (D3D8 Windowed = FALSE) on hwnd: its window covers the display, in the
 * desktop's own mode. */
void user32_set_fullscreen(uint32_t hwnd, int on);
/* The part of the SDL window, in its units, the game's client is presented into (iOS: the screen's
 * safe area, host/display_fit.c): the pointer is mapped from it to the client's pixels, and the
 * client may be any size. w 0 (the default): the client is the window, unit for unit. */
void user32_set_view(float x, float y, float w, float h);
/* A point in the window's units -> the focused client's pixels, when a view is set: 1; else 0, the
 * point unchanged. */
int user32_view_point(float* x, float* y);
/* The SDL_Window behind a guest HWND (for the graphics layer), or NULL. */
void* user32_sdl_window(uint32_t hwnd);
/* A window's client size (unchanged if hwnd is not ours); the desktop's mode. */
void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz);
/* --ui-aspect: the game's screen-space draws (its interface) keep width / height = aspect, centered
 * in a wider window (full height) or a taller one (full width), and the mouse is mapped to match. 0
 * (the default) is off. */
void user32_set_ui_aspect(float aspect);
/* PostMessage from the host: msg to hwnd's window procedure, on its thread, from its queue. */
void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
/* The game's window in one of its modes (registry 0034: 0 full screen, 1 a window, 2 borderless, 3
 * borderless over the desktop) at w x h, while it runs */
void user32_set_window(uint32_t hwnd, int mode, int w, int h);
float user32_ui_aspect(void);
/* The fraction of hwnd's width the interface keeps: 1 when off, or the window is not wider. */
float user32_ui_squeeze(uint32_t hwnd);
/* The fraction of hwnd's height the interface keeps: 1 when off, or the window is not taller. */
float user32_ui_squeeze_y(uint32_t hwnd);
/* Set by the graphics layer: whether the interface covers this point of the window (0..1 across
 * and down), as drawn last frame. The mouse is unsqueezed only there. */
extern int (*user32_ui_hit)(float fx, float fy);
/* Set by the addon host: sees every SDL event (an SDL_Event*) first, with the size of the window it
 * is for (points); returns 1 to keep a press, a wheel turn or typed text from the game (DirectInput
 * and the window messages alike). Releases and motion always reach the game. */
extern int (*user32_event_hook)(const void* sdl_event, int window_w, int window_h);
/* Set by the addon host: runs once when the player quits (the window closing), before the process
 * ends. */
extern void (*user32_quit_hook)(void);
/* Set by Config > Modern: hwnd's window (a window, not full screen; desktop Linux) has a size other
 * than the game gave it - the compositor refused it, or resized or tiled the window since - and the
 * game should draw at it. The game has been sent WM_SIZE; the window already has the size. Runs on
 * the window's thread, from its message pump. */
extern void (*user32_resize_hook)(uint32_t hwnd, int width, int height);
/* where the game was given the cursor, 0..1 of its window across and down: i = 0 the latest, up to
 * USER32_MOUSE_GIVEN back. Returns 1 when it was given as it is (over the world), 0 unsqueezed (over
 * the interface), -1 when there is no such point. */
#define USER32_MOUSE_GIVEN 8
int user32_mouse_given(int i, float* fx, float* fy);
