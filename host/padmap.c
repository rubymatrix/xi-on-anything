/* Config > Controller's mappings. See padmap.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../runtime/portable/winapi/input.h"
#include "padmap.h"

enum
{
    MAX_AXES = 16,
    MAX_BUTTONS = 64,
    MAX_HATS = 4,
    MAX_EXTRA = 24,
    PUSHED = 16384,    /* an axis this far from where it was: pushed (half its travel) */
    AT_REST = 8192,    /* and back within this: let go */
    RELEASE_MS = 1500, /* after a recording, the next waits this long at most for the controller to be let go */
};

static const struct
{
    const char* key; /* SDL's name in a mapping */
    const char* label;
    const char* prompt;
} CTL[PADMAP_NCTL] = {
    { "a", "A", "Press the button for A (the bottom face button)." },
    { "b", "B", "Press the button for B (the right face button)." },
    { "x", "X", "Press the button for X (the left face button)." },
    { "y", "Y", "Press the button for Y (the top face button)." },
    { "leftshoulder", "LB", "Press the left shoulder button." },
    { "rightshoulder", "RB", "Press the right shoulder button." },
    { "lefttrigger", "LT", "Press the left trigger." },
    { "righttrigger", "RT", "Press the right trigger." },
    { "leftstick", "Left Stick Click", "Press the left stick in." },
    { "rightstick", "Right Stick Click", "Press the right stick in." },
    { "start", "Start", "Press Start." },
    { "back", "Back", "Press Back (Select)." },
    { "dpup", "D-Pad Up", "Press up on the D-pad." },
    { "dpdown", "D-Pad Down", "Press down on the D-pad." },
    { "dpleft", "D-Pad Left", "Press left on the D-pad." },
    { "dpright", "D-Pad Right", "Press right on the D-pad." },
    { "leftx", "Left Stick Across", "Push the left stick all the way right." },
    { "lefty", "Left Stick Up/Down", "Push the left stick all the way down." },
    { "rightx", "Right Stick Across", "Push the right stick all the way right." },
    { "righty", "Right Stick Up/Down", "Push the right stick all the way down." },
};
enum
{
    FIRST_AXIS_CTL = 16,
};

static char g_val[PADMAP_NCTL][16];     /* each control's value, as a mapping has it ("" none) */

/* ---- the choices ---- */

/* Buttons: None; Button 0-31; Axis 0-7 +, then -; Axis 0-7 (a trigger at rest at one end); the hat's
 * 4. Axes: None; Axis 0-7; the same reversed. */
static const char* const HATS[4] = { "Up", "Down", "Left", "Right" };
static const int HAT_BITS[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };

const char* padmap_choice(int axis, int i)
{
    static char s[24];
    if (i <= 0)
        return "None";
    if (axis)
    {
        if (i > 16)
            return "None";
        snprintf(s, sizeof s, i <= 8 ? "Axis %d" : "Axis %d Reversed", (i - 1) % 8);
        return s;
    }
    if (i <= 32)
        snprintf(s, sizeof s, "Button %d", i - 1);
    else if (i <= 48)
        snprintf(s, sizeof s, "Axis %d %c", (i - 33) % 8, i <= 40 ? '+' : '-');
    else if (i <= 56)
        snprintf(s, sizeof s, "Axis %d", i - 49);
    else if (i <= 60)
        snprintf(s, sizeof s, "Hat %s", HATS[i - 57]);
    else
        return "None";
    return s;
}

/* a choice's value in a mapping */
static void choice_value(int axis, int i, char out[16])
{
    out[0] = 0;
    if (i <= 0)
        return;
    if (axis)
        snprintf(out, 16, i <= 8 ? "a%d" : "a%d~", (i - 1) % 8);
    else if (i <= 32)
        snprintf(out, 16, "b%d", i - 1);
    else if (i <= 48)
        snprintf(out, 16, "%ca%d", i <= 40 ? '+' : '-', (i - 33) % 8);
    else if (i <= 56)
        snprintf(out, 16, "a%d", i - 49);
    else if (i <= 60)
        snprintf(out, 16, "h0.%d", HAT_BITS[i - 57]);
}

/* a mapping's value as a choice: 0 for what the list has no words for */
static int value_choice(int axis, const char* v)
{
    char s[16];
    for (int i = 1; i < (axis ? PADMAP_NAXIS_CHOICES : PADMAP_NBUTTON_CHOICES); ++i)
    {
        choice_value(axis, i, s);
        if (!strcmp(s, v))
            return i;
    }
    return 0;
}

