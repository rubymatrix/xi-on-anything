/* SDL entry point of the Android test app (docs/android.md): Android defaults, the --android-*
 * options, then the portable host. Saved sign-in is disabled; the login comes from the arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "build.h"
#if defined(FFXI_RENDER_WORKER_BUILD)
#include "gfx_worker.h"
#else
static void gfx_worker_shutdown(void) {}
#endif
#ifdef main
#undef main
#endif
int xi_host_main(int argc, char** argv);
int xi_gfx_test_main(int argc, char** argv);
int xi_format_test_main(void);
int xi_state_test_main(void);
int xi_async_test_main(void);
int xi_area_test_main(void);
int xi_pass_plan_test_main(void);
int xi_visibility_storage_test_main(void);

enum AndroidOptionType
{
    OPTION_BOOLEAN,
    OPTION_TEXT,
    OPTION_WORKER,
    OPTION_GEOMETRY,
    OPTION_CONTROL,
};

static const struct AndroidOption
{
    const char* argument;
    const char* environment;
    enum AndroidOptionType type;
} android_options[] = {
    {"--android-control", "FFXI_CONTROL", OPTION_CONTROL},
    {"--android-render-worker", "FFXI_RENDER_WORKER", OPTION_WORKER},
    {"--android-worker-stats", "FFXI_RENDER_WORKER_DIAGNOSTICS", OPTION_WORKER},
    {"--android-worker-mailbox", "FFXI_RENDER_WORKER_MAILBOX", OPTION_WORKER},
    {"--android-worker-const-fx", "FFXI_RENDER_WORKER_CONST_FX", OPTION_WORKER},
    {"--android-visibility-storage", "FFXI_ANDROID_VISIBILITY_STORAGE", OPTION_BOOLEAN},
    {"--android-visibility-storage-stats", "FFXI_ANDROID_VISIBILITY_STORAGE_DIAGNOSTICS", OPTION_BOOLEAN},
    {"--android-native-geometry", "FFXI_ANDROID_NATIVE_GEOMETRY", OPTION_GEOMETRY},
    {"--android-encode-cache", "FFXI_ANDROID_ENCODE_CACHE", OPTION_BOOLEAN},
    {"--android-pass-plan", "FFXI_ANDROID_PASS_PLAN", OPTION_BOOLEAN},
    {"--android-pass-trace", "FFXI_ANDROID_PASS_TRACE", OPTION_TEXT},
    {"--android-probe-query-diag", "FFXI_ANDROID_PROBE_QUERY_DIAG", OPTION_TEXT},
    {"--android-bench-dir", "FFXI_BENCH_DIR", OPTION_TEXT},
    {"--android-addons", "FFXI_ADDONS", OPTION_TEXT},
    {"--android-readback", "FFXI_ASYNC_READBACK", OPTION_BOOLEAN},
    {"--android-fast-sync", "FFXI_ANDROID_FAST_SYNC", OPTION_BOOLEAN},
    {"--android-trim-uniforms", "FFXI_ANDROID_TRIM_UNIFORMS", OPTION_BOOLEAN},
    {"--android-dont-care-loads", "FFXI_ANDROID_DONT_CARE_LOADS", OPTION_BOOLEAN},
    {"--android-cache-sampled", "FFXI_ANDROID_CACHE_SAMPLED", OPTION_BOOLEAN},
    {"--android-bounded-area", "FFXI_ANDROID_BOUNDED_AREA", OPTION_BOOLEAN},
};

static int android_option_valid(enum AndroidOptionType type, const char* value)
{
    if (type == OPTION_TEXT)
        return 1;
    /* Upstream control.lua binds only loopback; adb forwards this port. */
    if (type == OPTION_CONTROL)
        return !strcmp(value, "0") || !strcmp(value, "54300");
    if (strcmp(value, "0") && strcmp(value, "1"))
        return 0;
#if !defined(FFXI_RENDER_WORKER_BUILD)
    if (type == OPTION_WORKER && !strcmp(value, "1"))
    {
        fprintf(stderr, "[android] this build has no render worker\n");
        return 0;
    }
#endif
#if FFXI_GEOMETRY_LAYOUT != 1
    if (type == OPTION_GEOMETRY && !strcmp(value, "1"))
    {
        fprintf(stderr, "[android] no verified geometry adapter for this client\n");
        return 0;
    }
#endif
    return 1;
}

