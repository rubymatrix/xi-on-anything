/* The touch controls and the wallpaper on Android (docs/android.md): drawn by a view over the game's
 * surface (MainActivity.Overlay), not into the frame, so they keep the display's rate whatever the
 * game's, and labels come in the system font. gfx_set_touch_shapes and gfx_set_wallpaper (gfx.h) are
 * these here, for host/touch.c and android_mobile_init; the C++ Vulkan back end only presents into
 * the rectangle display_fit.c chooses (gfx_set_present_rect), which the view is told too, for the
 * bars beside it.
 *
 * Each call goes to the activity through JNI, from whichever thread makes it (SDL attaches it): the
 * view copies what it is given and redraws on its own thread. The same shapes twice are sent once. */
#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "gfx.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

void android_overlay_present_rect(int x, int y, int w, int h);
void android_mobile_init(void);
void touch_init(void); /* touch.c */

static SDL_Mutex* g_lock;
static GfxTouchShape g_last[GFX_TOUCH_MAX];
static int g_nlast = -1;

static SDL_Mutex* lock(void)
{
    static SDL_InitState init;
    if (SDL_ShouldInit(&init))
    {
        g_lock = SDL_CreateMutex();
        SDL_SetInitialized(&init, true);
    }
    return g_lock;
}

/* The activity's method (name, signature) called with the arguments after it; 0 when it is not there */
static int call(const char* name, const char* sig, ...)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!activity)
        return 0;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, cls, name, sig);
    if (m)
    {
        va_list ap;
        va_start(ap, sig);
        (*env)->CallVoidMethodV(env, activity, m, ap);
        va_end(ap);
    }
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return m != NULL;
}

/* Eight numbers a shape (x, y, hw, hh in the surface's pixels, kind, sym, pressed, alpha), and the
 * labels a line each */
void gfx_set_touch_shapes(const GfxTouchShape* shapes, int n)
{
    n = n < 0 ? 0 : n > GFX_TOUCH_MAX ? GFX_TOUCH_MAX : n;
    SDL_LockMutex(lock());
    if (n == g_nlast && !memcmp(shapes, g_last, sizeof *shapes * (size_t)n))
    {
        SDL_UnlockMutex(g_lock);
        return;
    }
    memcpy(g_last, shapes, sizeof *shapes * (size_t)n);
    g_nlast = n;
    float v[GFX_TOUCH_MAX * 8];
    char labels[GFX_TOUCH_MAX * 9 + 1] = "";
    for (int i = 0; i < n; ++i)
    {
        const GfxTouchShape* s = &shapes[i];
        float* o = &v[i * 8];
        o[0] = s->x, o[1] = s->y, o[2] = s->hw, o[3] = s->hh;
        o[4] = (float)s->kind, o[5] = (float)s->sym, o[6] = (float)s->pressed, o[7] = s->alpha;
        char label[sizeof s->label + 1];
        snprintf(label, sizeof label, "%.*s", (int)sizeof s->label, s->label);
        strcat(labels, label);
        strcat(labels, "\n");
    }
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (env)
    {
        jfloatArray a = (*env)->NewFloatArray(env, n * 8);
        jstring l = (*env)->NewStringUTF(env, labels);
        if (a && l)
        {
            (*env)->SetFloatArrayRegion(env, a, 0, n * 8, v);
            call("setTouchShapes", "([FLjava/lang/String;)V", a, l);
        }
        if (a)
            (*env)->DeleteLocalRef(env, a);
        if (l)
            (*env)->DeleteLocalRef(env, l);
    }
    SDL_UnlockMutex(g_lock);
}

/* The picture behind the bars beside the game (RGBA8, copied), dimmed by the view; NULL for none */
void gfx_set_wallpaper(const uint8_t* rgba, int w, int h)
{
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    if (!env)
        return;
    if (!rgba || w <= 0 || h <= 0)
    {
        call("setWallpaper", "([III)V", (jintArray)NULL, 0, 0);
        return;
    }
    jintArray a = (*env)->NewIntArray(env, w * h);
    jint* px = a ? (*env)->GetIntArrayElements(env, a, NULL) : NULL;
    if (!px)
    {
        if (a)
            (*env)->DeleteLocalRef(env, a);
        return;
    }
    for (int i = 0; i < w * h; ++i) /* Android's ARGB */
    {
        const uint8_t* p = rgba + (size_t)i * 4;
        px[i] = (jint)((uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
    }
    (*env)->ReleaseIntArrayElements(env, a, px, 0);
    call("setWallpaper", "([III)V", a, (jint)w, (jint)h);
    (*env)->DeleteLocalRef(env, a);
}

/* Where the game is presented (display_fit.c), in the surface's pixels */
void android_overlay_present_rect(int x, int y, int w, int h)
{
    call("setPresentRect", "(IIII)V", (jint)x, (jint)y, (jint)w, (jint)h);
}

/* Before the host starts (android_main.c): the touch controls, and the player's wallpaper.png or .jpg
 * from the app's files folder (beside run.args), as on iOS; none in the APK */
void android_mobile_init(void)
{
    static const char* const NAMES[] = { "wallpaper.png", "wallpaper.jpg", "wallpaper.jpeg" };
    touch_init();
    const char* files = SDL_GetAndroidExternalStoragePath();
    for (size_t i = 0; files && i < sizeof NAMES / sizeof *NAMES; ++i)
    {
        char path[1100];
        int w, h, n;
        snprintf(path, sizeof path, "%s/%s", files, NAMES[i]);
        unsigned char* px = stbi_load(path, &w, &h, &n, 4);
        if (!px)
            continue;
        gfx_set_wallpaper(px, w, h);
        stbi_image_free(px);
        fprintf(stderr, "[android] wallpaper %s (%dx%d)\n", path, w, h);
        break;
    }
}