const char* padmap_short(int c)
{
    static char s[16];
    const char* v = c >= 0 && c < PADMAP_NCTL ? g_val[c] : "";
    int n = 0, m = 0;
    if (!v[0])
        return "-";
    if (sscanf(v, "b%d", &n) == 1)
        snprintf(s, sizeof s, "B%d", n);
    else if (sscanf(v, "h%d.%d", &n, &m) == 2)
        snprintf(s, sizeof s, "Hat %s", m == SDL_HAT_UP ? "Up" : m == SDL_HAT_DOWN ? "Down" : m == SDL_HAT_LEFT ? "Left" : "Right");
    else if ((v[0] == '+' || v[0] == '-') && sscanf(v + 1, "a%d", &n) == 1)
        snprintf(s, sizeof s, "A%d%c", n, v[0]);
    else if (sscanf(v, "a%d", &n) == 1)
        snprintf(s, sizeof s, strchr(v, '~') ? "A%d rev" : "A%d", n);
    else
        SDL_strlcpy(s, v, sizeof s);
    return s;
}

const char* padmap_label(int c) { return c >= 0 && c < PADMAP_NCTL ? CTL[c].label : ""; }
int padmap_is_axis(int c) { return c >= FIRST_AXIS_CTL && c < PADMAP_NCTL; }
const char* padmap_prompt(int c) { return c >= 0 && c < PADMAP_NCTL ? CTL[c].prompt : ""; }
const char* padmap_value(int c) { return c >= 0 && c < PADMAP_NCTL ? g_val[c] : ""; }
int padmap_get(int c) { return c >= 0 && c < PADMAP_NCTL ? value_choice(padmap_is_axis(c), g_val[c]) : 0; }

/* The Xbox's, the web port's and the Switch's SDL (sdl_uwp.c, sdl_web.c, sdl_nx.c) has no joystick of
 * its own to map: no controller, nothing to record */
#if defined(FFXI_UWP) || defined(__SWITCH__) || defined(__EMSCRIPTEN__)
void padmap_init(const char* data_dir) { (void)data_dir; }
int padmap_open(void) { return 0; }
const char* padmap_device(void) { return NULL; }
void padmap_close(void) {}
void padmap_set(int c, int choice) { (void)c, (void)choice; }
void padmap_listen(int c) { (void)c; }
int padmap_poll(void) { return 0; }
int padmap_listening(void) { return -1; }
int padmap_settling(void) { return 0; }
int padmap_button(void) { return -1; }
float padmap_live(int c) { (void)c; return 0.0f; }
int padmap_mapping(char* out, int n) { (void)out, (void)n; return 0; }
#else
static char g_path[1100];
static SDL_Joystick* g_js;
static SDL_JoystickID g_id;
static char g_name[128], g_guid[40];
static char g_extra[MAX_EXTRA][48];     /* the mapping's other fields (guide, misc1, platform...), kept */
static int g_nextra;
static int g_listen = -1, g_releasing;
static Uint64 g_t0;
static Sint16 g_base[MAX_AXES];
static Uint8 g_seen_up[MAX_BUTTONS], g_hat_seen_centred[MAX_HATS];
static Uint8 g_pad_up[SDL_GAMEPAD_BUTTON_COUNT + 2]; /* PADMAP_BUTTONS: each seen up (the triggers last) */
static int g_pad_got = -1;
static int g_wanted;      /* the page is open: a controller connected later is picked up (padmap_poll) */
static Uint64 g_retry_at; /* when to look for one again */

/* ---- the mapping ---- */

/* the mapping SDL has for the controller now, into the controls' values and the rest */
static void read_mapping(void)
{
    memset(g_val, 0, sizeof g_val);
    g_nextra = 0;
    char* m = SDL_GetGamepadMappingForID(g_id);
    if (!m)
        return;
    int field = 0;
    for (char *s = m, *end; s && *s; s = end ? end + 1 : NULL, ++field)
    {
        end = strchr(s, ',');
        if (end)
            *end = 0;
        if (field == 1 && *s)
            SDL_strlcpy(g_name, s, sizeof g_name);
        if (field < 2 || !*s)
            continue;
        char* colon = strchr(s, ':');
        int known = 0;
        for (int c = 0; colon && c < PADMAP_NCTL; ++c)
            if ((size_t)(colon - s) == strlen(CTL[c].key) && !strncmp(s, CTL[c].key, (size_t)(colon - s)))
                SDL_strlcpy(g_val[c], colon + 1, sizeof g_val[c]), known = 1;
        if (!known && g_nextra < MAX_EXTRA)
            SDL_strlcpy(g_extra[g_nextra++], s, sizeof g_extra[0]);
    }
    SDL_free(m);
}

