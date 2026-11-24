/* uam (third_party/uam, the deko3d shader compiler) for the deko3d back end: its messages kept, not printed.
 *
 * On the Switch, stdio from a thread other than the main one breaks the main thread's, and uam compiles on a
 * thread of the back end's own. So uam's archive has its calls to the C library's output functions renamed to
 * these (tools/build_switch.py uam_archive: objcopy --redefine-syms=third_party/uam/xi_redefine.txt, uam's objects
 * only): what it writes to stdout or stderr goes into one buffer, which dk_uam_compile hands back; writes to any
 * other file go to the C library as before. uam keeps global state, so a compile at a time (dk_uam_compile's lock). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dk_uam.h"
#include "uam_api.h"

static char* g_log;
static size_t g_len, g_cap;
static int g_capturing;

static void log_add(const char* s, size_t n)
{
    if (!g_capturing || !n)
        return;
    if (g_len + n + 1 > g_cap)
    {
        size_t cap = (g_len + n + 1) * 2 + 256;
        char* p = (char*)realloc(g_log, cap);
        if (!p)
            return;
        g_log = p, g_cap = cap;
    }
    memcpy(g_log + g_len, s, n);
    g_len += n;
    g_log[g_len] = 0;
}

static int is_std(FILE* f) { return f == stdout || f == stderr; }

int uam_vfprintf(FILE* f, const char* fmt, va_list ap)
{
    if (!is_std(f))
        return vfprintf(f, fmt, ap);
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0)
        log_add(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}

int uam_fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = uam_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int uam_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = uam_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int uam_fputs(const char* s, FILE* f)
{
    if (!is_std(f))
        return fputs(s, f);
    log_add(s, strlen(s));
    return 0;
}

int uam_puts(const char* s)
{
    log_add(s, strlen(s));
    log_add("\n", 1);
    return 0;
}

size_t uam_fwrite(const void* p, size_t size, size_t n, FILE* f)
{
    if (!is_std(f))
        return fwrite(p, size, n, f);
    log_add((const char*)p, size * n);
    return n;
}

int uam_fputc(int c, FILE* f)
{
    if (!is_std(f))
        return fputc(c, f);
    char ch = (char)c;
    log_add(&ch, 1);
    return c;
}

int uam_putc(int c, FILE* f) { return uam_fputc(c, f); }

int uam_putchar(int c) { return uam_fputc(c, stdout); }

int uam_fflush(FILE* f) { return is_std(f) || !f ? 0 : fflush(f); }

int dk_uam_compile(int vertex, const char* glsl, void** dksh, size_t* size, char** log)
{
    g_len = 0;
    if (g_log)
        g_log[0] = 0;
    g_capturing = 1;
    int ok = uam_compile(vertex ? UAM_VERTEX : UAM_FRAGMENT, glsl, dksh, size);
    g_capturing = 0;
    if (log)
        *log = g_len ? strdup(g_log) : NULL;
    return ok;
}
