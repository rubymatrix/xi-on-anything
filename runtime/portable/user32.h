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
/* The SDL_Window behind a guest HWND (for the graphics layer), or NULL. */
void* user32_sdl_window(uint32_t hwnd);
/* A window's client size (unchanged if hwnd is not ours); the desktop's mode. */
void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz);
/* --ui-aspect: the game's screen-space draws (its interface) keep width / height = aspect, centered
 * in a wider window (full height) or a taller one (full width), and the mouse is mapped to match. 0
 * (the default) is off. */
void user32_set_ui_aspect(float aspect);
/* The game's window in one of its modes (registry 0034: 0 full screen, 1 a window, 2 borderless, 3
 * borderless over the desktop) at w x h, while it runs */
void user32_set_window(uint32_t hwnd, int mode, int w, int h);
float user32_ui_aspect(void);
/* The fraction of hwnd's width the interface keeps: 1 when off, or the window is not wider. */
float user32_ui_squeeze(uint32_t hwnd);
/* The fraction of hwnd's height the interface keeps: 1 when off, or the window is not taller. */
float user32_ui_squeeze_y(uint32_t hwnd);
/* Set by the graphics layer: whether the interface covers this point of the window (0..1 across
 * and down), as drawn last frame. The mouse is unsqueezed only there; the orbit camera's wheel zooms
 * only elsewhere. */
extern int (*user32_ui_hit)(float fx, float fy);
/* Set by the addon host: sees every SDL event (an SDL_Event*) first, with the size of the window it
 * is for (points); returns 1 to keep a press, a wheel turn or typed text from the game (DirectInput
 * and the window messages alike). Releases and motion always reach the game. */
extern int (*user32_event_hook)(const void* sdl_event, int window_w, int window_h);
/* Set by the host's camera (host/camera.c): sees the mouse's motion, presses, releases and wheel
 * turns after the addon overlay (what the overlay took never reaches it), with the window's size;
 * returns 1 to keep one from the game. user32_replay hands the game one it kept earlier (a press that
 * turned out to be a click); user32_mouse_capture(1) hides the cursor and reports motion only
 * (relative), 0 puts it back where it was. */
extern int (*user32_mouse_hook)(const void* sdl_event, int window_w, int window_h);
void user32_replay(const void* sdl_event);
void user32_mouse_capture(int on);
/* The control port's mouse (FFXI_CONTROL): kind 0 a move to x, y (the window's points), 1 a press
 * and 2 a release of button (SDL's: 1 left, 3 right), 3 the wheel turned y notches. */
void user32_inject_mouse(int kind, float x, float y, int button);
/* the mouse id (SDL_MouseID, the events' `which`) of those: no real cursor to hide or move for them */
#define USER32_INJECTED_MOUSE 0x7F11u
/* Set by the addon host: runs once when the player quits (the window closing), before the process
 * ends. */
extern void (*user32_quit_hook)(void);
/* where the game was given the cursor, 0..1 of its window across and down: i = 0 the latest, up to
 * USER32_MOUSE_GIVEN back. Returns 1 when it was given as it is (over the world), 0 unsqueezed (over
 * the interface), -1 when there is no such point. */
#define USER32_MOUSE_GIVEN 8
int user32_mouse_given(int i, float* fx, float* fy);
