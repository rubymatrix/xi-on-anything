/* The SDL3 calls the runtime and the sign-in screen make (user32.c, input.c, dsound.c, host/signin.c and
 * what it uses), for the Nintendo Switch build, where SDL3 has no back end: the port of sdl_web.c (itself
 * sdl_uwp.c's, the Xbox's) to libnx. Compiled against SDL3's headers for its types, linked instead of SDL3.
 *
 * There is one window, the screen: 1280x720 handheld, 1920x1080 docked (nx_bridge.h's nx_set_view_size,
 * from host/switch_main.c). Input is read from HID on the game's thread, in SDL_PollEvent: the first
 * controller (or the handheld Joy-Con) is the SDL gamepad, a tap on the touch screen a click. Text comes from
 * the system keyboard (swkbd), shown over a sign-in field with the field's text and handed back whole, as
 * the Xbox's keyboard does. Audio is a thread that makes a 10 ms block at a time for audout: dsound.c's
 * callback (which moves the DirectSound cursors and signals the game's notification events) and the
 * streams bound to the device (the sign-in screen's effects, sewave.c) mixed together. */
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#include <SDL3/SDL.h>

#include "nx_bridge.h"
#include "plat.h"
#include "runtime.h"
#include "user32.h"

typedef pthread_mutex_t Lock;
#define LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)

#define APP_DIR "sdmc:/switch/xi-on-anything"

/* --- mutexes ---------------------------------------------------------------------------------------- */
struct SDL_Mutex
{
    pthread_mutex_t m; /* recursive, as SDL's */
};

SDL_Mutex* SDL_CreateMutex(void)
{
    SDL_Mutex* m = (SDL_Mutex*)calloc(1, sizeof *m);
    if (m)
    {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&m->m, &a);
        pthread_mutexattr_destroy(&a);
    }
    return m;
}
void SDL_LockMutex(SDL_Mutex* m)
{
    if (m)
        pthread_mutex_lock(&m->m);
}
void SDL_UnlockMutex(SDL_Mutex* m)
{
    if (m)
        pthread_mutex_unlock(&m->m);
}
void SDL_DestroyMutex(SDL_Mutex* m)
{
    if (m)
    {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}

/* --- odds and ends ------------------------------------------------------------------------------- */
const char* SDL_GetError(void) { return "not available in the Switch build"; }
void SDL_free(void* p) { free(p); }
void* SDL_malloc(size_t n) { return malloc(n); }
int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}
bool SDL_SetHint(const char* name, const char* value) { (void)name, (void)value; return true; }
const char* SDL_GetHint(const char* name) { (void)name; return NULL; }
bool SDL_GetHintBoolean(const char* name, bool default_value) { (void)name; return default_value; }
bool SDL_Init(SDL_InitFlags flags) { (void)flags; return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return true; }
void SDL_QuitSubSystem(SDL_InitFlags flags) { (void)flags; }
void SDL_Quit(void) {}
/* nothing is reported initialized: callers that check (modern.c's display modes, the addons' sounds) take
 * their paths for no SDL */
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { (void)flags; return 0; }

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
Uint64 SDL_GetTicks(void) { return now_ns() / 1000000u; }
Uint64 SDL_GetTicksNS(void) { return now_ns(); }
void SDL_Delay(Uint32 ms) { svcSleepThread((int64_t)ms * 1000000); }

static char* g_clipboard;
bool SDL_SetClipboardText(const char* text)
{
    free(g_clipboard);
    g_clipboard = text ? strdup(text) : NULL;
    return true;
}
bool SDL_HasClipboardText(void) { return g_clipboard && *g_clipboard; }
char* SDL_GetClipboardText(void) { return strdup(g_clipboard ? g_clipboard : ""); } /* freed with SDL_free */

/* --- the event queue ------------------------------------------------------------------------------ */
#define QUEUE 1024
static Lock g_qlock = LOCK_INIT;
static SDL_Event g_queue[QUEUE];
static unsigned g_qhead, g_qtail;
static char g_text[QUEUE][32]; /* SDL_EVENT_TEXT_INPUT points at these */

static int push(const SDL_Event* e)
{
    lock(&g_qlock);
    unsigned next = (g_qtail + 1) % QUEUE;
    int ok = next != g_qhead;
    if (ok)
    {
        g_queue[g_qtail] = *e;
        if (e->type == SDL_EVENT_TEXT_INPUT)
        {
            snprintf(g_text[g_qtail], sizeof g_text[0], "%s", e->text.text);
            g_queue[g_qtail].text.text = g_text[g_qtail];
        }
        g_qtail = next;
    }
    unlock(&g_qlock);
    return ok;
}

static void poll_input(void);

bool SDL_PollEvent(SDL_Event* e)
{
    poll_input();
    lock(&g_qlock);
    int any = g_qhead != g_qtail;
    if (any)
    {
        *e = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE;
    }
    unlock(&g_qlock);
    return any;
}
void SDL_PumpEvents(void) { poll_input(); }
bool SDL_PushEvent(SDL_Event* e) { return push(e); }

/* --- the window ------------------------------------------------------------------------------------ */
#define WINDOW_ID 1
struct SDL_Window
{
    int w, h;
    SDL_WindowFlags flags; /* hidden or not, as the game asked */
};
static SDL_Window g_window;
static volatile int g_view_w = 1280, g_view_h = 720;
static int g_have_window;

