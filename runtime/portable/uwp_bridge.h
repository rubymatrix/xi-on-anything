/* What the runtime and the UWP app (xbox/) give each other, where SDL3 cannot be used: SDL3 has
 * no UWP back end. sdl_uwp.c implements the SDL calls the runtime and the sign-in screen make
 * (user32.c, input.c, dsound.c, host/signin.c) over these; the app feeds it its window's input and
 * plays its audio.
 *
 * Plain C, callable from the app's C++. Thread-safe: the app calls in from its UI thread, the game
 * reads on its own. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- the app -> the game: its window --------------------------------------------------------------- */
void uwp_key(int sdl_scancode, int down, int repeat); /* an SDL_Scancode */
void uwp_text(const char* utf8);
void uwp_mouse_move(float x, float y); /* in the view's pixels */
void uwp_mouse_button(int sdl_button, int down); /* SDL_BUTTON_LEFT (1), _MIDDLE (2), _RIGHT (3) */
void uwp_mouse_wheel(float dy); /* notches, up positive */
void uwp_focus(int on);
void uwp_close(void);
void uwp_view_size(int w, int h); /* the view's size in pixels; also the "desktop" the game sees */

/* A controller's state, polled by the app: buttons as bit (1 << SDL_GamepadButton), axes in
 * SDL_GamepadAxis order (left x/y, right x/y, left/right trigger), SDL's ranges and signs. */
typedef struct UwpPad
{
    int connected;
    uint32_t buttons;
    int16_t axes[6];
} UwpPad;
void uwp_gamepad(const UwpPad* pad);

/* --- the game -> the app ---------------------------------------------------------------------------------- */
/* 48 kHz stereo float, called from the audio thread every 10 ms; NULL (the default) drops it. */
typedef void (*UwpAudioSink)(const float* frames, int nframes);
void uwp_set_audio_sink(UwpAudioSink sink);
typedef void (*UwpRumble)(uint16_t low, uint16_t high);
void uwp_set_rumble(UwpRumble rumble);

/* The app's CoreWindow (an IUnknown*, the app's reference), for the Direct3D 12 back end's swap
 * chain. There is one window, and one swap chain on it, from the sign-in screen into the game. */
void* uwp_core_window(void);

/* The on-screen keyboard, for a text field of the sign-in screen: show (1) with the field's text
 * so far, or hide (0). Called from the game's thread. While it is up, the app hands the field's whole
 * text back with uwp_replace_text as the player types, and passes none of the window's own keys or
 * characters on. */
enum
{
    UWP_TEXT,        /* plain text */
    UWP_TEXT_SECRET, /* a password, typed hidden */
    UWP_TEXT_NUMBER, /* digits: a one-time code */
};
typedef void (*UwpTextInput)(int show, int kind, const char* text);
void uwp_set_text_input(UwpTextInput cb);
void uwp_replace_text(const char* utf8);

/* The LandSandBoat sign-in's TLS (host/lsb_login.c): one request, one reply, the server's
 * certificate not checked (private servers' are self-signed). SChannel is not available to UWP
 * apps; the app implements this over Windows.Networking.Sockets. 1, or 0 with a message. */
int uwp_tls_exchange(uint32_t server_ipv4, uint16_t port, const char* request, char* reply, size_t replyn, char* err,
    size_t errn);

/* The app also implements host/keychain.h (saved passwords and trust tokens), over the Windows
 * credential locker. */

#ifdef __cplusplus
}
#endif
