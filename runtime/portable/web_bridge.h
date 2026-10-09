/* What the runtime and the browser shell (tools/web/) give each other, where SDL3 is not linked:
 * sdl_web.c implements the SDL calls the runtime makes (user32.c, input.c, dsound.c) over these, as
 * sdl_uwp.c does for the Xbox. The shell calls them from the page's main thread (they only queue);
 * the game reads on its own threads. All are exported to JS (Module._web_key and so on). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- the shell -> the game: its canvas --------------------------------------------------------------- */
void web_key(int sdl_scancode, int down, int repeat); /* an SDL_Scancode */
void web_text(const char* utf8);
void web_mouse_move(float x, float y); /* in the canvas's device pixels */
void web_mouse_button(int sdl_button, int down); /* SDL_BUTTON_LEFT (1), _MIDDLE (2), _RIGHT (3) */
void web_mouse_wheel(float dy); /* notches, up positive */
void web_focus(int on);
void web_close(void);
void web_view_size(int w, int h); /* the canvas's size in device pixels; also the "desktop" the game sees */

/* A controller's state, from the Gamepad API: buttons as bit (1 << SDL_GamepadButton), axes in
 * SDL_GamepadAxis order (left x/y, right x/y, left/right trigger), SDL's ranges and signs. */
void web_gamepad(int connected, uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, int16_t lt,
    int16_t rt);

/* --- the game -> the shell --------------------------------------------------------------------------- */
/* 48 kHz stereo float, called from the audio thread every 10 ms; NULL (the default) drops it. */
typedef void (*WebAudioSink)(const float* frames, int nframes);
void web_set_audio_sink(WebAudioSink sink);

#ifdef __cplusplus
}
#endif