void nx_set_view_size(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int was_w = g_view_w, was_h = g_view_h;
    __atomic_store_n(&g_view_w, w, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_view_h, h, __ATOMIC_SEQ_CST);
    /* the game's client (1920x1080, the registry's) is presented over the whole screen: a touch, in the
     * screen's units, is mapped to the client's pixels through it (else 1280x720 handheld touches landed at
     * two thirds of the way, and the screen's foot was out of reach) */
    user32_set_view(0, 0, (float)w, (float)h);
    if (g_have_window && (w != was_w || h != was_h))
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
        e.window.windowID = WINDOW_ID;
        e.window.data1 = w, e.window.data2 = h;
        push(&e);
    }
}

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags)
{
    (void)title;
    g_window.w = w, g_window.h = h, g_window.flags = flags & SDL_WINDOW_HIDDEN;
    g_have_window = 1;
    return &g_window;
}
void SDL_DestroyWindow(SDL_Window* w) { (void)w; g_have_window = 0; }
SDL_WindowID SDL_GetWindowID(SDL_Window* w) { return w ? WINDOW_ID : 0; }
SDL_Window* SDL_GetWindowFromID(SDL_WindowID id) { return id == WINDOW_ID && g_have_window ? &g_window : NULL; }
bool SDL_SetWindowPosition(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; return true; }
bool SDL_GetWindowPosition(SDL_Window* w, int* x, int* y)
{
    (void)w;
    if (x)
        *x = 0;
    if (y)
        *y = 0;
    return true;
}
bool SDL_SetWindowSize(SDL_Window* w, int width, int height)
{
    if (w)
        w->w = width, w->h = height;
    return true;
}
bool SDL_GetWindowSizeInPixels(SDL_Window* w, int* width, int* height)
{
    (void)w;
    if (width)
        *width = (int)g_view_w;
    if (height)
        *height = (int)g_view_h;
    return true;
}
bool SDL_GetWindowSize(SDL_Window* w, int* width, int* height) { return SDL_GetWindowSizeInPixels(w, width, height); }
bool SDL_GetWindowSafeArea(SDL_Window* w, SDL_Rect* r)
{
    if (r)
    {
        r->x = r->y = 0;
        SDL_GetWindowSizeInPixels(w, &r->w, &r->h);
    }
    return true;
}
bool SDL_SetTextInputArea(SDL_Window* w, const SDL_Rect* r, int cursor) { (void)w, (void)r, (void)cursor; return true; }
SDL_Window* SDL_GetWindowFromEvent(const SDL_Event* e) { (void)e; return g_have_window ? &g_window : NULL; }
SDL_Window* SDL_GetKeyboardFocus(void) { return g_have_window ? &g_window : NULL; }
SDL_Window* SDL_GetMouseFocus(void) { return g_have_window ? &g_window : NULL; }
bool SDL_ShowWindow(SDL_Window* w)
{
    if (w)
        w->flags &= ~SDL_WINDOW_HIDDEN;
    return true;
}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* w) { return w ? w->flags | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS : 0; }
bool SDL_SetWindowTitle(SDL_Window* w, const char* title) { (void)w, (void)title; return true; }
bool SDL_HideWindow(SDL_Window* w)
{
    if (w)
        w->flags |= SDL_WINDOW_HIDDEN;
    return true;
}
bool SDL_SetWindowFullscreen(SDL_Window* w, bool on) { (void)w, (void)on; return true; } /* the screen is the window */
bool SDL_SyncWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_RaiseWindow(SDL_Window* w) { (void)w; return true; }
float SDL_GetWindowPixelDensity(SDL_Window* w) { (void)w; return 1.0f; }
float SDL_GetWindowDisplayScale(SDL_Window* w) { (void)w; return 1.0f; }
bool SDL_SetWindowFullscreenMode(SDL_Window* w, const SDL_DisplayMode* m) { (void)w, (void)m; return true; }
bool SDL_SetWindowBordered(SDL_Window* w, bool on) { (void)w, (void)on; return true; }
bool SDL_SetWindowResizable(SDL_Window* w, bool on) { (void)w, (void)on; return true; }
bool SDL_SetWindowMinimumSize(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; return true; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* w) { (void)w; return 1; }
SDL_Window** SDL_GetWindows(int* count)
{
    SDL_Window** w = (SDL_Window**)calloc(2, sizeof *w);
    int n = g_have_window && w ? 1 : 0;
    if (n)
        w[0] = &g_window;
    if (count)
        *count = n;
    return w;
}

/* --- properties (the text input's, the window's: what the callers here pass and read) ----------------- */
#define PROPS 8
typedef struct Props
{
    int used;
    Sint64 type; /* SDL_PROP_TEXTINPUT_TYPE_NUMBER */
    int has_text;   /* a sign-in field's: its whole text comes back (else it is typed) */
    char text[256]; /* "xi.textinput.text" */
} Props;
static Props g_props[PROPS];
static Lock g_prlock = LOCK_INIT;

SDL_PropertiesID SDL_CreateProperties(void)
{
    lock(&g_prlock);
    SDL_PropertiesID id = 0;
    for (int i = 0; i < PROPS && !id; ++i)
        if (!g_props[i].used)
        {
            memset(&g_props[i], 0, sizeof g_props[i]);
            g_props[i].used = 1;
            g_props[i].type = SDL_TEXTINPUT_TYPE_TEXT;
            id = (SDL_PropertiesID)(i + 1);
        }
    unlock(&g_prlock);
    return id;
}
static Props* props(SDL_PropertiesID id) { return id >= 1 && id <= PROPS && g_props[id - 1].used ? &g_props[id - 1] : NULL; }
void SDL_DestroyProperties(SDL_PropertiesID id)
{
    Props* p = props(id);
    if (p)
        p->used = 0;
}
bool SDL_SetNumberProperty(SDL_PropertiesID id, const char* name, Sint64 value)
{
    Props* p = props(id);
    if (p && !strcmp(name, SDL_PROP_TEXTINPUT_TYPE_NUMBER))
        p->type = value;
    return true;
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID id, const char* name, Sint64 default_value)
{
    Props* p = props(id);
    return p && !strcmp(name, SDL_PROP_TEXTINPUT_TYPE_NUMBER) ? p->type : default_value;
}
bool SDL_SetStringProperty(SDL_PropertiesID id, const char* name, const char* value)
{
    Props* p = props(id);
    if (p && !strcmp(name, "xi.textinput.text"))
    {
        snprintf(p->text, sizeof p->text, "%s", value ? value : "");
        p->has_text = 1;
    }
    return true;
}
const char* SDL_GetStringProperty(SDL_PropertiesID id, const char* name, const char* default_value)
{
    Props* p = props(id);
    return p && !strcmp(name, "xi.textinput.text") ? p->text : default_value;
}
bool SDL_SetBooleanProperty(SDL_PropertiesID id, const char* name, bool value) { (void)id, (void)name, (void)value; return true; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* w) { (void)w; return 0; }
void* SDL_GetPointerProperty(SDL_PropertiesID id, const char* name, void* default_value)
{
    (void)id, (void)name;
    return default_value;
}