int padmap_mapping(char* out, int n)
{
    if (!g_js || n <= 0)
        return 0;
    char name[128];
    SDL_strlcpy(name, g_name[0] ? g_name : "Controller", sizeof name);
    for (char* p = name; *p; ++p)
        if (*p == ',')
            *p = ' ';
    int at = snprintf(out, (size_t)n, "%s,%s,", g_guid, name);
    for (int c = 0; c < PADMAP_NCTL && at < n; ++c)
        if (g_val[c][0])
            at += snprintf(out + at, (size_t)(n - at), "%s:%s,", CTL[c].key, g_val[c]);
    for (int i = 0; i < g_nextra && at < n; ++i)
        if (strncmp(g_extra[i], "platform:", 9))
            at += snprintf(out + at, (size_t)(n - at), "%s,", g_extra[i]);
    /* a file's lines are taken only for their platform (SDL_AddGamepadMappingsFromFile) */
    if (at < n)
        at += snprintf(out + at, (size_t)(n - at), "platform:%s,", SDL_GetPlatform());
    return at < n;
}

/* the gamepads file with this controller's line in place of the one it had */
static void save(const char* mapping)
{
    if (!g_path[0])
        return;
    char tmp[1200], line[1024];
    snprintf(tmp, sizeof tmp, "%s.new", g_path);
    FILE* in = fopen(g_path, "r");
    FILE* out = fopen(tmp, "w");
    if (!out)
    {
        if (in)
            fclose(in);
        fprintf(stderr, "[padmap] cannot write %s\n", tmp);
        return;
    }
    size_t glen = strlen(g_guid);
    while (in && fgets(line, sizeof line, in))
        if (strncmp(line, g_guid, glen) || line[glen] != ',')
            fputs(line, out);
    if (in)
        fclose(in);
    fprintf(out, "%s\n", mapping);
    if (fclose(out) || !SDL_RenamePath(tmp, g_path))
        fprintf(stderr, "[padmap] cannot replace %s\n", g_path);
}

/* the controls' values to SDL (the controller a gamepad from now on, if it was not) and the file */
static void apply(void)
{
    char m[1024];
    if (!padmap_mapping(m, sizeof m))
        return;
    if (SDL_AddGamepadMapping(m) < 0)
        fprintf(stderr, "[padmap] SDL took no mapping: %s\n", SDL_GetError());
    else
        fprintf(stderr, "[padmap] %s\n", m);
    save(m);
}

/* control c's value taken from any other control that had the same input (a stick's axis either
 * way round): that one is left with none */
static void take(int c)
{
    int n = 0, m = 0;
    int axis = padmap_is_axis(c) && sscanf(g_val[c], "a%d", &n) == 1;
    for (int k = 0; g_val[c][0] && k < PADMAP_NCTL; ++k)
        if (k != c && (!strcmp(g_val[k], g_val[c]) ||
                          (axis && padmap_is_axis(k) && sscanf(g_val[k], "a%d", &m) == 1 && m == n)))
            g_val[k][0] = 0;
}

void padmap_set(int c, int choice)
{
    if (c < 0 || c >= PADMAP_NCTL || !g_js)
        return;
    choice_value(padmap_is_axis(c), choice, g_val[c]);
    take(c);
    apply();
}

void padmap_init(const char* data_dir)
{
    if (!data_dir || !data_dir[0])
        return;
    size_t n = strlen(data_dir);
    char path[sizeof g_path];
    snprintf(path, sizeof path, "%s%sgamepads.txt", data_dir, n && (data_dir[n - 1] == '/' || data_dir[n - 1] == '\\') ? "" : "/");
    if (!strcmp(path, g_path)) /* the sign-in screen's already, the same folder */
        return;
    SDL_strlcpy(g_path, path, sizeof g_path);
    int added = SDL_AddGamepadMappingsFromFile(g_path);
    if (added > 0)
        fprintf(stderr, "[padmap] %d mapping(s) from %s\n", added, g_path);
}

/* ---- the controller ---- */

