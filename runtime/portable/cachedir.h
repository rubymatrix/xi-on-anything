/* The host's cache folder for its own files (fx.txt, pipelines, traces): FFXI_CACHE_DIR, else
 * ~/Library/Caches/FFXI on macOS, $XDG_CACHE_HOME/FFXI (~/.cache/FFXI) on Linux and
 * %LOCALAPPDATA%\FFXI\Cache on Windows. Made if missing; no trailing separator. 0 when there is none.
 * A header so the graphics back end and its tests need nothing else. */
#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#define cachedir_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define cachedir_mkdir(p) mkdir(p, 0755)
#endif

static inline int cache_dir(char* out, size_t n)
{
    const char* env = getenv("FFXI_CACHE_DIR");
    if (env && *env)
        snprintf(out, n, "%s", env);
    else
    {
#if defined(_WIN32)
        const char* base = getenv("LOCALAPPDATA");
        if (!base || !*base)
            return 0;
        snprintf(out, n, "%s\\FFXI", base);
        cachedir_mkdir(out);
        snprintf(out, n, "%s\\FFXI\\Cache", base);
#elif defined(__APPLE__)
        const char* home = getenv("HOME");
        if (!home || !*home)
            return 0;
        snprintf(out, n, "%s/Library/Caches/FFXI", home);
#else
        const char* xdg = getenv("XDG_CACHE_HOME");
        const char* home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(out, n, "%s/FFXI", xdg);
        else if (home && *home)
        {
            snprintf(out, n, "%s/.cache", home);
            cachedir_mkdir(out);
            snprintf(out, n, "%s/.cache/FFXI", home);
        }
        else
            return 0;
#endif
    }
    cachedir_mkdir(out);
    return 1;
}