/* --- text input: the system keyboard ----------------------------------------------------------------- */
/* The sign-in screen's SDL_EVENT_USER (host/signin.h's SIGNIN_KEYBOARD_TEXT): the field's whole text,
 * malloc'd for it to free, as the Xbox's keyboard hands it back. */
#define SIGNIN_KEYBOARD_TEXT 0x5849

static void replace_text(const char* utf8)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_USER;
    e.user.windowID = WINDOW_ID;
    e.user.code = SIGNIN_KEYBOARD_TEXT;
    e.user.data1 = strdup(utf8);
    if (e.user.data1 && !push(&e))
        free(e.user.data1);
}

static void key(SDL_Scancode sc, int down);

/* swkbd is a library applet: it takes the screen and blocks this thread (the game's) until the player is done */
static int keyboard(Sint64 type, const char* initial, char* out, size_t outn)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return 0;
    swkbdConfigMakePresetDefault(&kbd);
    if (type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN || type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE)
        swkbdConfigMakePresetPassword(&kbd);
    else if (type == SDL_TEXTINPUT_TYPE_NUMBER)
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetInitialText(&kbd, initial ? initial : "");
    Result rc = swkbdShow(&kbd, out, outn);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

bool SDL_StartTextInput(SDL_Window* w) { (void)w; return true; } /* text arrives only with a field (below) */

/* host/signin.c asks for the keyboard over a field, with its text so far; the game's chat line (host64.c)
 * asks with none: its text is typed into the line and Enter sends it */
bool SDL_StartTextInputWithProperties(SDL_Window* w, SDL_PropertiesID id)
{
    (void)w;
    Props* p = props(id);
    char text[512] = "";
    if (!keyboard(p ? p->type : SDL_TEXTINPUT_TYPE_TEXT, p ? p->text : "", text, sizeof text))
        return true;
    if (p && p->has_text)
    {
        replace_text(text);
        return true;
    }
    /* typed, a piece at a time (an SDL text event carries at most 31 bytes; pieces end on whole UTF-8
     * characters), then Enter */
    for (const char* s = text; *s;)
    {
        size_t n = strlen(s) < 31 ? strlen(s) : 31;
        while (n && (s[n] & 0xC0) == 0x80)
            --n;
        char piece[32];
        memcpy(piece, s, n);
        piece[n] = 0;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = WINDOW_ID;
        e.text.text = piece; /* copied by push */
        push(&e);
        s += n ? n : 1;
    }
    if (text[0])
    {
        key(SDL_SCANCODE_RETURN, 1);
        key(SDL_SCANCODE_RETURN, 0);
    }
    return true;
}
bool SDL_StopTextInput(SDL_Window* w) { (void)w; return true; }
bool SDL_TextInputActive(SDL_Window* w) { (void)w; return false; }
bool SDL_ScreenKeyboardShown(SDL_Window* w) { (void)w; return false; }
bool SDL_HasScreenKeyboardSupport(void) { return true; }

SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID id)
{
    static SDL_DisplayMode m;
    (void)id;
    m.displayID = 1;
    m.format = SDL_PIXELFORMAT_XRGB8888;
    m.w = (int)g_view_w, m.h = (int)g_view_h;
    m.pixel_density = 1.0f;
    m.refresh_rate = 60.0f;
    return &m;
}
const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID id) { return SDL_GetDesktopDisplayMode(id); }
/* the one mode: the screen (Config's resolutions list it, and the sizes the game asks for are drawn scaled to it) */
SDL_DisplayMode** SDL_GetFullscreenDisplayModes(SDL_DisplayID id, int* count)
{
    SDL_DisplayMode** list = (SDL_DisplayMode**)calloc(1, 2 * sizeof *list + sizeof(SDL_DisplayMode));
    if (!list)
    {
        if (count)
            *count = 0;
        return NULL;
    }
    SDL_DisplayMode* m = (SDL_DisplayMode*)(list + 2);
    *m = *SDL_GetDesktopDisplayMode(id);
    list[0] = m;
    if (count)
        *count = 1;
    return list;
}
SDL_DisplayID* SDL_GetDisplays(int* count)
{
    SDL_DisplayID* d = (SDL_DisplayID*)calloc(2, sizeof *d);
    if (d)
        d[0] = 1;
    if (count)
        *count = d ? 1 : 0;
    return d;
}
bool SDL_GetDisplayBounds(SDL_DisplayID id, SDL_Rect* r)
{
    (void)id;
    if (r)
        r->x = r->y = 0, r->w = g_view_w, r->h = g_view_h;
    return true;
}
bool SDL_GetDisplayUsableBounds(SDL_DisplayID id, SDL_Rect* r) { return SDL_GetDisplayBounds(id, r); }

