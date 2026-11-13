/* SDL entry point of the iPhone app (docs/ios.md). SDL_main.h turns main() into SDL_main and runs it
 * inside UIApplicationMain; this adds the arguments in Documents/run.args, finds the game folder the
 * player copied into Documents, sends the log to Documents/host64.log, then starts the portable host
 * (host64.c, built with main renamed to xi_host_main). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "appdefaults.h"
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

int xi_host_main(int argc, char** argv);
void touch_init(void); /* touch.c */

#define MAX_ARGS 64

/* Documents/run.args: one literal argument per line (no shell expansion), as on Android. */
static int read_args(const char* path, char** out, int room)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    int n = 0;
    char line[2048];
    while (n < room && fgets(line, sizeof line, f))
    {
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if (len)
            out[n++] = strdup(line);
    }
    fclose(f);
    return n;
}

/* The picture behind the game's black bars (gfx_set_wallpaper): Documents/wallpaper.png or .jpg (the
 * player's own, through the Files app), else the one built in (build_ios.py --wallpaper). None in
 * the source: the player chooses it. */
static void wallpaper(const char* docs)
{
    static const char* const NAMES[] = { "wallpaper.png", "wallpaper.jpg", "wallpaper.jpeg" };
    char path[1100];
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; ++i)
        {
            if (pass == 0)
                snprintf(path, sizeof path, "%s/%s", docs, NAMES[i]);
            else if (!app_resource(NAMES[i], path, sizeof path))
                continue;
            int w, h, n;
            unsigned char* px = stbi_load(path, &w, &h, &n, 4);
            if (!px)
                continue;
            gfx_set_wallpaper(px, w, h);
            stbi_image_free(px);
            fprintf(stderr, "[ios] wallpaper %s (%dx%d)\n", path, w, h);
            return;
        }
}

static int has_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], name))
            return 1;
    return 0;
}

int main(int argc, char** argv)
{
    const char* home = getenv("HOME"); /* the app's container */
    char docs[1024], path[1100];
    snprintf(docs, sizeof docs, "%s/Documents", home ? home : ".");
    mkdir(docs, 0755);
    /* launched from the home screen there is no terminal; from simctl --console-pty there is */
    if (!isatty(2))
    {
        snprintf(path, sizeof path, "%s/host64.log", docs);
        if (freopen(path, "w", stderr))
            setvbuf(stderr, NULL, _IOLBF, 0);
        freopen(path, "a", stdout);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    static char* args[MAX_ARGS];
    int n = 0;
    for (int i = 0; i < argc && n < MAX_ARGS; ++i)
        args[n++] = argv[i];
    snprintf(path, sizeof path, "%s/run.args", docs);
    n += read_args(path, args + n, MAX_ARGS - 2 - n);
    static char game[1100];
    struct stat st;
    snprintf(game, sizeof game, "%s/FINAL FANTASY XI", docs);
    if (!has_arg(n, args, "--game") && stat(game, &st) == 0 && S_ISDIR(st.st_mode))
    {
        args[n++] = "--game";
        args[n++] = game;
    }
    if (!has_arg(n, args, "--game"))
        fprintf(stderr, "[ios] no game folder: copy \"FINAL FANTASY XI\" into the app's Documents (Files app)\n");
    touch_init();
    wallpaper(docs);
    /* no Discord app to talk to on iOS */
    setenv("FFXI_DISCORD", "0", 0);
    /* the scene effects and the frame-rate gauge off by default: Config > Modern still turns them on */
    setenv("FFXI_FX", "0", 0);
    setenv("FFXI_FPS", "0", 0);
    args[n] = NULL;
    fprintf(stderr, "[ios] host64 with %d arguments\n", n - 1);
    int ret = xi_host_main(n, args);
    fprintf(stderr, "[ios] host exit %d\n", ret);
    /* the game ended (Shutdown, or Exit at its main menu): the app too. Returning from SDL_main
     * leaves an iOS app running, its window black. */
    fflush(NULL);
    exit(ret);
}