int padmap_open(void)
{
    g_wanted = 1;
    if (g_js)
        return 1;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n && !g_js; ++i)
    {
        const char* name = SDL_GetJoystickNameForID(ids[i]);
        if (!(SDL_IsJoystickVirtual(ids[i]) && name && !strcmp(name, "Touch controls"))) /* host/touch.c's */
            g_js = SDL_OpenJoystick(ids[i]), g_id = ids[i];
    }
    SDL_free(ids);
    if (!g_js)
        return 0;
    SDL_GUIDToString(SDL_GetJoystickGUID(g_js), g_guid, sizeof g_guid);
    const char* name = SDL_GetJoystickName(g_js);
    SDL_strlcpy(g_name, name ? name : "", sizeof g_name);
    read_mapping();
    return 1;
}

const char* padmap_device(void) { return g_js ? g_name : NULL; }

/* the controller let go of (closed, or gone): a recording stops, the game's pads read again */
static void drop(void)
{
    g_listen = -1, g_releasing = 0;
    input_hold_pads(0);
    if (g_js)
        SDL_CloseJoystick(g_js);
    g_js = NULL, g_id = 0;
}

void padmap_close(void)
{
    g_wanted = 0;
    drop();
}

int padmap_listening(void) { return g_listen; }
int padmap_settling(void) { return g_releasing; }
int padmap_button(void) { return g_pad_got; }

float padmap_live(int c)
{
    SDL_Gamepad* p = g_js ? SDL_GetGamepadFromID(g_id) : NULL;
    if (!p || c < 0 || c >= PADMAP_NCTL)
        return 0.0f;
    static const int BUTTON[16] = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, -1,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
    static const int AXIS[PADMAP_NCTL] = { [6] = SDL_GAMEPAD_AXIS_LEFT_TRIGGER, [7] = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
        [16] = SDL_GAMEPAD_AXIS_LEFTX, [17] = SDL_GAMEPAD_AXIS_LEFTY, [18] = SDL_GAMEPAD_AXIS_RIGHTX,
        [19] = SDL_GAMEPAD_AXIS_RIGHTY };
    if (c < 16 && BUTTON[c] >= 0)
        return SDL_GetGamepadButton(p, (SDL_GamepadButton)BUTTON[c]) ? 1.0f : 0.0f;
    return (float)SDL_GetGamepadAxis(p, (SDL_GamepadAxis)AXIS[c]) / 32767.0f;
}

void padmap_listen(int c)
{
    if (c < 0 && g_listen >= 0) /* stop recording */
        g_listen = -1, input_hold_pads(0);
    if (c < 0 || (c >= PADMAP_NCTL && c != PADMAP_BUTTONS) || !g_js)
        return;
    SDL_UpdateJoysticks();
    g_releasing = 0, g_pad_got = -1;
    SDL_Gamepad* gp = SDL_GetGamepadFromID(g_id);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        g_pad_up[b] = !gp || !SDL_GetGamepadButton(gp, (SDL_GamepadButton)b);
    g_pad_up[SDL_GAMEPAD_BUTTON_COUNT] = !gp || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) < PUSHED;
    g_pad_up[SDL_GAMEPAD_BUTTON_COUNT + 1] = !gp || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) < PUSHED;
    int na = SDL_GetNumJoystickAxes(g_js), nb = SDL_GetNumJoystickButtons(g_js), nh = SDL_GetNumJoystickHats(g_js);
    for (int a = 0; a < na && a < MAX_AXES; ++a)
        g_base[a] = SDL_GetJoystickAxis(g_js, a);
    /* what is held as it starts (the button that chose this row) counts once it has been let go */
    for (int b = 0; b < nb && b < MAX_BUTTONS; ++b)
        g_seen_up[b] = !SDL_GetJoystickButton(g_js, b);
    for (int h = 0; h < nh && h < MAX_HATS; ++h)
        g_hat_seen_centred[h] = SDL_GetJoystickHat(g_js, h) == SDL_HAT_CENTERED;
    g_listen = c, g_releasing = 0, g_t0 = SDL_GetTicks();
    input_hold_pads(1); /* the game's pads at rest: what is pressed to record does nothing in the game */
}

/* everything back where it was: buttons up, hats centred, axes near where they were */
static int let_go(void)
{
    int na = SDL_GetNumJoystickAxes(g_js), nb = SDL_GetNumJoystickButtons(g_js), nh = SDL_GetNumJoystickHats(g_js);
    for (int b = 0; b < nb; ++b)
        if (SDL_GetJoystickButton(g_js, b))
            return 0;
    for (int h = 0; h < nh; ++h)
        if (SDL_GetJoystickHat(g_js, h) != SDL_HAT_CENTERED)
            return 0;
    for (int a = 0; a < na && a < MAX_AXES; ++a)
        if (abs(SDL_GetJoystickAxis(g_js, a) - g_base[a]) > AT_REST)
            return 0;
    return 1;
}

