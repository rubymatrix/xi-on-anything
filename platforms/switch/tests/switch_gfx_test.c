/* tests/gfx/gfx_test.c on the Switch (build_switch.py gfx-test): the OpenGL back end (gfx_gl.c) through EGL on
 * Mesa's nouveau, as the game will draw. The checks' lines go to sdmc:/switch/xi-on-anything/gfx-test.log
 * (libnx's console can't share the screen with EGL); then gfx_test's frames to the window (--window), and
 * the verdict for three seconds as the whole screen's color: green when every check passed, red when one
 * failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "gfx.h"

#define DIR_ROOT "sdmc:/switch/xi-on-anything"

int gfx_test_main(int argc, char** argv); /* tests/gfx/gfx_test.c's main */

static void verdict(int ok)
{
    GfxTex* bb = gfx_tex_create(GFX_TEX_2D, 22, 1280, 720, 1, GFX_USE_RT);
    if (!bb)
        return;
    uint32_t vp[6] = { 0, 0, 1280, 720, 0, 0x3F800000u };
    gfx_set_targets(bb, 0, 0, NULL);
    for (int frame = 0; frame < 180 && appletMainLoop(); ++frame)
    {
        gfx_clear(0, NULL, 1, ok ? 0xFF20A040u : 0xFFC02020u, 1.0f, 0, vp);
        gfx_present(bb);
    }
    gfx_tex_destroy(bb);
}

int main(int argc, char** argv)
{
    (void)argc;
    mkdir(DIR_ROOT, 0777);
    FILE* f = fopen(DIR_ROOT "/gfx-test.log", "w"); /* emptied, then both streams append to it */
    if (f)
        fclose(f);
    if (freopen(DIR_ROOT "/gfx-test.log", "a", stdout))
        setvbuf(stdout, NULL, _IONBF, 0);
    if (freopen(DIR_ROOT "/gfx-test.log", "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    char* args[] = { argv && argv[0] ? argv[0] : "gfx-test", "--window", NULL };
    int r = gfx_test_main(2, args);
    printf("gfx-test: exit %d\n", r);
    verdict(r == 0);
    return r;
}
