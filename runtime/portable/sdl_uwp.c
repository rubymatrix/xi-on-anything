/* The SDL3 calls the runtime and the sign-in screen make (user32.c, input.c, dsound.c, host/signin.c
 * and what it uses), for the UWP build, where SDL3 has no back end. Compiled against SDL3's headers for its types, linked instead of SDL3.lib. There is one
 * window, the app's view; its input arrives through uwp_bridge.h and leaves here as SDL events. Audio
 * is a thread that pulls the mix every 10 ms, as SDL's device would - dsound.c moves the DirectSound
 * cursors, and signals the game's notification events, from that callback - and hands it to the app.
 *
 * Only Win32 calls UWP apps may make: SRW locks, threads, Sleep. */
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "uwp_bridge.h"

/* --- mutexes ---------------------------------------------------------------------------------------- */
struct SDL_Mutex
{
    CRITICAL_SECTION cs; /* recursive, as SDL's */
};

SDL_Mutex* SDL_CreateMutex(void)
{
    SDL_Mutex* m = (SDL_Mutex*)calloc(1, sizeof *m);
    if (m)
        InitializeCriticalSectionEx(&m->cs, 0, 0);
    return m;
}
void SDL_LockMutex(SDL_Mutex* m)
{
    if (m)
        EnterCriticalSection(&m->cs);
}
void SDL_UnlockMutex(SDL_Mutex* m)
{
    if (m)
        LeaveCriticalSection(&m->cs);
}

/* --- odds and ends ------------------------------------------------------------------------------- */
const char* SDL_GetError(void) { return "not available in the UWP build"; }
void SDL_free(void* p) { free(p); }
int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}
bool SDL_SetHint(const char* name, const char* value) { (void)name, (void)value; return true; }
/* no hints are set here (the ones asked for are macOS's) */
const char* SDL_GetHint(const char* name) { (void)name; return NULL; }
bool SDL_GetHintBoolean(const char* name, bool default_value) { (void)name; return default_value; }
bool SDL_Init(SDL_InitFlags flags) { (void)flags; return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return true; }
/* nothing is reported initialized: callers that check (modern.c's display modes, the addons' sounds)
 * take their paths for no SDL */
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { (void)flags; return 0; }
bool SDL_SetClipboardText(const char* text) { (void)text; return false; }
bool SDL_HasClipboardText(void) { return false; }
char* SDL_GetClipboardText(void) { return (char*)calloc(1, 1); } /* SDL's "" for none, freed with SDL_free */

Uint64 SDL_GetTicks(void) { return GetTickCount64(); }
int SDL_GetAtomicInt(SDL_AtomicInt* a) { return (int)InterlockedCompareExchange((volatile LONG*)&a->value, 0, 0); }
int SDL_SetAtomicInt(SDL_AtomicInt* a, int v) { return (int)InterlockedExchange((volatile LONG*)&a->value, v); }