/* --- keyboard (a USB keyboard, through HID) and mouse (the touch screen) ------------------------------ */
static Lock g_mlock = LOCK_INIT;
static float g_mx, g_my;
static SDL_MouseButtonFlags g_mbuttons;
static bool g_keystate[SDL_SCANCODE_COUNT];

const bool* SDL_GetKeyboardState(int* numkeys)
{
    if (numkeys)
        *numkeys = SDL_SCANCODE_COUNT;
    return g_keystate;
}
bool SDL_OpenURL(const char* url) { (void)url; return false; }

static void key(SDL_Scancode sc, int down)
{
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT || g_keystate[sc] == (down != 0))
        return;
    g_keystate[sc] = down != 0;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = WINDOW_ID;
    e.key.scancode = sc;
    e.key.down = down != 0;
    push(&e);
}

SDL_Keymod SDL_GetModState(void)
{
    SDL_Keymod m = 0;
    if (g_keystate[SDL_SCANCODE_LCTRL] || g_keystate[SDL_SCANCODE_RCTRL])
        m |= SDL_KMOD_CTRL;
    if (g_keystate[SDL_SCANCODE_LSHIFT] || g_keystate[SDL_SCANCODE_RSHIFT])
        m |= SDL_KMOD_SHIFT;
    if (g_keystate[SDL_SCANCODE_LALT] || g_keystate[SDL_SCANCODE_RALT])
        m |= SDL_KMOD_ALT;
    return m;
}

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y)
{
    lock(&g_mlock);
    if (x)
        *x = g_mx;
    if (y)
        *y = g_my;
    SDL_MouseButtonFlags b = g_mbuttons;
    unlock(&g_mlock);
    return b;
}
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) { return SDL_GetMouseState(x, y); }
SDL_MouseButtonFlags SDL_GetRelativeMouseState(float* x, float* y)
{
    if (x)
        *x = 0;
    if (y)
        *y = 0;
    return 0;
}
bool SDL_SetWindowRelativeMouseMode(SDL_Window* w, bool on) { (void)w, (void)on; return true; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }
bool SDL_CursorVisible(void) { return false; }

