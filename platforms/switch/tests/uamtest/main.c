/* uamtest: uam (the deko3d shader compiler) as a library on the Switch, against uam.exe on the PC.
 *
 * Every shader listed in romfs:/list.txt ("<name> <stage>", stage 0 vertex, 4 fragment) is compiled from
 * romfs:/glsl/<name>.glsl with uam_compile, twice (a cold and a warm compile, timed), and its DKSH compared byte for
 * byte with romfs:/ref/<name>.dksh, which uam.exe made from the same text. The results go to
 * sdmc:/switch/uamtest/uamtest.log; uam's own messages (stderr) to sdmc:/switch/uamtest/uam-stderr.txt. Then it
 * waits for + to quit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "uam_api.h"

#define DIR "sdmc:/switch/uamtest"

static void* read_all(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n)
        free(b), b = NULL;
    fclose(f);
    if (b)
        b[n] = 0, *size = (size_t)n;
    return b;
}

static double ms_since(u64 t0) { return (double)armTicksToNs(armGetSystemTick() - t0) * 1e-6; }

int main(int argc, char** argv)
{
    (void)argc, (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    romfsInit();
    mkdir(DIR, 0777);
    FILE* log = fopen(DIR "/uamtest.log", "w");
    freopen(DIR "/uam-stderr.txt", "w", stderr);
    if (!log)
        log = stdout;

    size_t ln = 0;
    char* list = (char*)read_all("romfs:/list.txt", &ln);
    int total = 0, same = 0, differ = 0, failed = 0;
    double cold_sum = 0, warm_sum = 0, cold_max = 0;
    fprintf(log, "uamtest: uam as a library on the console, against uam.exe on the PC\n");
    for (char* line = list ? strtok(list, "\n") : NULL; line; line = strtok(NULL, "\n"))
    {
        char name[64];
        int stage = -1;
        if (sscanf(line, "%63s %d", name, &stage) != 2)
            continue;
        char path[160];
        size_t gn = 0, rn = 0;
        snprintf(path, sizeof path, "romfs:/glsl/%s.glsl", name);
        char* glsl = (char*)read_all(path, &gn);
        snprintf(path, sizeof path, "romfs:/ref/%s.dksh", name);
        uint8_t* ref = (uint8_t*)read_all(path, &rn);
        ++total;
        if (!glsl)
        {
            fprintf(log, "%-8s no source\n", name), ++failed;
            continue;
        }
        void* out = NULL;
        size_t on = 0;
        fprintf(stderr, "--- %s\n", name);
        u64 t0 = armGetSystemTick();
        int ok = uam_compile(stage, glsl, &out, &on);
        double cold = ms_since(t0);
        double warm = -1;
        if (ok)
        {
            void* out2 = NULL;
            size_t on2 = 0;
            t0 = armGetSystemTick();
            if (uam_compile(stage, glsl, &out2, &on2))
                warm = ms_since(t0);
            free(out2);
        }
        if (!ok)
            fprintf(log, "%-8s FAILED to compile (%.1f ms; uam-stderr.txt has why)\n", name, cold), ++failed;
        else if (!ref)
            fprintf(log, "%-8s compiled, %zu bytes, %.1f ms cold, %.1f warm; no reference\n", name, on, cold, warm);
        else if (rn == on && !memcmp(ref, out, on))
            fprintf(log, "%-8s same as uam.exe, %zu bytes, %.1f ms cold, %.1f warm\n", name, on, cold, warm), ++same;
        else
        {
            size_t at = 0;
            while (at < on && at < rn && ((uint8_t*)out)[at] == ref[at])
                ++at;
            fprintf(log, "%-8s DIFFERS from uam.exe: %zu bytes against %zu, first difference at byte %zu (%.1f ms)\n",
                name, on, rn, at, cold);
            ++differ;
            snprintf(path, sizeof path, DIR "/%s.dksh", name); /* (ours, to compare on the PC) */
            FILE* o = fopen(path, "wb");
            if (o)
                fwrite(out, 1, on, o), fclose(o);
        }
        if (ok)
        {
            cold_sum += cold, warm_sum += warm > 0 ? warm : 0;
            if (cold > cold_max)
                cold_max = cold;
        }
        fflush(log);
        free(out), free(glsl), free(ref);
    }
    int compiled = total - failed;
    fprintf(log, "SUMMARY %d shaders: %d the same as uam.exe, %d different, %d failed; compile %.1f ms cold on average "
                 "(%.1f at most), %.1f warm\n", total, same, differ, failed, compiled ? cold_sum / compiled : 0.0,
        cold_max, compiled ? warm_sum / compiled : 0.0);
    fclose(log);
    printf("uamtest: %d shaders, %d same as uam.exe, %d different, %d failed.\nLog: " DIR "/uamtest.log\n"
           "Press + to quit.\n", total, same, differ, failed);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    romfsExit();
    consoleExit(NULL);
    return 0;
}
