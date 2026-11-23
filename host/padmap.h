/* A controller's own buttons, axes and hats as the game reads them, as an
 * SDL gamepad mapping (SDL_AddGamepadMapping) for it. For a controller SDL maps wrong or not at all (a
 * handheld's built-in one): a control is recorded by pressing it (padmap_listen) or picked from a
 * list. The mappings are kept in <data dir>/gamepads.txt, SDL's gamecontrollerdb format, a line a
 * controller (its GUID), and added again at the next start (padmap_init).
 *
 * The controller is the first connected one that is not the touch controls' pad (host/touch.c). Called
 * from the game's thread, which pumps SDL's events. */
#pragma once

enum
{
    PADMAP_NCTL = 20, /* A B X Y LB RB LT RT L3 R3 Start Back, the D-pad's 4, the sticks' 4 axes */
    PADMAP_NBUTTON_CHOICES = 61,
    PADMAP_NAXIS_CHOICES = 17,
    PADMAP_BUTTONS = 100, /* padmap_listen's: the next button the gamepad presses (padmap_button) */
};

/* The saved mappings, added to SDL's. */
void padmap_init(const char* data_dir);

/* Control c: its name, whether it is a stick's axis (the axis choices) or a button (the button
 * choices), and the words for choice i of its kind ("Button 3", "Axis 2 +", "Hat Up"). */
const char* padmap_label(int c);
int padmap_is_axis(int c);
const char* padmap_choice(int axis, int i);

/* The controller, opened for the page (0 if there is none) and its name; closed when the page is. */
int padmap_open(void);
const char* padmap_device(void);
void padmap_close(void);

/* Control c's choice now (0 None, also for what the list has no words for), and a new one: the
 * mapping applied and saved at once. */
int padmap_get(int c);
void padmap_set(int c, int choice);

/* Recording control c: the next button pressed, hat moved or axis pushed past half way (for a stick,
 * the axis pushed furthest: right or down is its positive way) becomes it, however long that takes;
 * padmap_listen(-1) stops. padmap_poll each frame: 1 when one was recorded (applied and saved).
 * padmap_listening: the
 * control being recorded, or -1; padmap_settling: one was just recorded and the controller is not let
 * go yet. While recording and until the controller is let go, the game's pads read as at rest
 * (input_hold_pads), so the press does nothing in the game. */
void padmap_listen(int c); /* -1: stop recording (cancelled: nothing changes) */
int padmap_poll(void);
int padmap_listening(void);
int padmap_settling(void);
/* After padmap_listen(PADMAP_BUTTONS) recorded: the SDL_GamepadButton pressed, SDL_GAMEPAD_BUTTON_COUNT the
 * left trigger, + 1 the right; -1 none. Nothing is applied: it is the game's own map's (Config > Gamepad). */
int padmap_button(void);
/* Control c as the game reads it now, through the mapping: a button 0 or 1, a trigger 0 to 1, a stick
 * axis -1 to 1 (right and down positive). */
float padmap_live(int c);
/* Control c's value in a few letters ("B5", "A2 rev", "A4+", "Hat Up", "-" none). */
const char* padmap_short(int c);
/* What to ask for while control c is recorded ("Press the button for A.") */
const char* padmap_prompt(int c);

/* For tests/host/padmap_test.c: control c's value in the mapping ("b3", "a1~"), and the mapping as it
 * would be given to SDL. */
const char* padmap_value(int c);
int padmap_mapping(char* out, int n);