static void mouse_move(float x, float y)
{
    lock(&g_mlock);
    g_mx = x, g_my = y;
    SDL_MouseButtonFlags b = g_mbuttons;
    unlock(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = WINDOW_ID;
    e.motion.state = b;
    e.motion.x = x, e.motion.y = y;
    push(&e);
}

static void mouse_button(int button, int down)
{
    lock(&g_mlock);
    if (down)
        g_mbuttons |= SDL_BUTTON_MASK(button);
    else
        g_mbuttons &= ~SDL_BUTTON_MASK(button);
    float x = g_mx, y = g_my;
    unlock(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = WINDOW_ID;
    e.button.button = (Uint8)button;
    e.button.down = down != 0;
    e.button.clicks = 1;
    e.button.x = x, e.button.y = y;
    push(&e);
}

/* The game putting the pointer somewhere (SetCursorPos: its menus put it on the item the D-pad selects): it
 * moves there, as a desktop's does, and says so. As a stub the game read it back where the last touch left
 * it, over another item, and the selection went back there: the D-pad couldn't move it. */
bool SDL_WarpMouseGlobal(float x, float y)
{
    mouse_move(x, y);
    return true;
}
void SDL_WarpMouseInWindow(SDL_Window* w, float x, float y)
{
    (void)w;
    mouse_move(x, y);
}

/* --- the controller ------------------------------------------------------------------------------------ */
#define PAD_ID 1
struct SDL_Gamepad
{
    int open;
};
static SDL_Gamepad g_pad;
static Lock g_plock = LOCK_INIT;
typedef struct Pad
{
    int connected;
    uint32_t buttons; /* bit n: SDL_GamepadButton n */
    int16_t axes[6];
} Pad;
static Pad g_padstate;
static PadState g_nxpad;
static int g_input_ready, g_layout_xbox;

void nx_set_layout(int xbox) { g_layout_xbox = xbox; }

static int closing; /* the system asked the game to close (appletMainLoop, below) */
int nx_exit_requested(void) { return closing; }

/* The game's confirm is SDL's south button (Xbox A, the bottom one). Nintendo's layout: A, the right
 * one, confirms and B cancels, as everywhere on the Switch. Xbox's: by position, B (bottom) confirms. */
static uint32_t map_buttons(uint64_t b)
{
    uint32_t out = 0;
#define MAP(nx, sdl) \
    if (b & (nx)) \
        out |= 1u << (sdl);
    if (g_layout_xbox)
    {
        MAP(HidNpadButton_B, SDL_GAMEPAD_BUTTON_SOUTH)
        MAP(HidNpadButton_A, SDL_GAMEPAD_BUTTON_EAST)
        MAP(HidNpadButton_Y, SDL_GAMEPAD_BUTTON_WEST)
        MAP(HidNpadButton_X, SDL_GAMEPAD_BUTTON_NORTH)
    }
    else
    {
        MAP(HidNpadButton_A, SDL_GAMEPAD_BUTTON_SOUTH)
        MAP(HidNpadButton_B, SDL_GAMEPAD_BUTTON_EAST)
        MAP(HidNpadButton_X, SDL_GAMEPAD_BUTTON_NORTH)
        MAP(HidNpadButton_Y, SDL_GAMEPAD_BUTTON_WEST)
    }
    MAP(HidNpadButton_Minus, SDL_GAMEPAD_BUTTON_BACK)
    MAP(HidNpadButton_Plus, SDL_GAMEPAD_BUTTON_START)
    MAP(HidNpadButton_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK)
    MAP(HidNpadButton_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK)
    MAP(HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
    MAP(HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
    MAP(HidNpadButton_Up, SDL_GAMEPAD_BUTTON_DPAD_UP)
    MAP(HidNpadButton_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN)
    MAP(HidNpadButton_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT)
    MAP(HidNpadButton_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
#undef MAP
    return out;
}

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void gamepad_event(Uint32 type, int button, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || type == SDL_EVENT_GAMEPAD_BUTTON_UP)
    {
        e.gbutton.which = PAD_ID;
        e.gbutton.button = (Uint8)button;
        e.gbutton.down = down != 0;
    }
    else
        e.gdevice.which = PAD_ID;
    push(&e);
}

/* The touch screen as Android's (host/touch.c): a finger doesn't move the pointer; a quick tap (up within
 * 250 ms, moved less than 16 of 1280) is a click where it lifted - the pointer there, the left button down
 * and, 50 ms on, up (the game reads the mouse once a frame, and would miss both at once). The pointer then
 * stays where the game puts it: the D-pad's menus move it onto the item they select (SDL_WarpMouse*). */
static struct
{
    int down;
    uint64_t t0;
    float x0, y0, x, y;
} g_tap;
static uint64_t g_click_up_at;
#define TAP_NS 250000000ull
#define TAP_MOVE 16.0f
#define CLICK_NS 50000000ull

static void audio_report(void);

static void poll_input(void)
{
    audio_report();
    if (!g_input_ready)
    {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_nxpad);
        hidInitializeTouchScreen();
        g_input_ready = 1;
    }
    /* the system's messages: docked or not (switch_main.c's hook), focus, and the player closing the game */
    if (!appletMainLoop() && !closing)
    {
        closing = 1;
        SDL_Event q;
        memset(&q, 0, sizeof q);
        q.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        q.window.windowID = WINDOW_ID;
        push(&q);
        q.type = SDL_EVENT_QUIT;
        push(&q);
    }
    padUpdate(&g_nxpad);
    Pad p;
    memset(&p, 0, sizeof p);
    p.connected = padIsConnected(&g_nxpad);
    if (p.connected)
    {
        uint64_t b = padGetButtons(&g_nxpad);
        p.buttons = map_buttons(b);
        HidAnalogStickState l = padGetStickPos(&g_nxpad, 0), r = padGetStickPos(&g_nxpad, 1);
        p.axes[SDL_GAMEPAD_AXIS_LEFTX] = clamp16(l.x);
        p.axes[SDL_GAMEPAD_AXIS_LEFTY] = clamp16(-l.y); /* SDL's y grows downwards */
        p.axes[SDL_GAMEPAD_AXIS_RIGHTX] = clamp16(r.x);
        p.axes[SDL_GAMEPAD_AXIS_RIGHTY] = clamp16(-r.y);
        p.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (b & HidNpadButton_ZL) ? 32767 : 0;
        p.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (b & HidNpadButton_ZR) ? 32767 : 0;
    }
    lock(&g_plock);
    Pad was = g_padstate;
    g_padstate = p;
    unlock(&g_plock);
    if (!was.connected != !p.connected)
        gamepad_event(p.connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED, 0, 0);
    uint32_t changed = was.buttons ^ p.buttons;
    for (int i = 0; changed && i < 32; ++i)
        if (changed >> i & 1u)
            gamepad_event((p.buttons >> i & 1u) ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP, i, p.buttons >> i & 1u);

    /* the touch screen (handheld): a quick tap is a click (g_tap) */
    uint64_t now = now_ns();
    if (g_click_up_at && now >= g_click_up_at)
    {
        mouse_button(SDL_BUTTON_LEFT, 0);
        g_click_up_at = 0;
    }
    HidTouchScreenState ts;
    if (hidGetTouchScreenStates(&ts, 1))
    {
        if (ts.count > 0)
        {
            float x = (float)ts.touches[0].x * (float)g_view_w / 1280.0f;
            float y = (float)ts.touches[0].y * (float)g_view_h / 720.0f;
            if (!g_tap.down)
                g_tap.down = 1, g_tap.t0 = now, g_tap.x0 = x, g_tap.y0 = y;
            g_tap.x = x, g_tap.y = y;
        }
        else if (g_tap.down)
        {
            g_tap.down = 0;
            float slop = TAP_MOVE * (float)g_view_w / 1280.0f;
            if (now - g_tap.t0 < TAP_NS && fabsf(g_tap.x - g_tap.x0) < slop && fabsf(g_tap.y - g_tap.y0) < slop &&
                !g_click_up_at)
            {
                mouse_move(g_tap.x, g_tap.y);
                mouse_button(SDL_BUTTON_LEFT, 1);
                g_click_up_at = now + CLICK_NS;
            }
        }
    }
}

static int pad_connected(void)
{
    lock(&g_plock);
    int c = g_padstate.connected;
    unlock(&g_plock);
    return c;
}

SDL_JoystickID* SDL_GetGamepads(int* count)
{
    int n = pad_connected() ? 1 : 0;
    SDL_JoystickID* ids = (SDL_JoystickID*)calloc(2, sizeof *ids);
    if (ids && n)
        ids[0] = PAD_ID;
    if (count)
        *count = n;
    return ids;
}
bool SDL_HasGamepad(void) { return pad_connected(); }
bool SDL_IsGamepad(SDL_JoystickID id) { return id == PAD_ID; }
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id)
{
    if (id != PAD_ID)
        return NULL;
    g_pad.open = 1;
    return &g_pad;
}
void SDL_CloseGamepad(SDL_Gamepad* p)
{
    if (p)
        p->open = 0;
}
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID id) { return id == PAD_ID && pad_connected() ? &g_pad : NULL; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* p) { return p ? PAD_ID : 0; }
bool SDL_GamepadConnected(SDL_Gamepad* p) { return p && pad_connected(); }
const char* SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return "Nintendo Switch controller"; }
const char* SDL_GetGamepadName(SDL_Gamepad* p) { (void)p; return "Nintendo Switch controller"; }
/* the Xbox face-button labels the game's and the sign-in's prompts use stay right for the Xbox layout;
 * the Nintendo layout's are Nintendo's */
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad* p)
{
    (void)p;
    return g_layout_xbox ? SDL_GAMEPAD_TYPE_XBOXONE : SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
}
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID id) { (void)id; return SDL_GetGamepadType(&g_pad); }
bool SDL_GetGamepadButton(SDL_Gamepad* p, SDL_GamepadButton b)
{
    if (!p || b < 0 || b >= 32)
        return false;
    lock(&g_plock);
    bool down = (g_padstate.buttons >> b) & 1u;
    unlock(&g_plock);
    return down;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* p, SDL_GamepadAxis a)
{
    if (!p || a < 0 || a >= 6)
        return 0;
    lock(&g_plock);
    Sint16 v = g_padstate.axes[a];
    unlock(&g_plock);
    return v;
}
bool SDL_RumbleGamepad(SDL_Gamepad* p, Uint16 low, Uint16 high, Uint32 ms)
{
    (void)p, (void)low, (void)high, (void)ms; /* HD rumble (later) */
    return true;
}
void SDL_SetGamepadEventsEnabled(bool on) { (void)on; }
void SDL_UpdateGamepads(void) { poll_input(); }