/* what was pressed for control c into its value: 1 if anything was */
static int record(int c)
{
    if (c == PADMAP_BUTTONS) /* the next button the gamepad (as SDL maps it) has pressed */
    {
        SDL_Gamepad* gp = SDL_GetGamepadFromID(g_id);
        for (int b = 0; gp && b < SDL_GAMEPAD_BUTTON_COUNT + 2; ++b)
        {
            int down = b < SDL_GAMEPAD_BUTTON_COUNT ? SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)
                : SDL_GetGamepadAxis(gp, b == SDL_GAMEPAD_BUTTON_COUNT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= PUSHED;
            if (!down)
                g_pad_up[b] = 1;
            else if (g_pad_up[b])
                return g_pad_got = b, 1;
        }
        return 0;
    }
    int na = SDL_GetNumJoystickAxes(g_js), nb = SDL_GetNumJoystickButtons(g_js), nh = SDL_GetNumJoystickHats(g_js);
    char* v = g_val[c];
    if (padmap_is_axis(c)) /* the axis pushed furthest; the wrong way round reversed */
    {
        int best = -1, delta = 0;
        for (int a = 0; a < na && a < MAX_AXES; ++a)
        {
            int d = SDL_GetJoystickAxis(g_js, a) - g_base[a];
            if (abs(d) > PUSHED && abs(d) > abs(delta))
                best = a, delta = d;
        }
        if (best < 0 || best > 7)
            return 0;
        snprintf(v, 16, delta > 0 ? "a%d" : "a%d~", best);
        return 1;
    }
    for (int b = 0; b < nb && b < MAX_BUTTONS; ++b)
    {
        int down = SDL_GetJoystickButton(g_js, b);
        if (!down)
            g_seen_up[b] = 1;
        else if (g_seen_up[b])
            return snprintf(v, 16, "b%d", b), 1;
    }
    for (int h = 0; h < nh && h < MAX_HATS; ++h)
    {
        Uint8 hat = SDL_GetJoystickHat(g_js, h);
        if (hat == SDL_HAT_CENTERED)
            g_hat_seen_centred[h] = 1;
        else if (g_hat_seen_centred[h])
            for (int k = 0; k < 4; ++k)
                if (hat & HAT_BITS[k])
                    return snprintf(v, 16, "h%d.%d", h, HAT_BITS[k]), 1;
    }
    for (int a = 0; a < na && a < MAX_AXES; ++a)
    {
        int now = SDL_GetJoystickAxis(g_js, a), d = now - g_base[a];
        if (abs(d) <= PUSHED)
            continue;
        if (g_base[a] < -PUSHED && d > 0) /* a trigger, at rest at one end: all of its travel */
            snprintf(v, 16, "a%d", a);
        else
            snprintf(v, 16, "%ca%d", d > 0 ? '+' : '-', a);
        return 1;
    }
    return 0;
}

int padmap_poll(void)
{
    /* unplugged: let go of it (its handle is dead), and while the page is open, take the next
     * controller connected (this one again, plugged back in) */
    if (g_js && !SDL_JoystickConnected(g_js))
    {
        fprintf(stderr, "[padmap] %s disconnected\n", g_name);
        drop();
    }
    if (!g_js)
    {
        if (g_wanted && SDL_GetTicks() >= g_retry_at)
        {
            g_retry_at = SDL_GetTicks() + 1000;
            if (padmap_open())
                fprintf(stderr, "[padmap] %s connected\n", g_name);
        }
        return 0;
    }
    if (g_listen < 0 && !g_releasing)
        return 0;
    SDL_UpdateJoysticks();
    Uint64 now = SDL_GetTicks();
    if (g_releasing)
    {
        if (let_go() || now - g_t0 > RELEASE_MS)
            g_releasing = 0, input_hold_pads(0);
        return 0;
    }
    int c = g_listen;
    if (record(c))
    {
        g_listen = -1, g_releasing = 1, g_t0 = now;
        if (c != PADMAP_BUTTONS) /* a button for the game's own map: the caller's to set */
            take(c), apply();
        return 1;
    }
    return 0;
}
#endif