/* the offscreen graphics fixtures linked into libmain.so (tools/build_android.py) */
static const struct AndroidTest
{
    const char* argument;
    int (*run)(void);
} android_tests[] = {
    {"--android-format-test", xi_format_test_main},
    {"--android-state-test", xi_state_test_main},
    {"--android-async-test", xi_async_test_main},
    {"--android-area-test", xi_area_test_main},
    {"--android-pass-plan-test", xi_pass_plan_test_main},
    {"--android-visibility-storage-test", xi_visibility_storage_test_main},
};

static const struct AndroidTest* android_test(const char* argument)
{
    for (size_t i = 0; i < sizeof android_tests / sizeof android_tests[0]; ++i)
        if (!strcmp(argument, android_tests[i].argument))
            return &android_tests[i];
    return NULL;
}

__attribute__((visibility("default"))) int SDL_main(int argc, char** argv)
{
    const char* dir = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--data-dir"))
            dir = argv[i + 1];
    if (!dir)
        return 2;
    mkdir(dir, 0700);
    char log[2048];
    snprintf(log, sizeof log, "%s/host64.log", dir);
    freopen(log, "a", stderr);
    freopen(log, "a", stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Keep optional shader/effect files inside this explicit app data directory. */
    char cache_dir[2048], fx_file[2048];
    if (snprintf(cache_dir, sizeof cache_dir, "%s/cache", dir) >= (int)sizeof cache_dir ||
        snprintf(fx_file, sizeof fx_file, "%s/android-fx.txt", dir) >= (int)sizeof fx_file)
        return 2;
    mkdir(cache_dir, 0700);
    setenv("FFXI_CACHE_DIR", cache_dir, 1);
    setenv("FFXI_FX_FILE", fx_file, 1);
    fprintf(stderr, "[android] renderer=android-cpp; optional keyed readback and NEON geometry\n");
    /* defaults: every optional path off; the --android-* options below override them */
    setenv("FFXI_ADDONS", "0", 1);
    setenv("FFXI_PROFILE", "0", 1);
    setenv("FFXI_FX", "0", 1);
    setenv("FFXI_DISCORD", "0", 1);
    setenv("FFXI_VSYNC", "0", 1);
    setenv("FFXI_CONTROL", "0", 1);
    /* the occlusion probe as the occlusion setting says (gfx_fx.c; d3d8.c lock_rect), not forced */
    unsetenv("FFXI_PROBE");
    setenv("FFXI_ASYNC_READBACK", "0", 1);
    setenv("FFXI_FPS", "0", 1);
    setenv("FFXI_RENDER_WORKER", "0", 1);
    setenv("FFXI_RENDER_WORKER_DIAGNOSTICS", "0", 1);
    setenv("FFXI_RENDER_WORKER_MAILBOX", "0", 1);
    setenv("FFXI_RENDER_WORKER_CONST_FX", "0", 1);
    setenv("FFXI_ANDROID_FAST_SYNC", "0", 1);
    setenv("FFXI_ANDROID_TRIM_UNIFORMS", "0", 1);
    setenv("FFXI_ANDROID_DONT_CARE_LOADS", "0", 1);
    setenv("FFXI_ANDROID_CACHE_SAMPLED", "0", 1);
    setenv("FFXI_ANDROID_BOUNDED_AREA", "0", 1);
    setenv("FFXI_ANDROID_PASS_PLAN", "0", 1);
    setenv("FFXI_ANDROID_VISIBILITY_STORAGE", "0", 1);
    setenv("FFXI_ANDROID_VISIBILITY_STORAGE_DIAGNOSTICS", "0", 1);
    unsetenv("FFXI_ANDROID_PASS_TRACE");
    unsetenv("FFXI_ANDROID_PROBE_QUERY_DIAG");
    setenv("FFXI_ANDROID_ENCODE_CACHE", "1", 1);
    /* the shared kernels (geometry_guest.c) where this client's layout is verified: the original's SSE results */
    setenv("FFXI_ANDROID_NATIVE_GEOMETRY", FFXI_GEOMETRY_LAYOUT == 1 ? "1" : "0", 1);
    setenv("FFXI_NATIVE_GEOMETRY", "0", 1);
    /* Android-only control options are consumed before the portable host sees argv. */
    for (int i = 1; i + 1 < argc;)
    {
        const struct AndroidOption* option = NULL;
        for (size_t j = 0; j < sizeof android_options / sizeof android_options[0]; ++j)
            if (!strcmp(argv[i], android_options[j].argument))
            {
                option = &android_options[j];
                break;
            }
        if (!option)
        {
            ++i;
            continue;
        }
        if (!android_option_valid(option->type, argv[i + 1]))
        {
            fprintf(stderr, "[android] invalid or unavailable value for %s\n", option->argument);
            return 4;
        }
        setenv(option->environment, argv[i + 1], 1);
        memmove(argv + i, argv + i + 2, (argc - i - 1) * sizeof *argv);
        argc -= 2;
    }
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--android-", 10) && strcmp(argv[i], "--android-gfx-test") && !android_test(argv[i]))
        {
            fprintf(stderr, "[android] unknown option or missing value: %s\n", argv[i]);
            return 4;
        }
    if ((!strcmp(getenv("FFXI_RENDER_WORKER_MAILBOX"), "1") || !strcmp(getenv("FFXI_RENDER_WORKER_CONST_FX"), "1")) &&
        strcmp(getenv("FFXI_RENDER_WORKER"), "1"))
    {
        fprintf(stderr, "[android] worker optimizations require render worker=1\n");
        return 4;
    }
    if (!strcmp(getenv("FFXI_ANDROID_VISIBILITY_STORAGE_DIAGNOSTICS"), "1") &&
        strcmp(getenv("FFXI_ANDROID_VISIBILITY_STORAGE"), "1"))
        return 4;
    if (!strcmp(getenv("FFXI_ANDROID_VISIBILITY_STORAGE"), "1") &&
        (strcmp(getenv("FFXI_ASYNC_READBACK"), "1") || !strcmp(getenv("FFXI_RENDER_WORKER"), "1") ||
         !strcmp(getenv("FFXI_ANDROID_PASS_PLAN"), "1") || !strcmp(getenv("FFXI_ANDROID_BOUNDED_AREA"), "1") ||
         getenv("FFXI_ANDROID_PASS_TRACE") || getenv("FFXI_ANDROID_PROBE_QUERY_DIAG")))
    {
        fprintf(
            stderr,
            "[android] visibility storage requires readback=1, worker=0, planner=0, bounded-area=0, trace/querydiag off\n");
        return 4;
    }
    /* Physical keyboard/controller baseline: do not open IME at every game window. */
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    SDL_SetMainReady();
    for (int i = 1; i < argc; i++)
    {
        const struct AndroidTest* test = android_test(argv[i]);
        if (test)
        {
            int result = test->run();
            fprintf(stderr, "[android] graphics gate exit %d (%s)\n", result, argv[i]);
            gfx_worker_shutdown();
            return result;
        }
    }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--android-gfx-test"))
        {
            char* args[] = {"gfx_test", "--window", NULL};
            int result = xi_gfx_test_main(2, args);
            fprintf(stderr, "[android] graphics gate exit %d\n", result);
            gfx_worker_shutdown();
            return result;
        }
    if (!strcmp(getenv("FFXI_RENDER_WORKER_MAILBOX"), "1") && strcmp(getenv("FFXI_ASYNC_READBACK"), "1"))
    {
        fprintf(stderr, "[android] game worker mailbox requires async readback=1\n");
        return 4;
    }
    /* a LandSandBoat account given in run.args: the sign-in screen and saved sessions are untried here */
    int user = 0, password = 0;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "--user") && argv[i + 1][0])
            user = 1;
        if (!strcmp(argv[i], "--pass") && argv[i + 1][0])
            password = 1;
        if (!strcmp(argv[i], "--session") || !strcmp(argv[i], "--auth"))
        {
            fprintf(stderr, "[android] sign in with --user and --pass\n");
            return 3;
        }
    }
    if (!user || !password)
    {
        fprintf(stderr, "[android] supply --user and --pass in private run.args\n");
        return 3;
    }
    fprintf(
        stderr,
        "[android] render policy fast-sync=%s trim-uniforms=%s dont-care-loads=%s cache-sampled=%s bounded-area=%s\n",
        getenv("FFXI_ANDROID_FAST_SYNC"), getenv("FFXI_ANDROID_TRIM_UNIFORMS"), getenv("FFXI_ANDROID_DONT_CARE_LOADS"),
        getenv("FFXI_ANDROID_CACHE_SAMPLED"), getenv("FFXI_ANDROID_BOUNDED_AREA"));
    setenv("FFXI_NATIVE_GEOMETRY", getenv("FFXI_ANDROID_NATIVE_GEOMETRY"), 1);
    fprintf(stderr, "[android] native ARM64 entry; async-readback=%s\n", getenv("FFXI_ASYNC_READBACK"));
    int ret = xi_host_main(argc, argv);
    fprintf(stderr, "[android] host exit %d\n", ret);
    gfx_worker_shutdown();
    return ret;
}