size_t SDL_strlcpy(char* dst, const char* src, size_t maxlen)
{
    size_t n = strlen(src);
    if (maxlen)
    {
        size_t k = n < maxlen - 1 ? n : maxlen - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}
size_t SDL_strlcat(char* dst, const char* src, size_t maxlen)
{
    size_t d = strnlen(dst, maxlen);
    return d + SDL_strlcpy(dst + d, src, maxlen - d);
}
char* SDL_strdup(const char* s) { return _strdup(s); }

/* the app passes host64 --data-dir (its LocalState): nothing asks for this */
char* SDL_GetPrefPath(const char* org, const char* app)
{
    (void)org, (void)app;
    return NULL;
}

/* UTF-8 paths, as SDL's, to the wide calls UWP apps may make */
static int wide(const char* s, wchar_t* out, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0; }

bool SDL_CreateDirectory(const char* path)
{
    wchar_t w[1024];
    return wide(path, w, 1024) && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}
bool SDL_RenamePath(const char* from, const char* to)
{
    wchar_t a[1024], b[1024];
    return wide(from, a, 1024) && wide(to, b, 1024) && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}

/* --- the event queue ------------------------------------------------------------------------------ */
#define QUEUE 1024
static SRWLOCK g_qlock = SRWLOCK_INIT;
static SDL_Event g_queue[QUEUE];
static unsigned g_qhead, g_qtail;
static char g_text[QUEUE][32]; /* SDL_EVENT_TEXT_INPUT points at these */

static int push(const SDL_Event* e)
{
    AcquireSRWLockExclusive(&g_qlock);
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
    ReleaseSRWLockExclusive(&g_qlock);
    return ok;
}

bool SDL_PollEvent(SDL_Event* e)
{
    AcquireSRWLockExclusive(&g_qlock);
    int any = g_qhead != g_qtail;
    if (any)
    {
        *e = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE;
    }
    ReleaseSRWLockExclusive(&g_qlock);
    return any;
}

/* --- the window ------------------------------------------------------------------------------------ */
#define WINDOW_ID 1
struct SDL_Window
{
    int w, h;
};
static SDL_Window g_window;
static volatile LONG g_view_w = 1920, g_view_h = 1080;
static int g_have_window;

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags)
{
    (void)title, (void)flags;
    g_window.w = w, g_window.h = h;
    g_have_window = 1;
    return &g_window;
}
void SDL_DestroyWindow(SDL_Window* w) { (void)w; g_have_window = 0; }
SDL_WindowID SDL_GetWindowID(SDL_Window* w) { return w ? WINDOW_ID : 0; }
bool SDL_SetWindowPosition(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; return true; }
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
SDL_Window* SDL_GetWindowFromEvent(const SDL_Event* e) { (void)e; return g_have_window ? &g_window : NULL; }
bool SDL_ShowWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_SetWindowTitle(SDL_Window* w, const char* title) { (void)w, (void)title; return true; } /* the app's */
bool SDL_HideWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_SetWindowFullscreen(SDL_Window* w, bool on) { (void)w, (void)on; return true; } /* the view is the screen */
bool SDL_SyncWindow(SDL_Window* w) { (void)w; return true; }
bool SDL_RaiseWindow(SDL_Window* w) { (void)w; return true; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* w) { (void)w; return 0; } /* shown, and the view is all there is */
bool SDL_SetWindowFullscreenMode(SDL_Window* w, const SDL_DisplayMode* m) { (void)w, (void)m; return true; }
float SDL_GetWindowPixelDensity(SDL_Window* w) { (void)w; return 1.0f; } /* the view's sizes are pixels already */
SDL_Window** SDL_GetWindows(int* count)
{
    SDL_Window** list = (SDL_Window**)calloc(2, sizeof *list);
    int n = list && g_have_window ? 1 : 0;
    if (n)
        list[0] = &g_window;
    if (count)
        *count = n;
    return list;
}
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* w) { return w ? 1 : 0; }
/* the TV's output mode is the console's to choose: the desktop mode only */
SDL_DisplayMode** SDL_GetFullscreenDisplayModes(SDL_DisplayID id, int* count)
{
    (void)id;
    if (count)
        *count = 0;
    return (SDL_DisplayMode**)calloc(1, sizeof(SDL_DisplayMode*));
}

/* --- properties: the few the sign-in screen sets for its text input ---------------------------------- */
#define PROPS 4
#define PROP_ITEMS 8
typedef struct Prop
{
    char name[64];
    Sint64 number;
    char text[256];
} Prop;
static struct
{
    int used;
    int n;
    Prop items[PROP_ITEMS];
} g_props[PROPS];
static SRWLOCK g_prlock = SRWLOCK_INIT;

SDL_PropertiesID SDL_CreateProperties(void)
{
    AcquireSRWLockExclusive(&g_prlock);
    SDL_PropertiesID id = 0;
    for (int i = 0; i < PROPS && !id; ++i)
        if (!g_props[i].used)
        {
            memset(&g_props[i], 0, sizeof g_props[i]);
            g_props[i].used = 1;
            id = (SDL_PropertiesID)(i + 1);
        }
    ReleaseSRWLockExclusive(&g_prlock);
    return id;
}
void SDL_DestroyProperties(SDL_PropertiesID props)
{
    if (props >= 1 && props <= PROPS)
        g_props[props - 1].used = 0;
}
static Prop* prop(SDL_PropertiesID props, const char* name, int add)
{
    if (props < 1 || props > PROPS || !g_props[props - 1].used)
        return NULL;
    for (int i = 0; i < g_props[props - 1].n; ++i)
        if (!strcmp(g_props[props - 1].items[i].name, name))
            return &g_props[props - 1].items[i];
    if (!add || g_props[props - 1].n == PROP_ITEMS)
        return NULL;
    Prop* p = &g_props[props - 1].items[g_props[props - 1].n++];
    memset(p, 0, sizeof *p);
    SDL_strlcpy(p->name, name, sizeof p->name);
    return p;
}
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 value)
{
    Prop* p = prop(props, name, 1);
    if (p)
        p->number = value;
    return p != NULL;
}
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char* name, bool value) { return SDL_SetNumberProperty(props, name, value); }
bool SDL_SetStringProperty(SDL_PropertiesID props, const char* name, const char* value)
{
    Prop* p = prop(props, name, 1);
    if (p)
        SDL_strlcpy(p->text, value ? value : "", sizeof p->text);
    return p != NULL;
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 default_value)
{
    Prop* p = prop(props, name, 0);
    return p ? p->number : default_value;
}
const char* SDL_GetStringProperty(SDL_PropertiesID props, const char* name, const char* default_value)
{
    Prop* p = prop(props, name, 0);
    return p ? p->text : default_value;
}
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* w) { (void)w; return 0; }
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* default_value)
{
    (void)props, (void)name;
    return default_value;
}

