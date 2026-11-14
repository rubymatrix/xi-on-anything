/* The SDL3 calls the runtime and the sign-in screen make (user32.c, input.c, dsound.c, host/signin.c and
 * what it uses), for the Nintendo Switch build, where SDL3 has no back end: the port of sdl_web.c (itself
 * sdl_uwp.c's, the Xbox's) to libnx. Compiled against SDL3's headers for its types, linked instead of SDL3.
 *
 * There is one window, the screen: 1280x720 handheld, 1920x1080 docked (nx_bridge.h's nx_set_view_size,
 * from host/switch_main.c). Input is read from HID on the game's thread, in SDL_PollEvent: the first
 * controller (or the handheld Joy-Con) is the SDL gamepad, the touch screen is the mouse. Text comes from
 * the system keyboard (swkbd), shown over a sign-in field with the field's text and handed back whole, as
 * the Xbox's keyboard does. Audio is a thread that pulls the mix every 10 ms, as SDL's device would -
 * dsound.c moves the DirectSound cursors, and signals the game's notification events, from that callback -
 * and hands it to audout. */
#include <errno.h>
#include <malloc.h>
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
bool SDL_WarpMouseGlobal(float x, float y) { (void)x, (void)y; return false; }
void SDL_WarpMouseInWindow(SDL_Window* w, float x, float y) { (void)w, (void)x, (void)y; }
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

static int g_touching;

static void poll_input(void)
{
    if (!g_input_ready)
    {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_nxpad);
        hidInitializeTouchScreen();
        g_input_ready = 1;
    }
    /* the system's messages: docked or not (switch_main.c's hook), focus, and the player closing the game */
    static int closing;
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

    /* the touch screen (handheld): the first finger is the mouse, its left button down while it touches */
    HidTouchScreenState ts;
    if (hidGetTouchScreenStates(&ts, 1))
    {
        if (ts.count > 0)
        {
            float x = (float)ts.touches[0].x * (float)g_view_w / 1280.0f;
            float y = (float)ts.touches[0].y * (float)g_view_h / 720.0f;
            mouse_move(x, y);
            if (!g_touching)
                mouse_button(SDL_BUTTON_LEFT, 1);
            g_touching = 1;
        }
        else if (g_touching)
        {
            mouse_button(SDL_BUTTON_LEFT, 0);
            g_touching = 0;
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
#define FRAMES 480 /* 10 ms at 48 kHz: dsound.c's cursors move in steps this size */
struct SDL_AudioStream
{
    SDL_AudioStreamCallback callback;
    void* user;
    volatile int running;
};
static SDL_AudioStream g_stream;

/* audout: 48 kHz stereo s16. A few buffers of FRAMES each, refilled as the device hands them back. */
#define AUD_BUFS 4
#define AUD_BYTES ((FRAMES * 2 * sizeof(int16_t) + 0xFFF) & ~(size_t)0xFFF)
static int16_t* g_aud_mem[AUD_BUFS];
static AudioOutBuffer g_aud_buf[AUD_BUFS];
static int g_audout;
static float g_mix[FRAMES * 2];
static int g_mix_frames;

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

/* one mixed block to the device: waits for a buffer it has played */
static void audout_put(const float* f)
{
    if (!g_audout)
        return;
    AudioOutBuffer* done = NULL;
    u32 n = 0;
    if (R_FAILED(audoutWaitPlayFinish(&done, &n, UINT64_MAX)) || !done)
        return;
    int16_t* out = (int16_t*)done->buffer;
    for (int i = 0; i < FRAMES * 2; ++i)
    {
        float v = f[i] * 32767.0f;
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    done->data_size = FRAMES * 2 * sizeof(int16_t);
    audoutAppendAudioOutBuffer(done);
}

static void* audio_thread(void* arg)
{
    SDL_AudioStream* s = (SDL_AudioStream*)arg;
    audout_start();
    uint64_t start = now_ns(), done = 0; /* without the device: paced to the clock */
    while (s->running)
    {
        if (g_audout)
        {
            /* the device's pace: one block per buffer it gives back */
            s->callback(s->user, s, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
            continue;
        }
        uint64_t due = (now_ns() - start) * 48000 / 1000000000u;
        while (done + FRAMES <= due)
        {
            s->callback(s->user, s, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
            done += FRAMES;
        }
        svcSleepThread(5000000);
    }
    return NULL;
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
    if (!s || __atomic_exchange_n(&s->running, 1, __ATOMIC_SEQ_CST))
        return true;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, audio_thread, s);
    pthread_attr_destroy(&a);
    if (r)
    {
        s->running = 0;
        return false;
    }
    return true;
}
bool SDL_PauseAudioStreamDevice(SDL_AudioStream* s) { (void)s; return true; }

/* the mix, as dsound.c hands it over: gathered into whole blocks for the device */
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len)
{
    (void)s;
    const float* f = (const float*)buf;
    int frames = len / (int)(2 * sizeof(float));
    while (frames > 0)
    {
        int take = FRAMES - g_mix_frames < frames ? FRAMES - g_mix_frames : frames;
        memcpy(g_mix + g_mix_frames * 2, f, (size_t)take * 2 * sizeof(float));
        g_mix_frames += take, f += take * 2, frames -= take;
        if (g_mix_frames == FRAMES)
        {
            audout_put(g_mix);
            g_mix_frames = 0;
        }
    }
    return true;
}

/* The addons' sounds and the sign-in screen's: no WAV loads and no second device, so none plays */
bool SDL_LoadWAV(const char* path, SDL_AudioSpec* spec, Uint8** buf, Uint32* len)
{
    (void)path, (void)spec, (void)buf, (void)len;
    return false;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) { (void)s; return 0; }
bool SDL_FlushAudioStream(SDL_AudioStream* s) { (void)s; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* s) { (void)s; }
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec)
{
    (void)dev, (void)spec;
    return 0;
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev, SDL_AudioSpec* spec, int* frames)
{
    (void)dev, (void)spec, (void)frames;
    return false;
}
SDL_AudioStream* SDL_CreateAudioStream(const SDL_AudioSpec* src, const SDL_AudioSpec* dst)
{
    (void)src, (void)dst;
    return NULL;
}
bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream* s) { (void)dev, (void)s; return false; }
bool SDL_ClearAudioStream(SDL_AudioStream* s) { (void)s; return true; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) { (void)s, (void)gain; return true; }

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