/* --- audio ----------------------------------------------------------------------------------------- */
/* One device: audout, 48 kHz stereo s16, fed in blocks of FRAMES by one thread. Two clients mix into each
 * block: dsound.c's callback stream (SDL_OpenAudioDeviceStream, the game's DirectSound mix, 48 kHz stereo
 * float) and the streams bound to the device (SDL_OpenAudioDevice + SDL_CreateAudioStream + SDL_BindAudioStream:
 * sewave.c's voices, the sign-in screen's effects, s16 at the sound's own rate, resampled here). */
#define FRAMES 480 /* 10 ms at 48 kHz: dsound.c's cursors move in steps this size */
#define OUT_RATE 48000
struct SDL_AudioStream
{
    /* the callback stream (g_stream) */
    SDL_AudioStreamCallback callback;
    void* user;
    /* a voice: its queued samples, interleaved, and the play position in them (frames, 32.32) */
    int voice, channels;
    uint32_t rate;
    float gain;
    int16_t* pcm;
    size_t frames, cap;
    uint64_t pos;
};
static SDL_AudioStream g_stream;
static volatile int g_stream_on; /* dsound's callback is pulled (SDL_ResumeAudioStreamDevice) */

#define AUD_VOICES 16
static Mutex g_voice_lock;
static SDL_AudioStream* g_voices[AUD_VOICES];

/* audout: a few buffers of FRAMES each, refilled as the device hands them back */
#define AUD_BUFS 4
#define AUD_BYTES ((FRAMES * 2 * sizeof(int16_t) + 0xFFF) & ~(size_t)0xFFF)
static int16_t* g_aud_mem[AUD_BUFS];
static AudioOutBuffer g_aud_buf[AUD_BUFS];
static int g_audout;
static float g_mix[FRAMES * 2]; /* the block being made */
static int g_mix_frames;        /* of it, what dsound's callback has put */

/* For the log (audio_report, on the main thread: stdio from other threads is not safe here). Underruns:
 * blocks the device had nothing for, from the clock since the first block against the blocks put. */
static volatile uint32_t g_aud_blocks, g_aud_under, g_aud_mix_max_us, g_aud_gap_max_us;

static void audout_start(void)
{
    if (R_FAILED(audoutInitialize()))
        return;
    if (R_FAILED(audoutStartAudioOut()))
    {
        audoutExit();
        return;
    }
    for (int i = 0; i < AUD_BUFS; ++i)
    {
        g_aud_mem[i] = (int16_t*)memalign(0x1000, AUD_BYTES);
        if (!g_aud_mem[i])
            return;
        memset(g_aud_mem[i], 0, AUD_BYTES);
        g_aud_buf[i].next = NULL;
        g_aud_buf[i].buffer = g_aud_mem[i];
        g_aud_buf[i].buffer_size = AUD_BYTES;
        g_aud_buf[i].data_size = FRAMES * 2 * sizeof(int16_t);
        g_aud_buf[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_aud_buf[i]);
    }
    g_audout = 1;
}