/* --- text input: the on-screen keyboard -------------------------------------------------------------- */
static UwpTextInput g_text_input;
static int g_keyboard_up;

void uwp_set_text_input(UwpTextInput cb) { g_text_input = cb; }

/* Text events arrive with or without this; it only asks for nothing to be shown. */
bool SDL_StartTextInput(SDL_Window* w) { (void)w; return true; }

/* host/signin.c asks for the keyboard over a field, with the field's text so far ("xi.textinput.text":
 * the app's keyboard edits the whole of it) */
bool SDL_StartTextInputWithProperties(SDL_Window* w, SDL_PropertiesID props)
{
    (void)w;
    Sint64 type = SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
    const char* text = SDL_GetStringProperty(props, "xi.textinput.text", "");
    int kind = type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN || type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE ? UWP_TEXT_SECRET
             : type == SDL_TEXTINPUT_TYPE_NUMBER                                                                   ? UWP_TEXT_NUMBER
                                                                                                                   : UWP_TEXT;
    UwpTextInput cb = g_text_input;
    if (cb)
        cb(1, kind, text);
    g_keyboard_up = cb != NULL;
    return true;
}

bool SDL_StopTextInput(SDL_Window* w)
{
    (void)w;
    UwpTextInput cb = g_text_input;
    if (cb && g_keyboard_up)
        cb(0, 0, "");
    g_keyboard_up = 0;
    return true;
}

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

void uwp_view_size(int w, int h)
{
    if (w > 0 && h > 0)
        InterlockedExchange(&g_view_w, w), InterlockedExchange(&g_view_h, h);
}

/* --- keyboard and mouse ---------------------------------------------------------------------------- */
static SRWLOCK g_mlock = SRWLOCK_INIT;
static float g_mx, g_my;
static SDL_MouseButtonFlags g_mbuttons;

static bool g_keystate[SDL_SCANCODE_COUNT];

const bool* SDL_GetKeyboardState(int* numkeys)
{
    if (numkeys)
        *numkeys = SDL_SCANCODE_COUNT;
    return g_keystate;
}

bool SDL_OpenURL(const char* url) { (void)url; return false; } /* no browser from the game on the console */

/* The key a scancode is on a US layout, as SDL would report it: the game reads scancodes, the
 * sign-in screen keys. */
