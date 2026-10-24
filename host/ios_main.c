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

int xi_host_main(int argc, char** argv);

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
    /* no Discord app to talk to on iOS */
    setenv("FFXI_DISCORD", "0", 0);
    args[n] = NULL;
    fprintf(stderr, "[ios] host64 with %d arguments\n", n - 1);
    int ret = xi_host_main(n, args);
    fprintf(stderr, "[ios] host exit %d\n", ret);
    return ret;
}
