/* host/padmap.c with SDL's virtual joystick as a controller SDL has no mapping for (a handheld's
 * built-in one): recording buttons, a trigger, a hat and the sticks makes it a gamepad that reads
 * them, and the mapping is saved and comes back at the next start.
 *   clang -I host -I runtime/portable $(pkg-config --cflags sdl3) tests/host/padmap_test.c host/padmap.c \
 *     $(pkg-config --libs sdl3) -o build/padmap_test && build/padmap_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "padmap.h"

static int failed, held;
void input_hold_pads(int hold) { held = hold; }

static void expect_str(const char* what, const char* got, const char* want)
{
    if (strcmp(got, want))
        printf("%s: \"%s\", not \"%s\"\n", what, got, want), failed = 1;
}

static void expect(const char* what, int got, int want)
{
    if (got != want)
        printf("%s: %d, not %d\n", what, got, want), failed = 1;
}

static SDL_Joystick* g_dev; /* the virtual device's own handle, to move its controls */

static void pump(void) { SDL_UpdateJoysticks(); }

/* control c recorded while the device does what set() does to it, then let go with clear() */
static void record(int c, void (*set)(void), void (*clear)(void))
{
    padmap_listen(c);
    expect("held while recording", held, 1);
    pump();
    expect("nothing yet", padmap_poll(), 0);
    set();
    pump();
    expect("recorded", padmap_poll(), 1);
    clear();
    pump();
    padmap_poll(); /* let go: the game's pads read again */
    expect("settled", padmap_settling(), 0);
    expect("let go", held, 0);
}

static void press5(void) { SDL_SetJoystickVirtualButton(g_dev, 5, true); }
static void release5(void) { SDL_SetJoystickVirtualButton(g_dev, 5, false); }
static void trigger_down(void) { SDL_SetJoystickVirtualAxis(g_dev, 4, 32767); }
static void trigger_up(void) { SDL_SetJoystickVirtualAxis(g_dev, 4, -32768); }
static void hat_up(void) { SDL_SetJoystickVirtualHat(g_dev, 0, SDL_HAT_UP); }
static void hat_centre(void) { SDL_SetJoystickVirtualHat(g_dev, 0, SDL_HAT_CENTERED); }
static void stick_right(void) { SDL_SetJoystickVirtualAxis(g_dev, 2, 30000); }
static void stick_up(void) { SDL_SetJoystickVirtualAxis(g_dev, 3, -30000); } /* asked for down: reversed */
static void stick_centre(void) { SDL_SetJoystickVirtualAxis(g_dev, 2, 0), SDL_SetJoystickVirtualAxis(g_dev, 3, 0); }

/* only the virtual controller: a real one connected where the test runs would be padmap's first */
static void only_virtual(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
}

static SDL_JoystickID attach(void)
{
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_UNKNOWN; /* no mapping of SDL's */
    d.naxes = 6, d.nbuttons = 12, d.nhats = 1;
    d.vendor_id = 0x1234, d.product_id = 0x5678;
    d.name = "Handheld, Built-in";
    return SDL_AttachVirtualJoystick(&d);
}

