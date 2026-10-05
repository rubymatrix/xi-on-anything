/* The SDL3 calls the runtime makes (user32.c, input.c, dsound.c), for the browser build, where SDL3
 * is not linked: the port of sdl_uwp.c (the Xbox's) to POSIX threads. There is one window, the
 * page's canvas; its input arrives through web_bridge.h from the shell (tools/web/) and leaves here
 * as SDL events. Audio is a thread that pulls the mix every 10 ms, as SDL's device would - dsound.c
 * moves the DirectSound cursors, and signals the game's notification events, from that callback -
 * and hands it to the shell. Compiled against SDL3's headers for its types. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>

#include <SDL3/SDL.h>

#include "web_bridge.h"

typedef pthread_mutex_t Lock;
#define LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)

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

/* --- odds and ends ------------------------------------------------------------------------------- */
const char* SDL_GetError(void) { return "not available in the browser build"; }
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

bool SDL_PollEvent(SDL_Event* e)
{
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

/* --- the window ------------------------------------------------------------------------------------ */
#define WINDOW_ID 1
struct SDL_Window
{
    int w, h;
};
static SDL_Window g_window;
static volatile int g_view_w = 1920, g_view_h = 1080;
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
bool SDL_StartTextInput(SDL_Window* w) { (void)w; return true; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* w) { (void)w; return 0; }
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* default_value)
{
    (void)props, (void)name;
    return default_value;
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

EMSCRIPTEN_KEEPALIVE void web_view_size(int w, int h)
{
    if (w > 0 && h > 0)
        __atomic_store_n(&g_view_w, w, __ATOMIC_SEQ_CST), __atomic_store_n(&g_view_h, h, __ATOMIC_SEQ_CST);
}

/* --- keyboard and mouse ---------------------------------------------------------------------------- */
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

bool SDL_OpenURL(const char* url) { (void)url; return false; } /* the shell opens links (later) */

EMSCRIPTEN_KEEPALIVE void web_key(int scancode, int down, int repeat)
{
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
        g_keystate[scancode] = down != 0;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = WINDOW_ID;
    e.key.scancode = (SDL_Scancode)scancode;
    e.key.down = down != 0;
    e.key.repeat = repeat != 0;
    push(&e);
}

EMSCRIPTEN_KEEPALIVE void web_text(const char* utf8)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = WINDOW_ID;
    e.text.text = utf8; /* copied by push */
    push(&e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_move(float x, float y)
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

EMSCRIPTEN_KEEPALIVE void web_mouse_button(int button, int down)
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

EMSCRIPTEN_KEEPALIVE void web_mouse_wheel(float dy)
{
    lock(&g_mlock);
    float x = g_mx, y = g_my;
    unlock(&g_mlock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = WINDOW_ID;
    e.wheel.y = dy;
    e.wheel.integer_y = (Sint32)dy;
    e.wheel.mouse_x = x, e.wheel.mouse_y = y;
    push(&e);
}

EMSCRIPTEN_KEEPALIVE void web_focus(int on)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = on ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = WINDOW_ID;
    push(&e);
}

EMSCRIPTEN_KEEPALIVE void web_close(void)
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
bool SDL_WarpMouseGlobal(float x, float y) { (void)x, (void)y; return false; } /* browsers can't move the cursor */
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

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
    uint32_t buttons;
    int16_t axes[6];
} Pad;
static Pad g_padstate;

EMSCRIPTEN_KEEPALIVE void web_gamepad(int connected, uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
    int16_t lt, int16_t rt)
{
    Pad p = { connected, buttons, { lx, ly, rx, ry, lt, rt } };
    lock(&g_plock);
    int was = g_padstate.connected;
    g_padstate = p;
    unlock(&g_plock);
    if (!was != !connected)
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
        e.gdevice.which = PAD_ID;
        push(&e);
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
    (void)p, (void)low, (void)high, (void)ms; /* the shell's vibrationActuator (later) */
    return true;
}

/* --- audio ----------------------------------------------------------------------------------------- */
#define FRAMES 480 /* 10 ms at 48 kHz: dsound.c's cursors move in steps this size */
struct SDL_AudioStream
{
    SDL_AudioStreamCallback callback;
    void* user;
    volatile int running;
};
static SDL_AudioStream g_stream;
static WebAudioSink g_sink;

void web_set_audio_sink(WebAudioSink sink) { g_sink = sink; }

/* The page's audio: a ring of interleaved stereo floats in the shared memory that the page's AudioWorklet
 * (tools/web/audio-worklet.js) reads straight from - no thread of the page's touches the samples.
 * Header: write index, read index (frames, each only moved by its side), size in frames. */
#define RING_FRAMES 16384 /* about 340 ms */
#define RING_TARGET 4800  /* frames ahead of the worklet the mix keeps at most: 100 ms */
typedef struct AudioRing
{
    volatile int32_t write, read, size, pad;
    float data[RING_FRAMES * 2];
} AudioRing;
static AudioRing* g_ring;

EMSCRIPTEN_KEEPALIVE AudioRing* web_audio_ring(void)
{
    if (!g_ring)
    {
        g_ring = (AudioRing*)calloc(1, sizeof *g_ring);
        g_ring->size = RING_FRAMES;
    }
    return g_ring;
}

static void ring_write(const float* f, int n)
{
    AudioRing* r = g_ring;
    if (!r)
        return;
    int32_t w = __atomic_load_n(&r->write, __ATOMIC_ACQUIRE), rd = __atomic_load_n(&r->read, __ATOMIC_ACQUIRE);
    int32_t ahead = (w - rd + RING_FRAMES) % RING_FRAMES;
    if (ahead + n > RING_TARGET) /* the worklet fell behind (a hidden page, a hitch): drop, so the sound
                                  * stays no more than RING_TARGET behind the game */
        return;
    for (int i = 0; i < n; ++i)
    {
        int32_t at = (w + i) % RING_FRAMES;
        r->data[2 * at] = f[2 * i], r->data[2 * at + 1] = f[2 * i + 1];
    }
    __atomic_store_n(&r->write, (w + n) % RING_FRAMES, __ATOMIC_RELEASE);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void* audio_thread(void* arg)
{
    SDL_AudioStream* s = (SDL_AudioStream*)arg;
    uint64_t start = now_ns(), done = 0; /* frames pulled so far: paced to the clock, not to sleep's rounding */
    while (s->running)
    {
        uint64_t due = (now_ns() - start) * 48000 / 1000000000u;
        while (done + FRAMES <= due)
        {
            s->callback(s->user, s, FRAMES * 2 * (int)sizeof(float), FRAMES * 2 * (int)sizeof(float));
            done += FRAMES;
        }
        usleep(5000);
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

bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len)
{
    (void)s;
    WebAudioSink sink = g_sink;
    if (sink)
        sink((const float*)buf, len / (int)(2 * sizeof(float)));
    ring_write((const float*)buf, len / (int)(2 * sizeof(float)));
    return true;
}

/* The addons' sounds (Windower's and Ashita's play_sound): no WAV loads, so none plays and no stream
 * of theirs is made; the game's own audio stream above is never destroyed. */
bool SDL_LoadWAV(const char* path, SDL_AudioSpec* spec, Uint8** buf, Uint32* len)
{
    (void)path, (void)spec, (void)buf, (void)len;
    return false;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) { (void)s; return 0; }
bool SDL_FlushAudioStream(SDL_AudioStream* s) { (void)s; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream* s) { (void)s; }

/* --- the rest host64 and its screens use ------------------------------------------------------------- */
/* The data folder: FFXI_DATA_DIR, else xi-data/ in the working folder (Node), else /xi-data/ (the page's
 * file system, kept by the shell). host64's --data-dir wins over all of these. */
char* SDL_GetPrefPath(const char* org, const char* app)
{
    (void)org, (void)app;
    const char* d = getenv("FFXI_DATA_DIR");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s/", d && *d ? d : "xi-data");
    mkdir(d && *d ? d : "xi-data", 0755);
    return strdup(buf);
}
bool SDL_CreateDirectory(const char* path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char* c = p + 1; *c; ++c)
        if (*c == '/')
        {
            *c = 0;
            mkdir(p, 0755);
            *c = '/';
        }
    return mkdir(p, 0755) == 0 || errno == EEXIST;
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
int SDL_GetAtomicInt(SDL_AtomicInt* a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
int SDL_SetAtomicInt(SDL_AtomicInt* a, int v) { return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST); }

float SDL_GetWindowPixelDensity(SDL_Window* w) { (void)w; return 1.0f; } /* the shell sizes in device pixels */
bool SDL_SetWindowFullscreenMode(SDL_Window* w, const SDL_DisplayMode* m) { (void)w, (void)m; return true; }
bool SDL_SetWindowBordered(SDL_Window* w, bool on) { (void)w, (void)on; return true; }
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
/* the one mode: the canvas (Config's resolutions list it, and the sizes the game asks for are drawn
 * scaled to it) */
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

/* The sign-in screen's sounds (host/sewave.c) want a device of their own: none here (the shell's
 * audio comes through the game's stream above), so the sign-in is silent. */
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

/* --- once a frame, on the game's thread (host64's Present hook) ------------------------------------------ */
/* XI_CPU_PROF_AT=<seconds>: the game thread's profile, from its first frame for that long (tools/web/node_pre.js) */
EM_JS(void, web_prof_start, (void), { if (globalThis.xiProfStart) globalThis.xiProfStart(); });
EM_JS(void, web_prof_stop, (void), { if (globalThis.xiProfStop) globalThis.xiProfStop('game'); });

void web_frame(void)
{
    static double stop_at = -1;
    if (stop_at < 0)
    {
        const char* at = getenv("XI_CPU_PROF_AT");
        stop_at = at ? emscripten_get_now() + atof(at) * 1000.0 : 0;
        if (stop_at)
            web_prof_start();
    }
    if (stop_at > 0 && emscripten_get_now() >= stop_at)
    {
        stop_at = 0;
        web_prof_stop();
    }
}