static SDL_Keycode keycode_of(int sc)
{
    static const char PUNCT[] = "-=[]\\\0;'`,./"; /* SDL_SCANCODE_MINUS (45) to SLASH (56); 50 is non-US */
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return (SDL_Keycode)('a' + sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return (SDL_Keycode)('1' + sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH && PUNCT[sc - SDL_SCANCODE_MINUS])
        return (SDL_Keycode)PUNCT[sc - SDL_SCANCODE_MINUS];
    switch (sc)
    {
    case SDL_SCANCODE_0: return SDLK_0;
    case SDL_SCANCODE_RETURN: return SDLK_RETURN;
    case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_TAB: return SDLK_TAB;
    case SDL_SCANCODE_SPACE: return SDLK_SPACE;
    default: return SDL_SCANCODE_TO_KEYCODE(sc);
    }
}

static void key_event(int scancode, int down, int repeat, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = WINDOW_ID;
    e.key.scancode = (SDL_Scancode)scancode;
    e.key.key = keycode_of(scancode);
    e.key.mod = mod;
    e.key.down = down != 0;
    e.key.repeat = repeat != 0;
    push(&e);
}

void uwp_key(int scancode, int down, int repeat)
{
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
        g_keystate[scancode] = down != 0;
    key_event(scancode, down, repeat, SDL_GetModState());
}

void uwp_text(const char* utf8)
{
    /* in pieces that fit the queue's text slots, whole UTF-8 characters each */
    while (*utf8)
    {
        char piece[sizeof g_text[0]];
        size_t n = strlen(utf8);
        if (n >= sizeof piece)
        {
            n = sizeof piece - 1;
            while (n && ((unsigned char)utf8[n] & 0xC0) == 0x80)
                --n;
        }
        memcpy(piece, utf8, n);
        piece[n] = 0;
        utf8 += n;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = WINDOW_ID;
        e.text.text = piece; /* copied by push */
        push(&e);
    }
}

/* The on-screen keyboard's text, for the field it was opened over: the caret to the end, everything
 * before it erased (Ctrl+Backspace), then the text typed in. */
void uwp_replace_text(const char* utf8)
{
    key_event(SDL_SCANCODE_END, 1, 0, SDL_KMOD_NONE);
    key_event(SDL_SCANCODE_END, 0, 0, SDL_KMOD_NONE);
    key_event(SDL_SCANCODE_BACKSPACE, 1, 0, SDL_KMOD_LCTRL);
    key_event(SDL_SCANCODE_BACKSPACE, 0, 0, SDL_KMOD_LCTRL);
    uwp_text(utf8);
}

void uwp_mouse_move(float x, float y)
{
    AcquireSRWLockExclusive(&g_mlock);
    g_mx = x, g_my = y;
    SDL_MouseButtonFlags b = g_mbuttons;
    ReleaseSRWLockExclusive(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = WINDOW_ID;
    e.motion.state = b;
    e.motion.x = x, e.motion.y = y;
    push(&e);
}

void uwp_mouse_button(int button, int down)
{
    AcquireSRWLockExclusive(&g_mlock);
    if (down)
        g_mbuttons |= SDL_BUTTON_MASK(button);
    else
        g_mbuttons &= ~SDL_BUTTON_MASK(button);
    float x = g_mx, y = g_my;
    ReleaseSRWLockExclusive(&g_mlock);
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

void uwp_mouse_wheel(float dy)
{
    AcquireSRWLockShared(&g_mlock);
    float x = g_mx, y = g_my;
    ReleaseSRWLockShared(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = WINDOW_ID;
    e.wheel.y = dy;
    e.wheel.integer_y = (Sint32)dy;
    e.wheel.mouse_x = x, e.wheel.mouse_y = y;
    push(&e);
}

void uwp_focus(int on)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = on ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = WINDOW_ID;
    push(&e);
}

void uwp_close(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = WINDOW_ID;
    push(&e);
}

SDL_Keymod SDL_GetModState(void)
{
    /* the addon host's binds and ImGui: from the keys the app has passed in */
    SDL_Keymod m = 0;
    const bool* k = SDL_GetKeyboardState(NULL);
    if (k)
    {
        if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL])
            m |= SDL_KMOD_CTRL;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT])
            m |= SDL_KMOD_SHIFT;
        if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT])
            m |= SDL_KMOD_ALT;
    }
    return m;
}

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y)
{
    AcquireSRWLockShared(&g_mlock);
    if (x)
        *x = g_mx;
    if (y)
        *y = g_my;
    SDL_MouseButtonFlags b = g_mbuttons;
    ReleaseSRWLockShared(&g_mlock);
    return b;
}
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) { return SDL_GetMouseState(x, y); }
void SDL_WarpMouseGlobal(float x, float y) { (void)x, (void)y; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

/* --- the controller ------------------------------------------------------------------------------------ */
#define PAD_ID 1
struct SDL_Gamepad
{
    int open;
};
static SDL_Gamepad g_pad;
static SRWLOCK g_plock = SRWLOCK_INIT;
static UwpPad g_padstate;
static UwpRumble g_rumble;

void uwp_gamepad(const UwpPad* p)
{
    AcquireSRWLockExclusive(&g_plock);
    int was = g_padstate.connected;
    uint32_t held = g_padstate.buttons;
    g_padstate = *p;
    ReleaseSRWLockExclusive(&g_plock);
    if (!was != !p->connected)
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = p->connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
        e.gdevice.which = PAD_ID;
        push(&e);
    }
    /* the buttons that changed, as events (the sign-in screen's); the game polls their state */
    uint32_t now = p->connected ? p->buttons : 0, changed = (was ? held : 0) ^ now;
    for (int b = 0; changed; ++b, changed >>= 1)
        if (changed & 1)
        {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = (now >> b) & 1 ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.which = PAD_ID;
            e.gbutton.button = (Uint8)b;
            e.gbutton.down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            push(&e);
        }
}
void uwp_set_rumble(UwpRumble rumble) { g_rumble = rumble; }