int main(void)
{
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(path, sizeof path, "%s/gamepads.txt", dir);
    remove(path);
    only_virtual();
    if (!SDL_Init(SDL_INIT_GAMEPAD))
        return printf("SDL_Init: %s\n", SDL_GetError()), 1;
    padmap_init(dir);
    SDL_JoystickID id = attach();
    g_dev = SDL_OpenJoystick(id);
    SDL_SetJoystickVirtualAxis(g_dev, 4, -32768); /* a trigger at rest */
    pump();
    expect("not a gamepad to start", SDL_IsGamepad(id), 0);

    expect("opened", padmap_open(), 1);
    expect_str("its name", padmap_device(), "Handheld, Built-in");
    expect("no mapping: A none", padmap_get(0), 0);

    record(0, press5, release5); /* A */
    expect_str("A", padmap_value(0), "b5");
    expect_str("A short", padmap_short(0), "B5");
    expect_str("A's words", padmap_choice(0, padmap_get(0)), "Button 5");
    expect("a gamepad now", SDL_IsGamepad(id), 1);
    record(6, trigger_down, trigger_up); /* LT: all of its travel */
    expect_str("LT", padmap_value(6), "a4");
    record(12, hat_up, hat_centre); /* D-pad up */
    expect_str("D-pad up", padmap_value(12), "h0.1");
    record(18, stick_right, stick_centre); /* right stick across */
    expect_str("right stick across", padmap_value(18), "a2");
    record(19, stick_up, stick_centre); /* right stick up/down, pushed up: reversed */
    expect_str("right stick up/down", padmap_value(19), "a3~");
    expect_str("its short", padmap_short(19), "A3 rev");

    /* the button that chose the row, still held as recording starts, is not taken for it */
    SDL_SetJoystickVirtualButton(g_dev, 7, true);
    pump();
    padmap_listen(1);
    pump();
    expect("a held button is not a press", padmap_poll(), 0);
    SDL_SetJoystickVirtualButton(g_dev, 7, false);
    pump();
    expect("its release is not one", padmap_poll(), 0);
    SDL_SetJoystickVirtualButton(g_dev, 7, true);
    pump();
    expect("pressed again: taken", padmap_poll(), 1);
    expect_str("B", padmap_value(1), "b7");
    SDL_SetJoystickVirtualButton(g_dev, 7, false);
    pump();
    padmap_poll();

    /* cancelled (Escape): nothing changes, the game's pads read again */
    padmap_listen(16);
    expect("held while recording", held, 1);
    padmap_listen(-1);
    expect("not recording", padmap_listening(), -1);
    expect("let go on cancel", held, 0);
    SDL_SetJoystickVirtualAxis(g_dev, 0, 30000);
    pump();
    expect("nothing recorded after cancel", padmap_poll(), 0);
    expect_str("left stick across as it was", padmap_value(16), "");
    SDL_SetJoystickVirtualAxis(g_dev, 0, 0);
    pump();

    /* a button for the game's own map: the gamepad's (as mapped: b5 is A), applied by the caller */
    SDL_Gamepad* bp = SDL_OpenGamepad(id); /* as input.c has it open */
    SDL_SetJoystickVirtualButton(g_dev, 5, true); /* held as it starts (the select): not taken */
    pump();
    padmap_listen(PADMAP_BUTTONS);
    expect("held while listening for a button", held, 1);
    pump();
    expect("a held button is not the press", padmap_poll(), 0);
    SDL_SetJoystickVirtualButton(g_dev, 5, false);
    pump();
    padmap_poll();
    SDL_SetJoystickVirtualAxis(g_dev, 4, 32767); /* LT, down */
    pump();
    expect("button recorded", padmap_poll(), 1);
    expect("the left trigger", padmap_button(), SDL_GAMEPAD_BUTTON_COUNT);
    expect_str("nothing mapped for it", padmap_value(0), "b5");
    SDL_SetJoystickVirtualAxis(g_dev, 4, -32768);
    pump();
    padmap_poll();
    expect("let go after the button", held, 0);
    SDL_CloseGamepad(bp);

    /* picked from the list */
    padmap_set(16, 1); /* Axis 0 */
    expect_str("left stick across", padmap_value(16), "a0");

    /* an input already in use: taken from the control that had it, which is left with none */
    padmap_set(16, 3); /* Axis 2: the right stick's across */
    expect_str("left stick across took axis 2", padmap_value(16), "a2");
    expect_str("right stick across left with none", padmap_value(18), "");
    padmap_set(18, 3); /* back */
    expect_str("right stick across took it back", padmap_value(18), "a2");
    expect_str("left stick across left with none", padmap_value(16), "");
    padmap_set(16, 1);

    /* the gamepad reads what was recorded */
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    expect("opened as a gamepad", gp != NULL, 1);
    SDL_SetJoystickVirtualButton(g_dev, 5, true);
    SDL_SetJoystickVirtualAxis(g_dev, 3, -30000);
    pump();
    expect("A down", SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH), 1);
    expect("stick up reads down (reversed)", SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) > 20000, 1);
    expect("A live", padmap_live(0) == 1.0f, 1);
    expect("right stick live", padmap_live(19) > 0.6f, 1);
    SDL_SetJoystickVirtualButton(g_dev, 5, false);
    SDL_SetJoystickVirtualAxis(g_dev, 3, 0);
    SDL_CloseGamepad(gp);
    padmap_close();
    expect("not held once closed", held, 0);

    /* unplugged while recording: let go, the game's pads read again; plugged back in while the page is
     * open: taken again, with its mapping */
    expect("opened for the unplug", padmap_open(), 1);
    padmap_listen(1);
    expect("held while recording", held, 1);
    SDL_CloseJoystick(g_dev);
    SDL_DetachVirtualJoystick(id);
    pump();
    padmap_poll();
    expect("let go when unplugged", held, 0);
    expect("no controller", padmap_device() == NULL, 1);
    expect("not recording", padmap_listening(), -1);
    id = attach();
    g_dev = SDL_OpenJoystick(id);
    pump();
    SDL_Delay(1100); /* it looks again once a second */
    padmap_poll();
    expect("taken again when plugged in", padmap_device() != NULL, 1);
    expect_str("with its mapping", padmap_value(0), "b5");
    padmap_close();

    /* the next start: the saved mapping comes back */
    SDL_CloseJoystick(g_dev);
    SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    only_virtual(); /* SDL_Quit forgot the hints */
    SDL_Init(SDL_INIT_GAMEPAD);
    char again[600]; /* a new process's: the same folder (padmap_init reads a folder once a run) */
    snprintf(again, sizeof again, "%s/.", dir);
    padmap_init(again);
    id = attach();
    g_dev = SDL_OpenJoystick(id);
    pump();
    expect("a gamepad at the next start", SDL_IsGamepad(id), 1);
    expect("opened again", padmap_open(), 1);
    expect_str("A kept", padmap_value(0), "b5");
    expect_str("right stick kept", padmap_value(19), "a3~");
    padmap_close();
    SDL_CloseJoystick(g_dev);
    SDL_Quit();
    remove(path);
    if (!failed)
        printf("padmap_test: ok\n");
    return failed;
}