/* one block to the device: waits for a buffer it has played (the thread's pace). 0 if the wait failed */
static int audout_put(const float* f)
{
    AudioOutBuffer* done = NULL;
    u32 n = 0;
    if (R_FAILED(audoutWaitPlayFinish(&done, &n, UINT64_MAX)) || !done)
        return 0;
    int16_t* out = (int16_t*)done->buffer;
    for (int i = 0; i < FRAMES * 2; ++i)
    {
        float v = f[i] * 32767.0f;
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    done->data_size = FRAMES * 2 * sizeof(int16_t);
    audoutAppendAudioOutBuffer(done);
    static uint64_t first, prev;
    uint64_t now = now_ns();
    if (!first)
        first = prev = now;
    uint32_t gap = (uint32_t)((now - prev) / 1000u), blocks = ++g_aud_blocks;
    prev = now;
    if (gap > g_aud_gap_max_us)
        g_aud_gap_max_us = gap;
    uint64_t due = (now - first) * OUT_RATE / 1000000000u / FRAMES; /* blocks the device has played */
    if (due > (uint64_t)blocks + AUD_BUFS && due - blocks - AUD_BUFS > g_aud_under)
        g_aud_under = (uint32_t)(due - blocks - AUD_BUFS);
    return 1;
}

/* the bound voices into the block: linear resampling to the output rate, as dsound.c's mixer */
static void mix_voices(float* out)
{
    mutexLock(&g_voice_lock);
    for (int v = 0; v < AUD_VOICES; ++v)
    {
        SDL_AudioStream* s = g_voices[v];
        if (!s || !s->frames)
            continue;
        uint64_t step = ((uint64_t)s->rate << 32) / OUT_RATE, end = (uint64_t)s->frames << 32;
        int ch = s->channels;
        for (int f = 0; f < FRAMES && s->pos < end; ++f, s->pos += step)
        {
            size_t i = (size_t)(s->pos >> 32), nx = i + 1 < s->frames ? i + 1 : i;
            float t = (float)(uint32_t)s->pos / 4294967296.0f, g = s->gain / 32768.0f;
            for (int c = 0; c < 2; ++c)
            {
                int k = c < ch ? c : 0;
                float a = s->pcm[i * ch + k], b = s->pcm[nx * ch + k];
                out[2 * f + c] += (a + (b - a) * t) * g;
            }
        }
        if (s->pos >= end)
            s->frames = 0, s->pos = 0;
    }
    mutexUnlock(&g_voice_lock);
}

static void audio_block(void)
{
    uint64_t t0 = now_ns();
    memset(g_mix, 0, sizeof g_mix);
    g_mix_frames = 0;
    if (__atomic_load_n(&g_stream_on, __ATOMIC_ACQUIRE))
        g_stream.callback(g_stream.user, &g_stream, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
    mix_voices(g_mix);
    uint32_t us = (uint32_t)((now_ns() - t0) / 1000u);
    if (us > g_aud_mix_max_us)
        g_aud_mix_max_us = us;
}

/* every 10 s while the device runs: blocks put, underruns so far, the longest mix and gap since the last */
static void audio_report(void)
{
    static uint64_t last;
    static uint32_t was;
    uint64_t now = now_ns();
    if (now - last < 10000000000ull || g_aud_blocks == was)
        return;
    last = now, was = g_aud_blocks;
    uint32_t mix = __atomic_exchange_n(&g_aud_mix_max_us, 0, __ATOMIC_RELAXED);
    uint32_t gap = __atomic_exchange_n(&g_aud_gap_max_us, 0, __ATOMIC_RELAXED);
    uint32_t threads = 0, idle = 0;
    plat_thread_counts(&threads, &idle);
    rt_log("[audio] blocks %u underruns %u mix max %u us gap max %u.%u ms; threads %u (%u idle)%s\n", was, g_aud_under,
        mix, gap / 1000, gap / 100 % 10, threads, idle, g_audout ? "" : " (no device: paced to the clock)");
}

static void audio_thread(void* arg)
{
    (void)arg;
    /* one step above the game's threads (plat_thread_start gives the main thread's priority, and Horizon
     * doesn't share a core between threads of one priority): a buffer coming back takes the core from a
     * busy thread there. Its work, a block's mix, is small. */
    s32 prio = 0;
    if (R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) && prio > 0x1C)
        svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)(prio - 1));
    audout_start();
    uint64_t start = now_ns(), done = 0; /* without the device: paced to the clock */
    for (;;)
    {
        if (g_audout)
        {
            audio_block();
            if (!audout_put(g_mix))
                svcSleepThread(10000000); /* a failed wait: never spin */
            continue;
        }
        uint64_t due = (now_ns() - start) * OUT_RATE / 1000000000u;
        while (done + FRAMES <= due)
        {
            audio_block();
            done += FRAMES;
        }
        svcSleepThread(5000000);
    }
}

/* the thread, once, for whichever client needs it first (dsound's Resume, a voice's first sound);
 * plat_thread_start puts it off the game's core */
static int audio_start(void)
{
    static int started;
    static Mutex start_lock;
    mutexLock(&start_lock);
    if (!started)
        started = plat_thread_start(audio_thread, NULL);
    mutexUnlock(&start_lock);
    return started;
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
    void* user)
{
    (void)dev, (void)spec; /* dsound.c asks for 48 kHz stereo float */
    g_stream.callback = callback;
    g_stream.user = user;
    return &g_stream;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s)
{
    if (s != &g_stream || !s->callback || !audio_start())
        return false;
    __atomic_store_n(&g_stream_on, 1, __ATOMIC_RELEASE);
    return true;
}
bool SDL_PauseAudioStreamDevice(SDL_AudioStream* s)
{
    if (s == &g_stream)
        __atomic_store_n(&g_stream_on, 0, __ATOMIC_RELEASE);
    return true;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len)
{
    if (!s || !buf || len <= 0)
        return false;
    if (s == &g_stream)
    {
        /* dsound's mix, from its callback on the audio thread: into the block being made */
        int frames = len / (int)(2 * sizeof(float));
        if (frames > FRAMES - g_mix_frames)
            frames = FRAMES - g_mix_frames;
        memcpy(g_mix + g_mix_frames * 2, buf, (size_t)frames * 2 * sizeof(float));
        g_mix_frames += frames;
        return true;
    }
    if (!s->voice || !audio_start()) /* the thread from the first sound, not the device's opening (below) */
        return false;
    size_t n = (size_t)len / (2 * (size_t)s->channels);
    mutexLock(&g_voice_lock);
    size_t used = (size_t)(s->pos >> 32); /* what has played goes first */
    if (used >= s->frames)
        s->frames = 0, s->pos = 0;
    else if (used)
    {
        memmove(s->pcm, s->pcm + used * s->channels, (s->frames - used) * s->channels * sizeof *s->pcm);
        s->frames -= used, s->pos -= (uint64_t)used << 32;
    }
    int ok = 1;
    if (s->frames + n > s->cap)
    {
        size_t cap = s->frames + n;
        int16_t* p = (int16_t*)realloc(s->pcm, cap * s->channels * sizeof *p);
        if (p)
            s->pcm = p, s->cap = cap;
        else
            ok = 0;
    }
    if (ok)
    {
        memcpy(s->pcm + s->frames * s->channels, buf, n * s->channels * sizeof *s->pcm);
        s->frames += n;
    }
    mutexUnlock(&g_voice_lock);
    return ok;
}