static int pad_connected(void)
{
    AcquireSRWLockShared(&g_plock);
    int c = g_padstate.connected;
    ReleaseSRWLockShared(&g_plock);
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
const char* SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return "Xbox controller"; }
const char* SDL_GetGamepadName(SDL_Gamepad* p) { (void)p; return "Xbox controller"; }
/* input.c lays the buttons out by the pad's kind: an Xbox One controller's */
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad* p) { (void)p; return SDL_GAMEPAD_TYPE_XBOXONE; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad* p) { (void)p; return 0x045E; }  /* Microsoft */
Uint16 SDL_GetGamepadProduct(SDL_Gamepad* p) { (void)p; return 0x02EA; } /* Xbox One S controller */
bool SDL_GetGamepadButton(SDL_Gamepad* p, SDL_GamepadButton b)
{
    if (!p || b < 0 || b >= 32)
        return false;
    AcquireSRWLockShared(&g_plock);
    bool down = (g_padstate.buttons >> b) & 1u;
    ReleaseSRWLockShared(&g_plock);
    return down;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* p, SDL_GamepadAxis a)
{
    if (!p || a < 0 || a >= 6)
        return 0;
    AcquireSRWLockShared(&g_plock);
    Sint16 v = g_padstate.axes[a];
    ReleaseSRWLockShared(&g_plock);
    return v;
}
bool SDL_RumbleGamepad(SDL_Gamepad* p, Uint16 low, Uint16 high, Uint32 ms)
{
    (void)p, (void)ms;
    UwpRumble r = g_rumble;
    if (r)
        r(low, high);
    return true;
}

/* --- audio ----------------------------------------------------------------------------------------- */
#define FRAMES 480 /* 10 ms at 48 kHz: dsound.c's cursors move in steps this size */
struct SDL_AudioStream
{
    SDL_AudioStreamCallback callback;
    void* user;
    volatile LONG running;
};
static SDL_AudioStream g_stream;
static UwpAudioSink g_sink;

void uwp_set_audio_sink(UwpAudioSink sink) { g_sink = sink; }

static DWORD WINAPI audio_thread(LPVOID arg)
{
    SDL_AudioStream* s = (SDL_AudioStream*)arg;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    uint64_t done = 0; /* frames pulled so far: paced to the clock, not to Sleep's rounding */
    while (s->running)
    {
        QueryPerformanceCounter(&now);
        uint64_t due = (uint64_t)((now.QuadPart - start.QuadPart) * 48000 / freq.QuadPart);
        while (done + FRAMES <= due)
        {
            s->callback(s->user, s, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
            done += FRAMES;
        }
        Sleep(5);
    }
    return 0;
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
    if (!s || InterlockedExchange(&s->running, 1))
        return true;
    HANDLE t = CreateThread(NULL, 0, audio_thread, s, 0, NULL);
    if (!t)
    {
        s->running = 0;
        return false;
    }
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(t);
    return true;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len)
{
    (void)s;
    UwpAudioSink sink = g_sink;
    if (sink)
        sink((const float*)buf, len / (int)(2 * sizeof(float)));
    return true;
}

/* The addons' sounds (Windower's and Ashita's play_sound): no WAV loads, so none plays and no stream
 * of theirs is made; the game's own audio stream above is never destroyed. */
bool SDL_LoadWAV(const char* path, SDL_AudioSpec* spec, Uint8** buf, Uint32* len)
{
    (void)path, (void)spec, (void)buf, (void)len;
    return false;
}
/* The sign-in screen's sound effects (host/sewave.c) want a device of their own: none here, so the
 * screen is silent and the game's own sounds are unchanged. */
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec)
{
    (void)dev, (void)spec;
    return 0;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev, SDL_AudioSpec* spec, int* frames)
{
    (void)dev, (void)spec, (void)frames;
    return false;
}
SDL_AudioStream* SDL_CreateAudioStream(const SDL_AudioSpec* from, const SDL_AudioSpec* to)
{
    (void)from, (void)to;
    return NULL;
}
bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream* s) { (void)dev, (void)s; return false; }
bool SDL_ClearAudioStream(SDL_AudioStream* s) { (void)s; return true; }
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) { (void)s, (void)gain; return true; }
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) { (void)s; return 0; }
bool SDL_FlushAudioStream(SDL_AudioStream* s) { (void)s; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* s) { (void)s; }