/* The addons' sounds: no WAV loads (the addons are not built for the Switch) */
bool SDL_LoadWAV(const char* path, SDL_AudioSpec* spec, Uint8** buf, Uint32* len)
{
    (void)path, (void)spec, (void)buf, (void)len;
    return false;
}

/* --- the device's own streams (sewave.c): s16, mono or stereo, any rate -------------------------------- */
/* The thread is not started here: the sign-in opens the device just after the render thread is made, and a
 * thread made then can meet the start-up race (docs/switch.md "Open"): a black screen on the console. */
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec)
{
    (void)dev, (void)spec;
    return 1;
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev, SDL_AudioSpec* spec, int* frames)
{
    (void)dev;
    if (spec)
        spec->format = SDL_AUDIO_F32, spec->channels = 2, spec->freq = OUT_RATE;
    if (frames)
        *frames = FRAMES;
    return true;
}
SDL_AudioStream* SDL_CreateAudioStream(const SDL_AudioSpec* src, const SDL_AudioSpec* dst)
{
    (void)dst; /* always the device's */
    if (!src || src->format != SDL_AUDIO_S16LE || src->channels < 1 || src->channels > 2 || src->freq <= 0 ||
        src->freq > 200000)
        return NULL;
    SDL_AudioStream* s = (SDL_AudioStream*)calloc(1, sizeof *s);
    if (s)
        s->voice = 1, s->channels = src->channels, s->rate = (uint32_t)src->freq, s->gain = 1.0f;
    return s;
}
bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream* s)
{
    if (!dev || !s || !s->voice)
        return false;
    int ok = 0;
    mutexLock(&g_voice_lock);
    for (int v = 0; v < AUD_VOICES && !ok; ++v)
        if (g_voices[v] == s)
            ok = 1;
    for (int v = 0; v < AUD_VOICES && !ok; ++v)
        if (!g_voices[v])
            g_voices[v] = s, ok = 1;
    mutexUnlock(&g_voice_lock);
    return ok;
}
bool SDL_ClearAudioStream(SDL_AudioStream* s)
{
    if (s && s->voice)
    {
        mutexLock(&g_voice_lock);
        s->frames = 0, s->pos = 0;
        mutexUnlock(&g_voice_lock);
    }
    return true;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain)
{
    if (!s || !s->voice || gain < 0)
        return false;
    mutexLock(&g_voice_lock);
    s->gain = gain;
    mutexUnlock(&g_voice_lock);
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s)
{
    if (!s || !s->voice)
        return 0;
    mutexLock(&g_voice_lock);
    size_t used = (size_t)(s->pos >> 32), left = used < s->frames ? s->frames - used : 0;
    mutexUnlock(&g_voice_lock);
    return (int)(left * 2 * (size_t)s->channels);
}
bool SDL_FlushAudioStream(SDL_AudioStream* s) { (void)s; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* s)
{
    if (!s || !s->voice)
        return;
    mutexLock(&g_voice_lock);
    for (int v = 0; v < AUD_VOICES; ++v)
        if (g_voices[v] == s)
            g_voices[v] = NULL;
    mutexUnlock(&g_voice_lock);
    free(s->pcm);
    free(s);
}
/* the device stays: dsound's stream may still be on it, and the thread is cheap */
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }

/* --- the rest host64 and its screens use ------------------------------------------------------------- */
/* host64's own files (the sign-in's settings, saved logins, patch.ver for an install without one):
 * FFXI_DATA_DIR, else sdmc:/switch/xi-on-anything/data/. host64's --data-dir wins over both. */
char* SDL_GetPrefPath(const char* org, const char* app)
{
    (void)org, (void)app;
    const char* d = getenv("FFXI_DATA_DIR");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", d && *d ? d : APP_DIR "/data");
    mkdir(dir, 0777);
    char buf[1040];
    snprintf(buf, sizeof buf, "%s/", dir);
    return strdup(buf);
}
const char* SDL_GetBasePath(void) { return APP_DIR "/"; }
bool SDL_CreateDirectory(const char* path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    /* past the device's "sdmc:/" */
    char* c = strstr(p, ":/");
    for (c = c ? c + 2 : p + 1; *c; ++c)
        if (*c == '/')
        {
            *c = 0;
            mkdir(p, 0777);
            *c = '/';
        }
    return mkdir(p, 0777) == 0 || errno == EEXIST;
}
bool SDL_RenamePath(const char* from, const char* to) { return rename(from, to) == 0; }
char* SDL_strdup(const char* s) { return strdup(s); }
size_t SDL_strlcpy(char* dst, const char* src, size_t n)
{
    size_t len = strlen(src);
    if (n)
    {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}
size_t SDL_strlcat(char* dst, const char* src, size_t n)
{
    size_t d = strnlen(dst, n);
    return d == n ? n + strlen(src) : d + SDL_strlcpy(dst + d, src, n - d);
}
Uint32 SDL_GetAtomicU32(SDL_AtomicU32* a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
void SDL_LockSpinlock(SDL_SpinLock* l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}
void SDL_UnlockSpinlock(SDL_SpinLock* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
int SDL_GetAtomicInt(SDL_AtomicInt* a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
int SDL_SetAtomicInt(SDL_AtomicInt* a, int v) { return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST); }
